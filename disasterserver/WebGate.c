/* WebSocket gateway for browser players.
 *
 * A browser cannot send UDP, so the web version of the game sends each ENet datagram as one
 * binary WebSocket message to ws://<server>:<port>/, the same port number as the lobby's UDP
 * port (TCP and UDP ports are separate). This thread listens on TCP for every lobby port.
 *
 * Exactly like a relay session (Relay.c), each WebSocket connection gets its own loopback UDP
 * socket that forwards to the real game port, so ENet sees an ordinary peer and the rest of the
 * server is untouched. The address reported for that peer (webgate_lookup_ip) is the browser's
 * real one, which bans, operators and logs then key on.
 *
 * Phone players come through the site of the phone app (a trusted proxy, "web_proxies"): for
 * those the address is the X-Real-IP the proxy sends (WsTrust.h), and the per-address limit
 * counts each player, not the proxy.
 *
 * Off with "web_clients": false in Config.json (section "server").
 */
#include <WebGate.h>
#include <WsProto.h>
#include <WsTrust.h>
#include <WebRtc.h>
#include <Config.h>
#include <Log.h>
#include <io/Socket.h>
#include <io/Threads.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  typedef SOCKET sock_t;
  #define BADSOCK INVALID_SOCKET
  #define sock_close closesocket
  static bool sock_would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
  typedef int sock_t;
  #define BADSOCK (-1)
  #define sock_close close
  static bool sock_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

#define WG_MAX_LOBBIES   16
#define WG_MAX_CLIENTS   256
#define WG_MAX_PER_IP    8
#define WG_IDLE_SEC      60    /* ENet pings every 250 ms: silence means the browser is gone */
#define WG_HANDSHAKE_SEC 10
#define WG_IN_CAP        8192
#define WG_OUT_CAP       32768
/* A browser that cannot keep up: past this much waiting data the game's datagrams for it are
   dropped, like UDP loss (ENet re-sends what is reliable; old positions are worthless), instead of
   queueing for seconds (players "slow and delayed", someone stuck in the air). The socket's own
   send buffer is kept small for the same reason. */
#define WG_DROP_AT       8192
#define WG_SNDBUF        (64 * 1024)

typedef struct
{
    bool     used;
    bool     open;          /* handshake done                          */
    bool     via_proxy;     /* from a trusted proxy: ip is set from its X-Real-IP */
    sock_t   tcp;
    sock_t   udp;           /* loopback socket toward the game port    */
    uint16_t game_port;
    uint16_t local_port;    /* what ENet sees as the peer port         */
    char     ip[48];        /* the browser's address                   */
    time_t   last_seen;
    uint8_t  in[WG_IN_CAP];
    size_t   in_len;
    uint8_t  msg[WS_MAX_MESSAGE];  /* a fragmented message being put together */
    size_t   msg_len;
    uint8_t  out[WG_OUT_CAP];      /* frames the socket could not take yet     */
    size_t   out_len;
    unsigned gen;                  /* tells a reused slot apart (WebRTC callbacks) */
    bool     msg_text;             /* the message being put together is text (WebRTC setup) */
} WgClient;

static WgClient *g_clients;
static Mutex     g_lock;               /* g_clients' ports and addresses (webgate_lookup_ip) */
static bool      g_enabled = false;
static sock_t    g_listen[WG_MAX_LOBBIES];
static uint16_t  g_listen_port[WG_MAX_LOBBIES];
static int       g_listen_count;
static unsigned  g_gen;

static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

/* an address as it may appear in the log */
static const char *shown(const char *ip) { return ip; }

static bool client_close(WgClient *c)
{
    if (!c->used) return true;
    wrtc_close((int)(c - g_clients));  /* its data channel first: it sends on c->udp */
    if (c->tcp != BADSOCK) sock_close(c->tcp);
    if (c->udp != BADSOCK) sock_close(c->udp);
    MutexLock(g_lock);
    c->used = false;
    c->open = false;
    MutexUnlock(g_lock);
    Debug("WebGate: %s left (local port %u)", shown(c->ip), c->local_port);
    return true;
}

/* Queue bytes for the browser; false when it cannot keep up (the connection is dropped) */
static bool client_send(WgClient *c, const uint8_t *data, size_t len)
{
    if (c->out_len == 0)
    {
        int n = (int)send(c->tcp, (const char *)data, (int)len, MSG_NOSIGNAL);
        if (n < 0 && !sock_would_block()) return false;
        if (n < 0) n = 0;
        data += n;
        len -= (size_t)n;
    }
    if (len == 0) return true;
    if (c->out_len + len > WG_OUT_CAP) return false;
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    return true;
}

