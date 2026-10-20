/* Relay tunnel client.
 *
 * Built into the server, so a host runs nothing extra: enable it in
 * Config.json and the server dials out to the directory, gets a ticket and the
 * relay list for its region, then keeps a tunnel open to that relay. Players
 * reach the public relay port with a stock game client.
 *
 * Each player session gets its own loopback socket that forwards to the real
 * game port, so ENet sees an ordinary peer and the rest of the server is
 * untouched. The address reported for that peer is the synthetic one supplied
 * by the relay, which is what bans, operators and logs then key on.
 */
#include <Relay.h>
#include <RelayProto.h>
#include <Config.h>
#include <Log.h>
#include <io/Socket.h>
#include <io/Threads.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  typedef SOCKET sock_t;
  #define BADSOCK INVALID_SOCKET
#else
  typedef int sock_t;
  #define BADSOCK (-1)
#endif

#define RELAY_MAX_SESSIONS  512
#define RELAY_KEEPALIVE_SEC 15
#define RELAY_RETRY_SEC     10

typedef struct
{
    bool     used;
    uint32_t id;            /* session id, as the relay knows it     */
    uint8_t  lobby;
    sock_t   local;         /* loopback socket toward the game port  */
    uint16_t local_port;    /* what ENet will see as the peer port   */
    char     synthetic[24]; /* what the server records as the ip     */
    time_t   last_seen;
} TunnelSession;

static TunnelSession g_sessions[RELAY_MAX_SESSIONS];
static Mutex         g_sessions_lock;
static bool          g_enabled = false;
static bool          g_online  = false;
static sock_t        g_tunnel  = BADSOCK;
static struct sockaddr_in g_relay_addr;
static uint32_t      g_base_port = 0;
static uint8_t       g_port_count = 0;
static RelayCrypto   g_crypto;
static time_t        g_last_rx = 0;
/* The relay has echoed a keepalive on this tunnel. Older relays never answer
   one, and with no players on they send nothing at all, so silence only
   means a dead tunnel once we know this relay does reply. */
static bool          g_relay_echoes = false;

#define RELAY_DEAD_SEC 45   /* no frame from the relay for this long = redial */

static void sock_close(sock_t s)
{
    if (s != BADSOCK) close(s);
}

static bool addr_from_text(const char *host, uint16_t port, struct sockaddr_in *out)
{
    uint32_t ip = 0;
    if (!relay_resolve(host, &ip)) return false;

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    out->sin_addr.s_addr = ip;
    return true;
}

static void frame_send(sock_t s, const struct sockaddr_in *to, uint8_t type,
                       uint8_t lobby, uint32_t session, const void *body, size_t len)
{
    uint8_t buf[RELAY_MAX_FRAME];
    if (len > RELAY_MAX_PAYLOAD) return;
    size_t n = relay_write_header(buf, type, lobby, session);
    if (len) memcpy(buf + n, body, len);
    sendto(s, (const char *)buf, (int)(n + len), 0, (const struct sockaddr *)to, sizeof(*to));
}

static void frame_send_sealed(uint8_t type, uint8_t lobby, uint32_t session,
                              const void *body, size_t len)
{
    uint8_t buf[RELAY_MAX_FRAME];
    if (len > RELAY_MAX_PAYLOAD) return;

    size_t n = relay_write_header(buf, type, lobby, session);
    if (len) memcpy(buf + n + RELAY_NONCE_FIELD, body, len);

    int total = relay_seal(&g_crypto, buf, n, len);
    if (total < 0) return;

    sendto(g_tunnel, (const char *)buf, total, 0,
           (const struct sockaddr *)&g_relay_addr, sizeof(g_relay_addr));
}

/* Returns whatever frame arrives, so the caller can tell an offer from a
   failure rather than guessing. */
static int frame_expect_any(sock_t s, uint8_t *buf, size_t cap, int timeout_ms)
{
    struct pollfd p;
    p.fd = (int)s;
    p.events = POLLIN;
    if (Poll(&p, 1, timeout_ms) <= 0) return -1;

    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = (int)recvfrom(s, (char *)buf, (int)cap, 0, (struct sockaddr *)&from, &fl);
    if (n < 0 || !relay_valid_header(buf, (size_t)n)) return -1;
    return n;
}

