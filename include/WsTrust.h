#ifndef WSTRUST_H
#define WSTRUST_H
/* Trusted WebSocket proxies, shared by the server's gateway (WebGate.c) and the relay.

   The phone version of the game is an installed web app, which browsers only allow over
   https, and an https page may only open wss:// connections. Game servers and relays speak
   plain ws://, so the site the app comes from (sonicmb.dr2.site) forwards the connection:
   wss://<site>/ws/<host>/<port> -> ws://<host>:<port>/. Everyone using it then arrives from
   the site's own address. For connections from a trusted proxy the player's address comes
   from the X-Real-IP header instead, so bans, logs and the per-address limits still see each
   player; from anyone else the header is ignored. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* names: host names, IPv4 / IPv6 addresses or IPv4 networks (a.b.c.d/nn), separated by
   commas or spaces. Host names are looked up now and again every 10 minutes, in a thread of
   its own. Calling it again replaces the list. */
void ws_trust_start(const char *names);

/* the connection's address (text form) belongs to a trusted proxy */
bool ws_trust_is(const char *ip);

/* The player's address a trusted proxy sent (X-Real-IP) in the HTTP upgrade request: true and
   the address in normalized text form when the header is there and is a valid IPv4 / IPv6
   address. */
bool ws_trust_forwarded(const char *request, size_t len, char *out, size_t cap);

#endif