static void client_flush(WgClient *c)
{
    if (!c->out_len) return;
    int n = (int)send(c->tcp, (const char *)c->out, (int)c->out_len, MSG_NOSIGNAL);
    if (n < 0)
    {
        if (!sock_would_block()) client_close(c);
        return;
    }
    memmove(c->out, c->out + n, c->out_len - (size_t)n);
    c->out_len -= (size_t)n;
}

static bool client_frame(WgClient *c, uint8_t opcode, const uint8_t *payload, size_t len)
{
    uint8_t frame[10 + WS_MAX_MESSAGE];
    if (len > WS_MAX_MESSAGE) return true;  /* (an ENet datagram never is) */
    size_t h = ws_frame_header(frame, opcode, len);
    memcpy(frame + h, payload, len);
    return client_send(c, frame, h + len);
}

/* A whole message from the browser: one ENet datagram for the game port */
static void client_datagram(WgClient *c, const uint8_t *data, size_t len)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(c->game_port);
    sendto(c->udp, (const char *)data, (int)len, 0, (struct sockaddr *)&to, sizeof(to));
}

static bool client_open_udp(WgClient *c)
{
    c->udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (c->udp == BADSOCK) return false;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;  /* ephemeral */
    if (bind(c->udp, (struct sockaddr *)&a, sizeof(a)) != 0) return false;
    socklen_t al = sizeof(a);
    getsockname(c->udp, (struct sockaddr *)&a, &al);
    set_nonblocking(c->udp);
    MutexLock(g_lock);
    c->local_port = ntohs(a.sin_port);
    c->open = true;
    MutexUnlock(g_lock);
    return true;
}

static int clients_from_ip(const char *ip)
{
    int n = 0;
    for (int i = 0; i < WG_MAX_CLIENTS; i++)
        if (g_clients[i].used && !strcmp(g_clients[i].ip, ip)) n++;
    return n;
}

/* Bytes from the browser: the HTTP upgrade, then frames */
/* A text message: the browser's WebRTC offer ("o" + SDP). The answer comes back as text ("a" +
   SDP) once ready (the worker loop); then the datagrams use the data channel. */
static void client_signal(WgClient *c)
{
    if (c->msg_len < 2 || c->msg_len >= sizeof(c->msg) || c->msg[0] != 'o' || !wrtc_available()) return;
    c->msg[c->msg_len] = 0;
    if (!wrtc_open((int)(c - g_clients), c->gen, (const char *)c->msg + 1,
                   (uint16_t)(c->game_port + WRTC_PORT_OFFSET), c->udp, c->game_port))
        Debug("WebGate: %s: no data channel (WebSocket only)", shown(c->ip));
}

/* (bool: the lock macros return false when they fail) */
static bool client_set_ip(WgClient *c, const char *ip)
{
    MutexLock(g_lock);
    snprintf(c->ip, sizeof(c->ip), "%s", ip);
    MutexUnlock(g_lock);
    return true;
}