/* Blocking request/response used only during the short handshake phase. */
static int frame_expect(sock_t s, uint8_t want, uint8_t *buf, size_t cap, int timeout_ms)
{
    struct pollfd p;
    p.fd = (int)s;
    p.events = POLLIN;
    if (Poll(&p, 1, timeout_ms) <= 0) return -1;

    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = (int)recvfrom(s, (char *)buf, (int)cap, 0, (struct sockaddr *)&from, &fl);
    if (n < 0 || !relay_valid_header(buf, (size_t)n)) return -1;
    if (buf[2] != want) return -1;
    return n;
}

/* ---------------------------------------------------------------------------
   Directory step: authenticate with the account password and receive the relay
   list for this account plus a signed ticket. The password itself never
   travels; only an HMAC over the nonce the directory chose.
   --------------------------------------------------------------------------- */
static bool directory_fetch(uint8_t ticket[RELAY_TICKET_SIZE], char *relay_ip, size_t relay_ip_cap,
                            uint16_t *relay_port, uint8_t session_key[32])
{
    struct sockaddr_in dir;
    if (!addr_from_text(g_config.relay_directory_host, (uint16_t)g_config.relay_directory_port, &dir))
    {
        Err("Relay: could not resolve the directory host '%s'.",
            g_config.relay_directory_host);
        return false;
    }

    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BADSOCK) return false;

    uint8_t buf[RELAY_MAX_FRAME];
    size_t ulen = strlen(g_config.relay_username);

    frame_send(s, &dir, RELAY_DIR_HELLO, 0, 0, g_config.relay_username, ulen);

    int n = frame_expect(s, RELAY_DIR_CHALLENGE, buf, sizeof(buf), 4000);
    if (n < (int)(RELAY_HEADER_SIZE + RELAY_NONCE_SIZE))
    {
        Err("Relay: no answer from the directory at %s:%d.",
            g_config.relay_directory_host, g_config.relay_directory_port);
        sock_close(s);
        return false;
    }

    /* proof = HMAC(password, nonce || username) */
    uint8_t msg[RELAY_NONCE_SIZE + RELAY_USER_MAX];
    memcpy(msg, buf + RELAY_HEADER_SIZE, RELAY_NONCE_SIZE);
    if (ulen > RELAY_USER_MAX) ulen = RELAY_USER_MAX;
    memcpy(msg + RELAY_NONCE_SIZE, g_config.relay_username, ulen);

    uint8_t proof[RELAY_MAC_SIZE];
    hmac_sha256(g_config.relay_password, strlen(g_config.relay_password),
                msg, RELAY_NONCE_SIZE + ulen, proof);

    /* Same nonce, different label: this unmasks the session key in the offer. */
    uint8_t mask[32];
    static const char *sklabel = "session-key";
    uint8_t skmsg[RELAY_NONCE_SIZE + 16];
    memcpy(skmsg, buf + RELAY_HEADER_SIZE, RELAY_NONCE_SIZE);
    memcpy(skmsg + RELAY_NONCE_SIZE, sklabel, strlen(sklabel));
    hmac_sha256(g_config.relay_password, strlen(g_config.relay_password),
                skmsg, RELAY_NONCE_SIZE + strlen(sklabel), mask);

    uint8_t auth[RELAY_MAC_SIZE + RELAY_USER_MAX];
    memcpy(auth, proof, RELAY_MAC_SIZE);
    memcpy(auth + RELAY_MAC_SIZE, g_config.relay_username, ulen);
    frame_send(s, &dir, RELAY_DIR_AUTH, 0, 0, auth, RELAY_MAC_SIZE + ulen);

    /* The directory answers with an offer, or a failure carrying a reason.
       Reporting both as a bad password sent us hunting the wrong problem once
       already, so say which one actually happened. */
    n = frame_expect_any(s, buf, sizeof(buf), 4000);
    if (n < 0)
    {
        Err("Relay: the directory stopped answering during sign-in.");
        sock_close(s);
        return false;
    }

    if (buf[2] == RELAY_DIR_FAIL)
    {
        uint8_t why = (n > (int)RELAY_HEADER_SIZE) ? buf[RELAY_HEADER_SIZE] : 0;
        switch (why)
        {
        case RELAY_FAIL_BADMAC:
            Err("Relay: wrong relay password for '%s'.", g_config.relay_username);
            break;
        case RELAY_FAIL_SUSPENDED:
            Err("Relay: this server has been suspended by the host of the relay.");
            break;
        case RELAY_FAIL_BADUSER:
        default:
            Err("Relay: the directory does not recognise '%s'. Either the account "
                "is gone, its region was removed, or the directory cannot read "
                "its account file.", g_config.relay_username);
            break;
        }
        sock_close(s);
        return false;
    }

    if (buf[2] != RELAY_DIR_OFFER)
    {
        Err("Relay: unexpected reply from the directory (type %u).", buf[2]);
        sock_close(s);
        return false;
    }

    /* offer: ticket, then one endpoint (region, ipv4, port) */
    const uint8_t *p = buf + RELAY_HEADER_SIZE;
    size_t left = (size_t)n - RELAY_HEADER_SIZE;
    if (left < RELAY_TICKET_SIZE + RELAY_REGION_MAX + 6 + 32) { sock_close(s); return false; }

    memcpy(ticket, p, RELAY_TICKET_SIZE);
    p += RELAY_TICKET_SIZE;

    char region[RELAY_REGION_MAX + 1];
    memcpy(region, p, RELAY_REGION_MAX);
    region[RELAY_REGION_MAX] = 0;
    p += RELAY_REGION_MAX;

    struct in_addr ia;
    memcpy(&ia.s_addr, p, 4);
    p += 4;
    uint16_t port = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;

    for (int i = 0; i < 32; i++) session_key[i] = (uint8_t)(p[i] ^ mask[i]);

    snprintf(relay_ip, relay_ip_cap, "%s", inet_ntoa(ia));
    *relay_port = port;

    Info("Relay: region " LOG_GRN "%s" LOG_RST " at %s:%u", region, relay_ip, port);
    sock_close(s);
    return true;
}

