#ifndef WSPROTO_H
#define WSPROTO_H
/* The small part of WebSocket (RFC 6455) that browser players need, shared by the server's
   gateway (WebGate.c) and the relay. A browser cannot send UDP, so the web version of the game
   sends each ENet datagram as one binary WebSocket message to the same port number as the
   UDP one (TCP and UDP ports are separate, both can be open on 8606). No TLS: the web game is
   served over http and opens ws://host:port. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_MAX_REQUEST   4096   /* the HTTP upgrade request must fit */
#define WS_MAX_MESSAGE   8192   /* an ENet datagram is at most 1400 bytes; a WebRTC offer a few KB */

enum { WS_OP_CONT = 0x0, WS_OP_TEXT = 0x1, WS_OP_BINARY = 0x2, WS_OP_CLOSE = 0x8, WS_OP_PING = 0x9, WS_OP_PONG = 0xA };

/* Answer an HTTP request (the complete header block, up to the empty line). Returns true and
   the 101 response for a WebSocket upgrade; false and a plain HTTP error response otherwise. */
bool ws_handshake(const char *request, size_t len, char *response, size_t cap, size_t *response_len);

/* The value of a header of the request (name case-insensitive), trimmed; false if absent */
bool ws_header(const char *request, size_t len, const char *name, char *out, size_t cap);

/* Header end: the length of the request through "\r\n\r\n", or 0 while incomplete */
size_t ws_request_length(const char *buf, size_t len);

typedef struct
{
    uint8_t  opcode;
    bool     fin;
    uint8_t *payload;      /* inside the caller's buffer, unmasked in place */
    size_t   payload_len;
} WsFrame;

/* One frame from a client (masked). Returns the bytes it used, 0 while incomplete, -1 for a
   protocol error (unmasked, too large, reserved bits). */
int ws_parse(uint8_t *buf, size_t len, WsFrame *out);

/* The header of a server frame (unmasked, FIN set) for a payload of len bytes; at most 10 bytes */
size_t ws_frame_header(uint8_t *out, uint8_t opcode, size_t len);

#endif
