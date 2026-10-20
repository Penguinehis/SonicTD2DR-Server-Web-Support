#ifndef WEBRTC_H
#define WEBRTC_H
/* WebRTC data channels for web players (WebGate). The browser sends a WebRTC offer as a text
   message on its WebSocket; the answer goes back the same way; then the ENet datagrams travel
   on an unreliable, unordered data channel: UDP-like, nothing queues behind a slow packet.
   Until the channel is open (or when WebRTC is not possible) the WebSocket carries them.
   Built only with HAVE_WEBRTC (libdatachannel); otherwise every call does nothing. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET wrtc_sock_t;
#else
typedef int wrtc_sock_t;
#endif

#define WRTC_PORT_OFFSET 10000   /* the UDP port of a lobby's data channels: game port + this */

bool wrtc_available(void);
void wrtc_init(int slots);
/* An offer from WebGate client slot (gen tells reused slots apart): datagrams from its channel
   go to the game port from udp. False if it could not start. */
bool wrtc_open(int slot, unsigned gen, const char *offer, uint16_t mux_port, wrtc_sock_t udp, uint16_t game_port);
/* The answer for a slot, once ready (copied into out, then forgotten): WebGate sends it */
bool wrtc_take_answer(int slot, unsigned gen, char *out, size_t cap);
bool wrtc_pending(void);        /* an answer is being made: WebGate polls more often */
/* A datagram for the browser on its channel; false when there is no open channel */
bool wrtc_send(int slot, unsigned gen, const uint8_t *data, size_t len);
void wrtc_close(int slot);      /* before WebGate closes the slot's udp socket */
/* When the browser last sent on its channel (0: never): the WebSocket is quiet while it is used */
time_t wrtc_last_seen(int slot, unsigned gen);

#endif