static TunnelSession *session_by_id(uint32_t id)
{
    for (int i = 0; i < RELAY_MAX_SESSIONS; i++)
        if (g_sessions[i].used && g_sessions[i].id == id) return &g_sessions[i];
    return NULL;
}

static void session_close(TunnelSession *s)
{
    if (!s->used) return;
    sock_close(s->local);
    s->used = false;
}

static TunnelSession *session_open(uint32_t id, uint8_t lobby, uint32_t synthetic)
{
    for (int i = 0; i < RELAY_MAX_SESSIONS; i++)
    {
        if (g_sessions[i].used) continue;
        TunnelSession *s = &g_sessions[i];

        s->local = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->local == BADSOCK) return NULL;

        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;                       /* ephemeral */
        if (bind(s->local, (struct sockaddr *)&a, sizeof(a)) != 0)
        {
            sock_close(s->local);
            return NULL;
        }

        socklen_t al = sizeof(a);
        getsockname(s->local, (struct sockaddr *)&a, &al);

        s->used = true;
        s->id = id;
        s->lobby = lobby;
        s->local_port = ntohs(a.sin_port);
        s->last_seen = time(NULL);
        snprintf(s->synthetic, sizeof(s->synthetic), "%u.%u.%u.%u",
                 (synthetic >> 24) & 0xff, (synthetic >> 16) & 0xff,
                 (synthetic >> 8) & 0xff, synthetic & 0xff);

        Debug("Relay: session %u -> local port %u as %s", id, s->local_port, s->synthetic);
        return s;
    }
    return NULL;
}

/* Server.c asks for this at the single point where a peer address is read. */
uint16_t relay_public_port(int lobby_index)
{
    /* Only meaningful once the tunnel is up: before that we have no ticket and
       therefore no public port block to point anyone at. */
    if (relay_is_online() && g_base_port)
        return (uint16_t)(g_base_port + lobby_index);
    return (uint16_t)(g_config.port + lobby_index);
}

bool relay_lookup_ip(uint32_t host_be, uint16_t port, char *out, size_t cap)
{
    if (!g_enabled) return false;
    if (host_be != htonl(INADDR_LOOPBACK)) return false;

    bool found = false;
    MutexLock(g_sessions_lock);
    for (int i = 0; i < RELAY_MAX_SESSIONS; i++)
        if (g_sessions[i].used && g_sessions[i].local_port == port)
        {
            snprintf(out, cap, "%s", g_sessions[i].synthetic);
            found = true;
            break;
        }
    MutexUnlock(g_sessions_lock);
    return found;
}

