#ifndef RELAYPROTO_H
#define RELAYPROTO_H
/* Wire protocol shared by the relay daemon and the tunnel client that is built
   into the server. Everything travels over a single UDP flow between the host
   and the relay; game payloads are already ENet, which handles its own
   reliability, so the tunnel deliberately adds none. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RELAY_MAGIC0        0xBE
#define RELAY_MAGIC1        0x01          /* protocol version */
#define RELAY_HEADER_SIZE   8
#define RELAY_MAX_FRAME     1400          /* stay under common MTU */
#define RELAY_TAG_SIZE      16
#define RELAY_NONCE_FIELD    8
/* header + per-frame nonce + tag */
#define RELAY_OVERHEAD      (RELAY_HEADER_SIZE + RELAY_NONCE_FIELD + RELAY_TAG_SIZE)
/* What actually fits in a sealed frame. This must account for the nonce and
   tag, not just the header: a payload sized against the header alone overruns
   the send buffer once the frame is sealed, and anything the relay accepts
   here it must also be able to carry. The game's ENet MTU is pinned to this
   when the relay is on, so no datagram is ever too large to forward. */
#define RELAY_MAX_PAYLOAD   (RELAY_MAX_FRAME - RELAY_OVERHEAD)
#define RELAY_MAX_PORTS     6             /* lobbies per host account */
#define RELAY_NONCE_SIZE    16
#define RELAY_MAC_SIZE      32
#define RELAY_USER_MAX      32

/* frame types */
enum
{
    RELAY_HELLO         = 0x01,  /* host  -> relay : username                  */
    RELAY_CHALLENGE     = 0x02,  /* relay -> host  : nonce                     */
    RELAY_AUTH          = 0x03,  /* host  -> relay : HMAC(secret, nonce|user)  */
    RELAY_AUTH_OK       = 0x04,  /* relay -> host  : port_count, base_port     */
    RELAY_AUTH_FAIL     = 0x05,  /* relay -> host  : reason byte               */
    RELAY_SESSION_OPEN  = 0x06,  /* relay -> host  : lobby, synthetic ip       */
    RELAY_SESSION_CLOSE = 0x07,  /* both ways                                  */
    RELAY_DATA          = 0x08,  /* both ways : game payload                   */
    RELAY_KEEPALIVE     = 0x09,  /* host  -> relay : keeps the NAT mapping open*/

    /* Directory: one central service that knows accounts and regions. The host
       authenticates here first, gets the relay list plus a signed ticket, then
       talks to whichever relay it picked. Relays verify the ticket with a key
       they share with the directory, so they hold no account database at all
       and a new region is just another daemon with that key. */
    RELAY_DIR_HELLO     = 0x10,  /* host -> directory : username               */
    RELAY_DIR_CHALLENGE = 0x11,  /* directory -> host : nonce                  */
    RELAY_DIR_AUTH      = 0x12,  /* host -> directory : HMAC(secret,nonce|user)*/
    RELAY_DIR_OFFER     = 0x13,  /* directory -> host : relays + ticket        */
    RELAY_DIR_FAIL      = 0x14,  /* directory -> host : reason byte            */

    /* Status query. The website asks a relay what is actually online, proving
       it holds the ticket key the relays and directory already share, so this
       needs no new secret. The answer carries account ids and player counts
       only - never a player address, which the relay does not disclose to
       anyone, including the site. */
    RELAY_STAT_REQ      = 0x20,  /* site  -> relay : ts, nonce, mac            */
    RELAY_STAT_RESP     = 0x21   /* relay -> site  : per-account player counts */
};

#define RELAY_STAT_LABEL_REQ  "relay-status-req"   /* 16 bytes, no terminator */
#define RELAY_STAT_LABEL_RESP "relay-status-res"
#define RELAY_STAT_SKEW       30   /* seconds a status request stays valid    */
#define RELAY_STAT_MAX       200   /* accounts reported in one response       */

#define RELAY_MAX_REGIONS   8
#define RELAY_REGION_MAX    16

/* A ticket is issued by the directory and presented to a relay. The relay only
   has to check the MAC and the expiry, so it never needs to know the account's
   password. Keep it small: it travels in an AUTH frame. */
#define RELAY_TICKET_FIELDS 24
#define RELAY_TICKET_KEYLEN 32
/* fields, then the session key sealed under a key derived from the ticket key */
#define RELAY_TICKET_BODY   (RELAY_TICKET_FIELDS + RELAY_TICKET_KEYLEN)
#define RELAY_TICKET_SIZE   (RELAY_TICKET_BODY + RELAY_MAC_SIZE)

typedef struct
{
    uint32_t account_id;
    uint32_t base_port;     /* first public port on that relay */
    uint32_t port_count;    /* lobbies allowed, <= RELAY_MAX_PORTS */
    uint32_t issued_at;     /* unix seconds */
    uint32_t expires_at;    /* unix seconds */
    uint32_t region_id;
} RelayTicket;

typedef struct
{
    char     region[RELAY_REGION_MAX];
    uint32_t ip;            /* network order, as sent on the wire */
    uint16_t port;
} RelayEndpoint;

/* auth failure reasons */
enum
{
    RELAY_FAIL_BADUSER  = 1,
    RELAY_FAIL_BADMAC   = 2,
    RELAY_FAIL_INUSE    = 3,
    RELAY_FAIL_SUSPENDED= 4
};

static inline void relay_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v      ); p[1] = (uint8_t)(v >>  8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t relay_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* header: magic0, magic1, type, lobby, session(4) */
static inline size_t relay_write_header(uint8_t *buf, uint8_t type, uint8_t lobby, uint32_t session)
{
    buf[0] = RELAY_MAGIC0;
    buf[1] = RELAY_MAGIC1;
    buf[2] = type;
    buf[3] = lobby;
    relay_put32(buf + 4, session);
    return RELAY_HEADER_SIZE;
}

static inline int relay_valid_header(const uint8_t *buf, size_t len)
{
    return len >= RELAY_HEADER_SIZE && buf[0] == RELAY_MAGIC0 && buf[1] == RELAY_MAGIC1;
}

/* Per-direction keys plus the replay window for the receiving side. */
typedef struct
{
    uint8_t  send_enc[32], send_mac[32];
    uint8_t  recv_enc[32], recv_mac[32];
    uint64_t send_counter;
    uint64_t recv_highest;
    uint64_t recv_window;
    int      ready;
} RelayCrypto;

void relay_derive_keys(const uint8_t session_key[32], bool host_side, RelayCrypto *out);
void relay_keystream_xor(const uint8_t key[32], const uint8_t nonce[8], uint8_t *data, size_t len);
int  relay_seal(RelayCrypto *c, uint8_t *frame, size_t header_len, size_t payload_len);
int  relay_open(RelayCrypto *c, uint8_t *frame, size_t header_len, size_t frame_len);

/* SHA-256 / HMAC-SHA256 (FIPS 180-4 / RFC 2104) */
typedef struct { uint32_t s[8]; uint64_t n; uint8_t b[64]; size_t bl; } SHA256;
void sha256_init(SHA256 *c);
void sha256_update(SHA256 *c, const void *data, size_t len);
void sha256_final(SHA256 *c, uint8_t out[32]);
/* (renamed: the WebRTC library has its own hmac_sha256) */
#define hmac_sha256 ds_hmac_sha256
void hmac_sha256(const void *key, size_t keylen,
                 const void *msg, size_t msglen, uint8_t out[32]);

#endif