static void client_input(WgClient *c)
{
    if (!c->open)
    {
        size_t req = ws_request_length((const char *)c->in, c->in_len);
        if (!req)
        {
            if (c->in_len >= WS_MAX_REQUEST) client_close(c);
            return;
        }
        char resp[512];
        size_t rlen = 0;
        bool ok = ws_handshake((const char *)c->in, req, resp, sizeof(resp), &rlen);
        char real[48];
        if (ok && c->via_proxy && ws_trust_forwarded((const char *)c->in, req, real, sizeof(real)))
        {
            if (clients_from_ip(real) >= WG_MAX_PER_IP)
            {
                Debug("WebGate: %s (through %s) has too many connections", shown(real), shown(c->ip));
                client_close(c);
                return;
            }
            client_set_ip(c, real);
        }
        client_send(c, (const uint8_t *)resp, rlen);
        if (!ok || !client_open_udp(c))
        {
            client_flush(c);
            client_close(c);
            return;
        }
        memmove(c->in, c->in + req, c->in_len - req);
        c->in_len -= req;
        Debug("WebGate: %s joined port %u (local port %u)", shown(c->ip), c->game_port, c->local_port);
    }
    while (c->used && c->in_len)
    {
        WsFrame f;
        int used = ws_parse(c->in, c->in_len, &f);
        if (used == 0) break;
        if (used < 0)
        {
            client_close(c);
            return;
        }
        switch (f.opcode)
        {
        case WS_OP_BINARY:
        case WS_OP_TEXT:
        case WS_OP_CONT:
            if (f.opcode != WS_OP_CONT)
            {
                c->msg_len = 0;
                c->msg_text = f.opcode == WS_OP_TEXT;
            }
            if (c->msg_len + f.payload_len <= sizeof(c->msg))
            {
                memcpy(c->msg + c->msg_len, f.payload, f.payload_len);
                c->msg_len += f.payload_len;
            }
            if (f.fin)
            {
                if (c->msg_text) client_signal(c);
                else client_datagram(c, c->msg, c->msg_len);
                c->msg_len = 0;
            }
            break;
        case WS_OP_PING:
            client_frame(c, WS_OP_PONG, f.payload, f.payload_len);
            break;
        case WS_OP_CLOSE:
            client_frame(c, WS_OP_CLOSE, NULL, 0);
            client_flush(c);
            client_close(c);
            return;
        default:
            break;
        }
        memmove(c->in, c->in + used, c->in_len - (size_t)used);
        c->in_len -= (size_t)used;
    }
}