bool relay_is_enabled(void) { return g_enabled; }
bool relay_is_online(void)  { return g_online; }

static bool tunnel_connect(void)
{
    uint8_t ticket[RELAY_TICKET_SIZE];
    uint8_t session_key[32];
    char    ip[32];
    uint16_t port = 0;

    if (!directory_fetch(ticket, ip, sizeof(ip), &port, session_key)) return false;
    if (!addr_from_text(ip, port, &g_relay_addr)) return false;

    g_tunnel = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_tunnel == BADSOCK) return false;

    uint8_t buf[RELAY_MAX_FRAME];
    frame_send(g_tunnel, &g_relay_addr, RELAY_HELLO, 0, 0, NULL, 0);
    if (frame_expect(g_tunnel, RELAY_CHALLENGE, buf, sizeof(buf), 4000) < 0)
    {
        Err("Relay: no answer from the relay at %s:%u.", ip, port);
        sock_close(g_tunnel); g_tunnel = BADSOCK;
        return false;
    }

    frame_send(g_tunnel, &g_relay_addr, RELAY_AUTH, 0, 0, ticket, RELAY_TICKET_SIZE);
    int n = frame_expect(g_tunnel, RELAY_AUTH_OK, buf, sizeof(buf), 4000);
    if (n < (int)(RELAY_HEADER_SIZE + 5))
    {
        Err("Relay: the relay rejected our ticket.");
        sock_close(g_tunnel); g_tunnel = BADSOCK;
        return false;
    }

    g_base_port  = relay_get32(buf + RELAY_HEADER_SIZE);
    g_port_count = buf[RELAY_HEADER_SIZE + 4];
    relay_derive_keys(session_key, true, &g_crypto);
    g_last_rx = time(NULL);
    g_relay_echoes = false;
    g_online = true;

    Info("Relay: " LOG_GRN "online" LOG_RST " - players can join at %s ports %u-%u",
         ip, (unsigned)g_base_port, (unsigned)g_base_port + g_port_count - 1);
    return true;
}

static bool tunnel_pump(void)
{
    struct pollfd pfd[1 + RELAY_MAX_SESSIONS];
    int map[1 + RELAY_MAX_SESSIONS];
    int nfd = 0;

    pfd[nfd].fd = (int)g_tunnel; pfd[nfd].events = POLLIN; map[nfd] = -1; nfd++;

    MutexLock(g_sessions_lock);
    for (int i = 0; i < RELAY_MAX_SESSIONS && nfd < (int)(sizeof(map) / sizeof(map[0])); i++)
        if (g_sessions[i].used)
        {
            pfd[nfd].fd = (int)g_sessions[i].local;
            pfd[nfd].events = POLLIN;
            map[nfd] = i;
            nfd++;
        }
    MutexUnlock(g_sessions_lock);

    if (Poll(pfd, nfd, 200) <= 0) return true;

    for (int i = 0; i < nfd; i++)
    {
        if (!(pfd[i].revents & POLLIN)) continue;

        uint8_t buf[RELAY_MAX_FRAME];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);

        if (map[i] < 0)
        {
            /* from the relay */
            int n = (int)recvfrom(g_tunnel, (char *)buf, sizeof(buf), 0,
                                  (struct sockaddr *)&from, &fl);
            if (n < 0 || !relay_valid_header(buf, (size_t)n)) continue;

            /* Reject anything without a valid tag: that covers forged frames,
               replays, and any attempt to move a payload to another session. */
            int plain = relay_open(&g_crypto, buf, RELAY_HEADER_SIZE, (size_t)n);
            if (plain < 0) continue;

            g_last_rx = time(NULL);

            uint8_t  type    = buf[2];
            if (type == RELAY_KEEPALIVE)
                g_relay_echoes = true;
            uint8_t  lobby   = buf[3];
            uint32_t session = relay_get32(buf + 4);
            uint8_t *body    = buf + RELAY_HEADER_SIZE + RELAY_NONCE_FIELD;
            size_t   blen    = (size_t)plain;

            if (type == RELAY_SESSION_OPEN && blen >= 4)
            {
                MutexLock(g_sessions_lock);
                if (!session_by_id(session))
                    session_open(session, lobby, relay_get32(body));
                MutexUnlock(g_sessions_lock);
            }
            else if (type == RELAY_SESSION_CLOSE)
            {
                MutexLock(g_sessions_lock);
                TunnelSession *s = session_by_id(session);
                if (s) session_close(s);
                MutexUnlock(g_sessions_lock);
            }
            else if (type == RELAY_DATA)
            {
                MutexLock(g_sessions_lock);
                TunnelSession *s = session_by_id(session);
                if (!s) s = session_open(session, lobby, 0);
                if (s)
                {
                    struct sockaddr_in game;
                    memset(&game, 0, sizeof(game));
                    game.sin_family = AF_INET;
                    game.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    game.sin_port = htons((uint16_t)(g_config.port + s->lobby));
                    sendto(s->local, (const char *)body, (int)blen, 0,
                           (struct sockaddr *)&game, sizeof(game));
                    s->last_seen = time(NULL);
                }
                MutexUnlock(g_sessions_lock);
            }
            continue;
        }

        /* reply coming back out of the game server */
        MutexLock(g_sessions_lock);
        TunnelSession *s = &g_sessions[map[i]];
        if (s->used)
        {
            int n = (int)recvfrom(s->local, (char *)buf, sizeof(buf), 0,
                                  (struct sockaddr *)&from, &fl);
            if (n > 0)
            {
                s->last_seen = time(NULL);
                frame_send_sealed(RELAY_DATA, s->lobby, s->id, buf, (size_t)n);
            }
        }
        MutexUnlock(g_sessions_lock);
    }

    return true;
}

