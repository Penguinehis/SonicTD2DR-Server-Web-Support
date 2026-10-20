#ifndef MODOBJ_H
#define MODOBJ_H
/* Mods' objects (mod API 2): object types made in the Mod Studio (objects/<name>.json and the events in
   objects/<name>.lua: create, step, touch, leave, destroy), their instances placed in a mod map or made by
   a script (game.spawn), moved and run 60 times a second. One file for both sides: the server
   (betterserver, the real game) and the web game's local rules (the Studio's Run, no server). The host
   reads the instances' state after each tick: the server sends the changes, the web game draws them.

   This file is the same in sonicmods/engine, betterserver(bots)/disasterserver and sonicweb / sonicios
   source: edit it in sonicmods/engine and copy it (sonicmods/engine/copy.sh). */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct lua_State lua_State;
typedef struct cJSON cJSON;

#define OBJ_MAX 256          /* instances at once */
#define OBJ_TYPES 64
#define OBJ_PLAYERS 32       /* players an instance can be touching at once */
#define OBJ_SPRITE_LEN 64

/* a player as the objects see one (touches) */
#define OBJ_PLAYER_BOX_L (-10.0f)   /* the player's box from its x, y (its sprite's origin is near the middle) */
#define OBJ_PLAYER_BOX_T (-16.0f)
#define OBJ_PLAYER_BOX_W 20.0f
#define OBJ_PLAYER_BOX_H 36.0f

enum { OBJ_EV_CREATE, OBJ_EV_STEP, OBJ_EV_TOUCH, OBJ_EV_LEAVE, OBJ_EV_DESTROY, OBJ_EV_COUNT };

typedef struct
{
	char name[32];               /* objects/<name>.json */
	char sprite[OBJ_SPRITE_LEN]; /* a sprite of the mod or the game ("": none) */
	float box[4];                /* left, top, width, height from x, y */
	bool front;
	uint32_t color;              /* its colour and blend at first ("color", "blend" in its .json) */
	bool blend_add;                  /* drawn over the players */
	int env;                     /* registry: the environment of its events (objects/<name>.lua) */
	int vars;                    /* registry: its starting fields (the json's "vars"), or LUA_NOREF */
	bool has[OBJ_EV_COUNT];
} ObjType;

typedef struct
{
	bool used;
	bool dead;                   /* destroyed in this tick (its slot is freed after it) */
	uint16_t id;
	uint8_t type;
	int ref;                     /* registry: its table (self) */
	/* its state, read back from self after each tick */
	float x, y, vx, vy, xscale, yscale, alpha, angle;
	uint32_t color;              /* 0xRRGGBB it is tinted with (0xFFFFFF: its own colours; GameMaker's image_blend) */
	bool blend_add;              /* drawn adding its colours (a glow) */
	char sprite[OBJ_SPRITE_LEN];
	int frame;                   /* -1: animated by the sprite's speed */
	bool visible;
	bool solid_off;              /* self.solid = false: players pass through it (when its kind is solid) */
	uint16_t touching[OBJ_PLAYERS];
	int ntouch;
} ObjInst;

typedef struct
{
	uint16_t id;
	float x, y;
	bool alive;                  /* in the round, not dead, not escaped: they can touch things */
	float bl, bt, bw, bh;        /* their box from x, y (the game's: their sprite's); bw 0: OBJ_PLAYER_BOX_* */
} ObjPlayer;

enum { OBJ_CREATED = 1, OBJ_DESTROYED };

typedef struct
{
	void *ud;
	/* a file of the package (NULL: not there) */
	const uint8_t *(*file)(void *ud, const char *name, uint32_t *size);
	/* calls the function under nargs arguments (as lua_pcall, with the host's time budget); false: it failed
	   (the host logged it and popped the error) */
	bool (*call)(void *ud, lua_State *L, int nargs, int nres, const char *where);
	/* pushes the player table of this id (as game.player gives it), or nil */
	void (*push_player)(void *ud, lua_State *L, uint16_t id);
	/* the players that can touch objects now */
	int (*players)(void *ud, ObjPlayer *out, int cap);
	/* a problem with the package's objects (logged; the object is left out) */
	void (*warn)(void *ud, const char *text);
	/* an instance was made / destroyed (optional) */
	void (*changed)(void *ud, ObjInst *o, int what);
} ObjHost;

typedef struct
{
	ObjHost host;
	lua_State *L;
	int ntypes;
	ObjType types[OBJ_TYPES];
	ObjInst inst[OBJ_MAX];
	uint16_t next_id;
	bool clearing;
} ObjEngine;

/* reads objects/<name>.json + .lua for each entry of the package (names: the package's file names) and adds
   game.spawn / destroy / objects / object to the "game" table (it must be there). False: out of memory. */
bool obj_init(ObjEngine *e, lua_State *L, const ObjHost *host, const char *const *names, int count);
int obj_type_index(const ObjEngine *e, const char *name);    /* -1: none */
/* the map's placed objects ({"type": "object", "object": "<name>", "x", "y"} in its "objects") */
void obj_spawn_map(ObjEngine *e, const cJSON *objects);
ObjInst *obj_spawn(ObjEngine *e, int type, float x, float y);   /* (calls create; NULL: too many) */
void obj_tick(ObjEngine *e);                                   /* 60 a second, in a round */
void obj_clear(ObjEngine *e);                                  /* the round ended: all gone (no events) */
ObjInst *obj_find(ObjEngine *e, uint16_t id);
int obj_count(const ObjEngine *e);

#endif