static bool accept_client(sock_t l, uint16_t game_port)
{
    struct sockaddr_storage from;
    socklen_t fl = sizeof(from);
    sock_t t = accept(l, (struct sockaddr *)&from, &fl);
    if (t == BADSOCK) return true;
    char ip[48] = "?";
    if (from.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&from)->sin_addr, ip, sizeof(ip));
    else if (from.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&from)->sin6_addr, ip, sizeof(ip));
    WgClient *c = NULL;
    bool proxy = ws_trust_is(ip);  /* (its players are counted by their own address later) */
    if (proxy || clients_from_ip(ip) < WG_MAX_PER_IP)
        for (int i = 0; i < WG_MAX_CLIENTS && !c; i++)
            if (!g_clients[i].used) c = &g_clients[i];
    if (!c)
    {
        sock_close(t);
        return true;
    }
    set_nonblocking(t);
    int one = 1;
    setsockopt(t, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    int sndbuf = WG_SNDBUF;
    setsockopt(t, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
    memset(c, 0, offsetof(WgClient, in));
    c->tcp = t;
    c->udp = BADSOCK;
    c->game_port = game_port;
    c->via_proxy = proxy;
    snprintf(c->ip, sizeof(c->ip), "%s", ip);
    c->last_seen = time(NULL);
    c->in_len = c->msg_len = c->out_len = 0;
    c->gen = ++g_gen;
    MutexLock(g_lock);
    c->used = true;
    MutexUnlock(g_lock);
    return true;
}

static bool webgate_worker(void *unused)
{
    (void)unused;
    ThreadVarSet(g_threadName, "WebGate Thr");
    static struct pollfd fds[WG_MAX_LOBBIES + WG_MAX_CLIENTS * 2];
    static int who[WG_MAX_LOBBIES + WG_MAX_CLIENTS * 2];  /* listener index, or client*2 (+1: udp) */
    while (true)
    {
        int n = 0;
        for (int i = 0; i < g_listen_count; i++)
        {
            fds[n].fd = g_listen[i];
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            who[n++] = -1 - i;
        }
        for (int i = 0; i < WG_MAX_CLIENTS; i++)
        {
            WgClient *c = &g_clients[i];
            if (!c->used) continue;
            fds[n].fd = c->tcp;
            fds[n].events = (short)(POLLIN | (c->out_len ? POLLOUT : 0));
            fds[n].revents = 0;
            who[n++] = i * 2;
            if (c->open)
            {
                fds[n].fd = c->udp;
                fds[n].events = POLLIN;
                fds[n].revents = 0;
                who[n++] = i * 2 + 1;
            }
        }
        int ready = Poll(fds, (unsigned)n, wrtc_pending() ? 20 : 500);
        /* WebRTC answers made on libdatachannel's threads go out from here */
        for (int i = 0; wrtc_available() && i < WG_MAX_CLIENTS; i++)
        {
            WgClient *c = &g_clients[i];
            static char answer[4200];
            if (!c->used || !c->open) continue;
            answer[0] = 'a';
            if (wrtc_take_answer(i, c->gen, answer + 1, sizeof(answer) - 1))
                if (!client_frame(c, WS_OP_TEXT, (const uint8_t *)answer, strlen(answer))) client_close(c);
        }
        time_t now = time(NULL);
        for (int k = 0; ready > 0 && k < n; k++)
        {
            if (!fds[k].revents) continue;
            if (who[k] < 0)
            {
                int li = -1 - who[k];
                accept_client(g_listen[li], g_listen_port[li]);
                continue;
            }
            WgClient *c = &g_clients[who[k] / 2];
            if (!c->used) continue;
            if (who[k] & 1)
            {
                /* the server's reply for this browser */
                uint8_t buf[WS_MAX_MESSAGE];
                for (int burst = 0; burst < 64; burst++)
                {
                    int got = (int)recvfrom(c->udp, (char *)buf, sizeof(buf), 0, NULL, NULL);
                    if (got <= 0) break;
                    if (wrtc_send((int)(c - g_clients), c->gen, buf, (size_t)got)) continue;  /* the data channel */
                    if (c->out_len > WG_DROP_AT) continue;  /* backed up: this one is "lost" */
                    if (!client_frame(c, WS_OP_BINARY, buf, (size_t)got))
                    {
                        client_close(c);
                        break;
                    }
                }
                continue;
            }
            if (fds[k].revents & POLLOUT) client_flush(c);
            if (!c->used) continue;
            if (fds[k].revents & (POLLIN | POLLHUP | POLLERR))
            {
                int got = (int)recv(c->tcp, (char *)c->in + c->in_len, (int)(WG_IN_CAP - c->in_len), 0);
                if (got <= 0)
                {
                    if (got < 0 && sock_would_block()) continue;
                    client_close(c);
                    continue;
                }
                c->in_len += (size_t)got;
                c->last_seen = now;
                client_input(c);
            }
        }
        for (int i = 0; i < WG_MAX_CLIENTS; i++)
        {
            WgClient *c = &g_clients[i];
            if (!c->used) continue;
            time_t seen = c->last_seen, rtc = wrtc_last_seen(i, c->gen);  /* the data channel counts too */
            if (rtc > seen) seen = rtc;
            if (now - seen > (c->open ? WG_IDLE_SEC : WG_HANDSHAKE_SEC)) client_close(c);
        }
    }
    return true;
}

bool webgate_start(void)
{
    g_enabled = g_config.web_clients;
    if (!g_enabled) return true;
    g_clients = calloc(WG_MAX_CLIENTS, sizeof(WgClient));
    if (!g_clients)
    {
        g_enabled = false;
        return true;
    }
    MutexCreate(g_lock);
    ws_trust_start(g_config.web_proxies);
    wrtc_init(WG_MAX_CLIENTS);
    int lobbies = g_config.server_count < WG_MAX_LOBBIES ? g_config.server_count : WG_MAX_LOBBIES;
    for (int i = 0; i < lobbies; i++)
    {
        uint16_t port = (uint16_t)(g_config.port + i);
        sock_t l = socket(AF_INET, SOCK_STREAM, 0);
        if (l == BADSOCK) continue;
        int one = 1;
        setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (bind(l, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(l, 32) != 0)
        {
            Warn("WebGate: TCP port %u is in use, browser players cannot join that lobby.", port);
            sock_close(l);
            continue;
        }
        set_nonblocking(l);
        g_listen[g_listen_count] = l;
        g_listen_port[g_listen_count++] = port;
    }
    if (!g_listen_count)
    {
        g_enabled = false;
        return true;
    }
    Thread thr;
    ThreadSpawn(thr, webgate_worker, NULL);
    Info("WebGate: browser players can join on TCP port%s %u%s", g_listen_count > 1 ? "s" : "",
         g_listen_port[0], g_listen_count > 1 ? " and up" : "");
    return true;
}

bool webgate_lookup_ip(uint32_t host_be, uint16_t port, char *out, size_t cap)
{
    if (!g_enabled) return false;
    if (host_be != htonl(INADDR_LOOPBACK)) return false;
    bool found = false;
    MutexLock(g_lock);
    for (int i = 0; i < WG_MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].open && g_clients[i].local_port == port)
        {
            snprintf(out, cap, "%s", g_clients[i].ip);
            found = true;
            break;
        }
    MutexUnlock(g_lock);
    return found;
}
