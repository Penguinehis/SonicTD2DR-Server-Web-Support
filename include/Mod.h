#ifndef MOD_H
#define MOD_H
/* Mods for the web version (sonicmods): a .dmod package (a zip with stored entries) named in
   Mod.json next to the server. Its server.lua runs in a sandboxed Lua 5.4 per lobby and gets the
   match's events (hooks) and the game.* functions; its maps join the map vote as map ids
   MOD_MAP_BASE + n. Only web players that can load mods (IDENTITY ends with 'W' and the mod api)
   may join while a mod is on; the others are told why and turned away.

   Mod.json:
     { "enabled": true, "file": "mods/hot_potato-1.0.0.dmod",
       "url": "https://modsc.dr2.site/dl/hot_potato/1.0.0.dmod",
       "settings": { "rounds": 3 } }
*/
#include <Server.h>
#include <stdbool.h>
#include <stdint.h>

#define MOD_API 2              /* the newest the server knows: 2 adds objects (modobj.c) */
#define MOD_MAP_BASE 32
#define MOD_MAX_MAPS 16
#define MOD_MAX_SYNC 128

typedef struct
{
	char file[64];        /* maps/<file>.json, in the package's (sorted) order */
	char name[48];
	uint16_t time;        /* seconds */
	uint8_t rings;        /* ring spots (obj ring) */
	uint8_t exits;        /* big ring spots */
} ModMap;

typedef struct
{
	bool active;
	int api;              /* the mod's (manifest api): players need at least this */
	char id[40];
	char name[48];
	char version[16];
	char sha256[65];
	char url[200];
	uint32_t size;
	bool maps_only;
	bool bots;
	int exes;        /* EXEs a round: 0 (survivors only) .. 3; 1 the game's */
	bool ffa;        /* free-for-all: everyone an EXE */
	int map_count;
	ModMap maps[MOD_MAX_MAPS];
	uint8_t *data;        /* the package */
	size_t data_len;
} ModInfo;

extern ModInfo g_mod;

bool mod_global_init(void);             /* reads Mod.json and the package (once, at start) */
bool mod_server_init(Server *server);   /* the lobby's Lua state */
void mod_server_free(Server *server);

bool mod_client_ok(PeerData *v);        /* may this player join (always true without a mod) */
void mod_refuse(PeerData *v);           /* tell it why, then turn it away */
void mod_send_info(PeerData *v);        /* the mod's id / version / hash / url, its synced values */
bool mod_is_map(int map);
const ModMap *mod_map(int map);
void mod_add_vote_maps(uint8_t *source, int *count, int cap);  /* the map vote's choices */
void mod_round_time(Server *server);    /* a mod map's time */
bool mod_bots_allowed(Server *server);

/* hooks (server.lua on_*) */
void mod_on_join(PeerData *v);
void mod_on_leave(PeerData *v);
void mod_on_lobby(Server *server);
void mod_on_round_start(Server *server);
void mod_on_round_end(Server *server, int ending);
void mod_on_tick(Server *server);
void mod_on_death(PeerData *v);
void mod_on_escape(PeerData *v);
void mod_on_ring(PeerData *v);
bool mod_on_chat(PeerData *v, const char *text);   /* false: the message is hidden */
void mod_on_hurt(PeerData *attacker, PeerData *target, int damage, int stun);
void mod_on_hp(PeerData *v, int old_hp, int hp);
void mod_on_demonize(PeerData *v);
void mod_on_packet(PeerData *v, Packet *packet, uint8_t type);  /* CLIENT_MOD_READY / CLIENT_MOD_EVENT */
uint16_t mod_pick_exe(Server *server, uint16_t chosen);         /* game.choose_exe / on_choose_exe */

#endif
