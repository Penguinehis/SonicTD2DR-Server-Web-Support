#ifndef WEBGATE_H
#define WEBGATE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* WebSocket gateway for browser players (the web version of the game): listens on TCP on the
   same port numbers as the lobbies' UDP ports and turns each connection into an ordinary
   loopback ENet peer. On unless "web_clients" is false in Config.json. */
bool webgate_start(void);

/* Maps a loopback peer address back to the browser's real address, so bans, operators and
   logs record that. Returns false when the peer did not come through the gateway. */
bool webgate_lookup_ip(uint32_t host_be, uint16_t port, char *out, size_t cap);

#endif