/* Drop every session and the tunnel socket, so the next loop redials from
   scratch with a fresh ticket and a fresh session key. */
static bool tunnel_teardown(void)
{
    MutexLock(g_sessions_lock);
    for (int i = 0; i < RELAY_MAX_SESSIONS; i++)
        if (g_sessions[i].used) session_close(&g_sessions[i]);
    MutexUnlock(g_sessions_lock);

    if (g_tunnel != BADSOCK) { sock_close(g_tunnel); g_tunnel = BADSOCK; }
    memset(&g_crypto, 0, sizeof(g_crypto));
    g_online = false;
    return true;
}

static bool relay_worker(void *unused)
{
    (void)unused;
    ThreadVarSet(g_threadName, "Relay Thr");

    time_t next_keepalive = 0;

    while (true)
    {
        if (!g_online)
        {
            if (!tunnel_connect())
            {
                Warn("Relay: retrying in %d seconds.", RELAY_RETRY_SEC);
                ThreadSleep(RELAY_RETRY_SEC * 1000);
                continue;
            }
            next_keepalive = 0;
        }

        (void)tunnel_pump();

        time_t now = time(NULL);
        if (now >= next_keepalive)
        {
            frame_send_sealed(RELAY_KEEPALIVE, 0, 0, NULL, 0);
            next_keepalive = now + RELAY_KEEPALIVE_SEC;
        }

        /* If the relay stops answering - it restarted, the route changed, the
           NAT mapping expired - drop everything and dial again rather than
           silently sending into a hole. */
        if (g_online && g_relay_echoes && now - g_last_rx > RELAY_DEAD_SEC)
        {
            Warn("Relay: no reply for %ds, reconnecting.", RELAY_DEAD_SEC);
            (void)tunnel_teardown();
        }
    }
    return true;
}

bool relay_start(void)
{
    g_enabled = g_config.relay_enabled;
    if (!g_enabled) return true;

    if (!g_config.relay_username[0] || !g_config.relay_password[0])
    {
        Err("Relay is enabled but relay_username/relay_password are empty.");
        g_enabled = false;
        return true;
    }

    MutexCreate(g_sessions_lock);

    Thread thr;
    ThreadSpawn(thr, relay_worker, NULL);
    Info("Relay: connecting to the directory at %s:%d as " LOG_GRN "%s" LOG_RST,
         g_config.relay_directory_host, g_config.relay_directory_port, g_config.relay_username);
    return true;
}
