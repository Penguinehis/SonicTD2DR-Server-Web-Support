#ifndef PALETTE_H
#define PALETTE_H
#include <Packet.h>
#include <Server.h>
#include <stdbool.h>

bool palette_player_validate(PeerData* v, Packet* packet);
/* A player palette of any colours: its sender's own id, 1..60 colours, every colour opaque (a
   transparent one could make a character invisible). The default skin sends one (0,0,0,0) entry
   (changes nothing): allowed as a whole list, and as "to" only after such a "from". */
bool palette_player_check(PeerData* v, Packet* packet);
bool palette_player_reset(PeerData* v);

#endif