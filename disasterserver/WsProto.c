/* WebSocket handshake and framing for browser players (see WsProto.h). */
#include <WsProto.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- SHA-1 (handshake only) */

typedef struct { uint32_t h[5]; uint64_t n; uint8_t b[64]; size_t bl; } Sha1;

static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(Sha1 *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1(const void *data, size_t len, uint8_t out[20])
{
    Sha1 c = { { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 }, 0, { 0 }, 0 };
    const uint8_t *p = data;
    c.n = (uint64_t)len * 8;
    for (size_t i = 0; i < len; i++)
    {
        c.b[c.bl++] = p[i];
        if (c.bl == 64) { sha1_block(&c, c.b); c.bl = 0; }
    }
    c.b[c.bl++] = 0x80;
    if (c.bl > 56) { while (c.bl < 64) c.b[c.bl++] = 0; sha1_block(&c, c.b); c.bl = 0; }
    while (c.bl < 56) c.b[c.bl++] = 0;
    for (int i = 7; i >= 0; i--) c.b[c.bl++] = (uint8_t)(c.n >> (i * 8));
    sha1_block(&c, c.b);
    for (int i = 0; i < 5; i++)
    {
        out[i * 4] = (uint8_t)(c.h[i] >> 24); out[i * 4 + 1] = (uint8_t)(c.h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c.h[i] >> 8); out[i * 4 + 3] = (uint8_t)c.h[i];
    }
}

static void base64(const uint8_t *in, size_t len, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < len ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? T[v & 63] : '=';
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- handshake */

size_t ws_request_length(const char *buf, size_t len)
{
    for (size_t i = 3; i < len; i++)
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') return i + 1;
    return 0;
}

/* The value of a header (case-insensitive name), trimmed; false if absent */
static bool header_value(const char *req, size_t len, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl + 1 < len; i++)
    {
        if (i > 0 && req[i - 1] != '\n') continue;
        size_t k = 0;
        while (k < nl && tolower((unsigned char)req[i + k]) == tolower((unsigned char)name[k])) k++;
        if (k < nl || req[i + nl] != ':') continue;
        size_t p = i + nl + 1, o = 0;
        while (p < len && (req[p] == ' ' || req[p] == '\t')) p++;
        while (p < len && req[p] != '\r' && req[p] != '\n' && o + 1 < cap) out[o++] = req[p++];
        while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
        out[o] = 0;
        return true;
    }
    return false;
}

bool ws_header(const char *request, size_t len, const char *name, char *out, size_t cap)
{
    return header_value(request, len, name, out, cap);
}

bool ws_handshake(const char *request, size_t len, char *response, size_t cap, size_t *response_len)
{
    char key[128], upgrade[64];
    bool is_ws = len > 4 && !memcmp(request, "GET ", 4) &&
                 header_value(request, len, "Upgrade", upgrade, sizeof upgrade) &&
                 header_value(request, len, "Sec-WebSocket-Key", key, sizeof key);
    for (char *c = upgrade; is_ws && *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!is_ws || strcmp(upgrade, "websocket") != 0)
    {
        int n = snprintf(response, cap,
                         "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n"
                         "Content-Length: 46\r\n\r\nThis port takes game connections (WebSocket).\n");
        *response_len = n > 0 ? (size_t)n : 0;
        return false;
    }
    char joined[192];
    snprintf(joined, sizeof joined, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    uint8_t digest[20];
    sha1(joined, strlen(joined), digest);
    char accept[32];
    base64(digest, sizeof digest, accept);
    int n = snprintf(response, cap,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    *response_len = n > 0 ? (size_t)n : 0;
    return true;
}

/* ---------------------------------------------------------------- frames */

int ws_parse(uint8_t *buf, size_t len, WsFrame *out)
{
    if (len < 2) return 0;
    if (buf[0] & 0x70) return -1;              /* reserved bits: no extensions agreed */
    if (!(buf[1] & 0x80)) return -1;           /* a client must mask its frames       */
    uint64_t plen = buf[1] & 0x7F;
    size_t pos = 2;
    if (plen == 126)
    {
        if (len < 4) return 0;
        plen = (uint64_t)buf[2] << 8 | buf[3];
        pos = 4;
    }
    else if (plen == 127)
    {
        if (len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = plen << 8 | buf[2 + i];
        pos = 10;
    }
    if (plen > WS_MAX_MESSAGE) return -1;
    if (len < pos + 4 + plen) return 0;
    const uint8_t *mask = buf + pos;
    uint8_t *payload = buf + pos + 4;
    for (size_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
    out->opcode = buf[0] & 0x0F;
    out->fin = (buf[0] & 0x80) != 0;
    out->payload = payload;
    out->payload_len = (size_t)plen;
    return (int)(pos + 4 + plen);
}

size_t ws_frame_header(uint8_t *out, uint8_t opcode, size_t len)
{
    out[0] = (uint8_t)(0x80 | opcode);
    if (len < 126)
    {
        out[1] = (uint8_t)len;
        return 2;
    }
    if (len < 65536)
    {
        out[1] = 126;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)len;
        return 4;
    }
    out[1] = 127;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)((uint64_t)len >> (56 - i * 8));
    return 10;
}
