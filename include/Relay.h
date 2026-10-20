#ifndef RELAY_H
#define RELAY_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Tunnel client built into the server. When enabled it dials out to the
   directory, obtains a ticket, and keeps a relay tunnel open so players can
   join without the host forwarding any ports. */
bool relay_start(void);
bool relay_is_enabled(void);
bool relay_is_online(void);

/* The port a player must actually dial to reach a given lobby. Behind a relay
   that is the relay's public port, which is what has to go into a lobby-change
   packet; the server's own listening port is unreachable from outside. Falls
   back to the local port when no relay is in use. */
uint16_t relay_public_port(int lobby_index);

/* Maps a loopback peer address back to the synthetic address the relay
   assigned, so bans, operators and logs record that instead of 127.0.0.1.
   Returns false when the peer did not arrive through the tunnel. */
bool relay_lookup_ip(uint32_t host_be, uint16_t port, char *out, size_t cap);

/* Resolves a hostname (or accepts a literal address) to an IPv4 address in
   network order. Needed because the static Linux builds cannot use glibc's
   resolver. */
bool relay_resolve(const char *host, uint32_t *out_ip_be);

#endif
