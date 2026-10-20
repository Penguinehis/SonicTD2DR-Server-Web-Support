/* Mods (see Mod.h): the package, the lobby's sandboxed Lua, the game.* functions and the hooks.

   The package is a zip whose entries are all stored (sonicmods/tools/dmod.py makes them so), read
   straight from memory. Each lobby (Server) has its own Lua state, used only by that lobby's
   worker thread. A script error is logged and the hook skipped; after too many errors the mod's
   hooks rest until the next round. Lua gets base / string / table / math / utf8 / coroutine
   only (no io, os, debug, files, bytecode), a memory cap and an instruction budget per call. */
#include <Mod.h>
#include <Config.h>
#include <Maps.h>
#include <Lib.h>
#include <Log.h>
#include <RelayProto.h>
#include <cJSON.h>
#include <States.h>
#include <modobj.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool game_end(Server *server, Ending ending, bool achiv);

ModInfo g_mod;

#define MOD_FILE_JSON "Mod.json"
#define MOD_MEMORY_CAP (48u * 1024 * 1024)
#define MOD_INSTR_STEP 10000
#define MOD_INSTR_LIMIT 300      /* * MOD_INSTR_STEP instructions per hook call */
#define MOD_TICK_LIMIT 600       /* * MOD_INSTR_STEP instructions for all the calls of one tick */
#define MOD_MAX_TIMERS 1000
#define MOD_MAX_ERRORS 25        /* in a round; then the hooks rest until the next one */
#define MOD_TIME_MAX 1090        /* seconds: SERVER_GAME_TIME_SYNC carries 16-bit ticks */
#define MOD_LOG_PER_SEC 200      /* game.log / print lines a second (the rest are skipped) */
#define MOD_SAY_PER_SEC 20       /* game.say / game.tell a second */
#define MOD_OUT_PER_SEC 240      /* mod packets a second to one player (not the objects': their own pace) */
#define MOD_OBJ_SEND_TICKS 3     /* the objects' changes go out 20 times a second */

/* ------------------------------------------------------------------ the package (stored zip) */

typedef struct
{
	char name[128];
	uint32_t offset;   /* of the data */
	uint32_t size;
} ModEntry;

static ModEntry *g_entries;
static int g_entry_count;

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static bool zip_index(const uint8_t *d, size_t len)
{
	if (len < 22)
		return false;
	size_t eocd = SIZE_MAX;
	size_t lo = len > 65557 ? len - 65557 : 0;
	for (size_t i = len - 22 + 1; i-- > lo;)
		if (rd32(d + i) == 0x06054b50)
		{
			eocd = i;
			break;
		}
	if (eocd == SIZE_MAX)
		return false;
	uint32_t count = rd16(d + eocd + 10), cd = rd32(d + eocd + 16);
	if (count > 4000 || cd >= len)
		return false;
	g_entries = (ModEntry *)calloc(count ? count : 1, sizeof(ModEntry));
	if (!g_entries)
		return false;
	size_t p = cd;
	for (uint32_t i = 0; i < count; i++)
	{
		if (p + 46 > len || rd32(d + p) != 0x02014b50)
			return false;
		uint32_t method = rd16(d + p + 10), csize = rd32(d + p + 20), usize = rd32(d + p + 24);
		uint32_t nlen = rd16(d + p + 28), elen = rd16(d + p + 30), clen = rd16(d + p + 32);
		uint32_t local = rd32(d + p + 42);
		if (p + 46 + nlen > len)
			return false;
		if (method != 0 || csize != usize)
		{
			Warn("[mod] %.*s is compressed: packages must be stored (pack them with dmod.py)", (int)nlen, d + p + 46);
			return false;
		}
		if (local + 30 > len || rd32(d + local) != 0x04034b50)
			return false;
		uint32_t data = local + 30 + rd16(d + local + 26) + rd16(d + local + 28);
		if ((size_t)data + usize > len)
			return false;
		if (nlen > 0 && nlen < sizeof(g_entries[0].name) && d[p + 46 + nlen - 1] != '/')
		{
			ModEntry *e = &g_entries[g_entry_count++];
			memcpy(e->name, d + p + 46, nlen);
			e->name[nlen] = 0;
			e->offset = data;
			e->size = usize;
		}
		p += 46 + nlen + elen + clen;
	}
	return true;
}

static const uint8_t *mod_file(const char *name, uint32_t *size)
{
	for (int i = 0; i < g_entry_count; i++)
		if (!strcmp(g_entries[i].name, name))
		{
			if (size)
				*size = g_entries[i].size;
			return g_mod.data + g_entries[i].offset;
		}
	return NULL;
}

static cJSON *mod_json(const char *name)
{
	uint32_t size;
	const uint8_t *d = mod_file(name, &size);
	if (!d)
		return NULL;
	return cJSON_ParseWithLength((const char *)d, size);
}

static uint8_t *read_all(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 64L * 1024 * 1024)
	{
		fclose(f);
		return NULL;
	}
	uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf)
	{
		buf[n] = 0;
		*len = (size_t)n;
	}
	return buf;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	snprintf(dst, cap, "%s", src ? src : "");
}

/* the owner's settings (Mod.json) over the manifest's defaults, kept as JSON for each lobby */
static cJSON *g_settings;


/* The bots' map of a mod map (bots/maps/zz_mod_<n>.json, BotMap.h's hand-made format): the ground,
   jump-through platforms (their top line), slopes as 1 px columns, spawns, rings, big ring spots */
static void rect_out(FILE *f, bool *first, double x0, double y0, double x1, double y1)
{
	fprintf(f, "%s[%.0f,%.0f,%.0f,%.0f]", *first ? "" : ",", x0, y0, x1, y1);
	*first = false;
}

static void points_out(FILE *f, cJSON *objs, const char *type)
{
	bool first = true;
	cJSON *o;
	cJSON_ArrayForEach(o, objs)
	{
		cJSON *t = cJSON_GetObjectItem(o, "type");
		if (!cJSON_IsString(t) || strcmp(t->valuestring, type))
			continue;
		cJSON *x = cJSON_GetObjectItem(o, "x"), *y = cJSON_GetObjectItem(o, "y");
		if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y))
			continue;
		fprintf(f, "%s[%.0f,%.0f]", first ? "" : ",", x->valuedouble, y->valuedouble);
		first = false;
	}
}

static double num_at(cJSON *a, int i)
{
	cJSON *v = cJSON_GetArrayItem(a, i);
	return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void write_bot_map(int n, const char *file)
{
	cJSON *mj = mod_json(file);
	if (!mj)
		return;
	char path[96];
	snprintf(path, sizeof path, "bots/maps/zz_mod_%d.json", n);
	FILE *f = fopen(path, "wb");
	if (!f)
	{
		cJSON_Delete(mj);
		return;
	}
	cJSON *w = cJSON_GetObjectItem(mj, "width"), *h = cJSON_GetObjectItem(mj, "height");
	fprintf(f, "{\"map_index\": %d, \"room\": \"mod_%s_%d\", \"name\": \"%s\", \"width\": %d, \"height\": %d,\n",
		MOD_MAP_BASE + n, g_mod.id, n, g_mod.id, cJSON_IsNumber(w) ? w->valueint : 3200, cJSON_IsNumber(h) ? h->valueint : 1024);
	fprintf(f, "\"floor_rects\": [");
	bool first = true;
	{	/* the map's left and right edges are walls (as in the games) */
		double mw = cJSON_IsNumber(w) ? w->valuedouble : 3200, mh = cJSON_IsNumber(h) ? h->valuedouble : 1024;
		rect_out(f, &first, -64, -4096, 0, mh + 4096);
		rect_out(f, &first, mw, -4096, mw + 64, mh + 4096);
	}
	cJSON *r, *objs = cJSON_GetObjectItem(mj, "objects");
	cJSON_ArrayForEach(r, cJSON_GetObjectItem(mj, "solids"))
		rect_out(f, &first, num_at(r, 0), num_at(r, 1), num_at(r, 0) + num_at(r, 2), num_at(r, 1) + num_at(r, 3));
	cJSON *o;
	cJSON_ArrayForEach(o, objs)
	{
		cJSON *t = cJSON_GetObjectItem(o, "type");
		if (cJSON_IsString(t) && !strcmp(t->valuestring, "solid"))
		{
			double x = cJSON_GetNumberValue(cJSON_GetObjectItem(o, "x")), y = cJSON_GetNumberValue(cJSON_GetObjectItem(o, "y"));
			cJSON *ow = cJSON_GetObjectItem(o, "w"), *oh = cJSON_GetObjectItem(o, "h");
			rect_out(f, &first, x, y, x + (cJSON_IsNumber(ow) ? ow->valuedouble : 64), y + (cJSON_IsNumber(oh) ? oh->valuedouble : 16));
		}
	}
	cJSON_ArrayForEach(r, cJSON_GetObjectItem(mj, "slopes"))
	{
		double x = num_at(r, 0), y = num_at(r, 1), sw = num_at(r, 2), sh = num_at(r, 3);
		int dir = (int)num_at(r, 4), shape = cJSON_GetArraySize(r) == 6 ? (int)num_at(r, 5) : 0;
		for (int i = 0; i < (int)sw; i++)
		{
			/* the column's share of the height: from its low side, by its shape (as the games' masks:
			   1 a quarter pipe, 2 a hill's top, else straight) */
			double u = dir >= 0 ? (i + 1) / sw : (sw - i) / sw, k;
			if (u > 1)
				u = 1;
			k = shape == 1 ? 1 - sqrt(1 - u * u) : shape == 2 ? sqrt(1 - (1 - u) * (1 - u)) : u;
			rect_out(f, &first, x + i, y + sh - sh * k, x + i + 1, y + sh);
		}
	}
	fprintf(f, "],\n\"platform_rects\": [");
	first = true;
	cJSON_ArrayForEach(r, cJSON_GetObjectItem(mj, "platforms"))
		rect_out(f, &first, num_at(r, 0), num_at(r, 1), num_at(r, 0) + num_at(r, 2), num_at(r, 1) + 1);
	cJSON_ArrayForEach(o, objs)
	{
		cJSON *t = cJSON_GetObjectItem(o, "type");
		if (cJSON_IsString(t) && !strcmp(t->valuestring, "platform"))
		{
			double x = cJSON_GetNumberValue(cJSON_GetObjectItem(o, "x")), y = cJSON_GetNumberValue(cJSON_GetObjectItem(o, "y"));
			cJSON *ow = cJSON_GetObjectItem(o, "w");
			rect_out(f, &first, x, y, x + (cJSON_IsNumber(ow) ? ow->valuedouble : 64), y + 1);
		}
	}
	fprintf(f, "],\n\"spawns\": [");
	points_out(f, objs, "spawn");
	fprintf(f, "],\n\"exe_spawns\": [");
	points_out(f, objs, "exe_spawn");
	fprintf(f, "],\n\"ring_spawners\": [");
	points_out(f, objs, "ring");
	fprintf(f, "],\n\"bigring_spawns\": [");
	points_out(f, objs, "exit");
	fprintf(f, "]}\n");
	fclose(f);
	cJSON_Delete(mj);
}

static int map_file_cmp(const void *a, const void *b)
{
	return strcmp(((const ModMap *)a)->file, ((const ModMap *)b)->file);
}

static bool mod_map_init_cb(Server *server)
{
	RAssert(map_time(server, 180, 0));
	RAssert(map_ring(server, 5));
	return true;
}

static bool mod_map_tick_cb(Server *server) { return map_tick(server); }  /* (the big ring: last minute) */
static bool mod_map_tcp_cb(PeerData *v, Packet *packet) { (void)v; (void)packet; return true; }
static bool mod_map_left_cb(PeerData *v) { (void)v; return true; }

bool mod_global_init(void)
{
	memset(&g_mod, 0, sizeof g_mod);
	size_t len;
	char *json = (char *)read_all(MOD_FILE_JSON, &len);
	if (!json)
		return true;  /* no mod */
	cJSON *cfg = cJSON_Parse(json);
	free(json);
	if (!cfg)
	{
		Warn("[mod] " MOD_FILE_JSON " is not valid JSON: no mod");
		return true;
	}
	cJSON *en = cJSON_GetObjectItem(cfg, "enabled");
	cJSON *file = cJSON_GetObjectItem(cfg, "file");
	if ((en && cJSON_IsFalse(en)) || !cJSON_IsString(file))
	{
		cJSON_Delete(cfg);
		return true;
	}
	g_mod.data = read_all(file->valuestring, &g_mod.data_len);
	if (!g_mod.data || !zip_index(g_mod.data, g_mod.data_len))
	{
		Err("[mod] can't read the package %s: the server runs without it", file->valuestring);
		cJSON_Delete(cfg);
		return true;
	}
	cJSON *man = mod_json("manifest.json");
	cJSON *id = man ? cJSON_GetObjectItem(man, "id") : NULL;
	cJSON *api = man ? cJSON_GetObjectItem(man, "api") : NULL;
	if (!cJSON_IsString(id) || !cJSON_IsNumber(api) || api->valueint < 1 || api->valueint > MOD_API)
	{
		Err("[mod] %s has no usable manifest.json (api 1-%d): the server runs without it (a newer server?)", file->valuestring, MOD_API);
		cJSON_Delete(man);
		cJSON_Delete(cfg);
		return true;
	}
	copy_str(g_mod.id, sizeof g_mod.id, id->valuestring);
	g_mod.api = api->valueint;
	cJSON *it;
	if ((it = cJSON_GetObjectItem(man, "name")) && cJSON_IsString(it))
		copy_str(g_mod.name, sizeof g_mod.name, it->valuestring);
	if ((it = cJSON_GetObjectItem(man, "version")) && cJSON_IsString(it))
		copy_str(g_mod.version, sizeof g_mod.version, it->valuestring);
	it = cJSON_GetObjectItem(man, "maps");
	g_mod.maps_only = cJSON_IsString(it) && !strcmp(it->valuestring, "only");
	it = cJSON_GetObjectItem(man, "bots");
	g_mod.bots = !it || !cJSON_IsFalse(it);
	it = cJSON_GetObjectItem(man, "exes");
	g_mod.exes = cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 3 ? it->valueint : 1;
	g_mod.ffa = cJSON_IsTrue(cJSON_GetObjectItem(man, "free_for_all"));
	if ((it = cJSON_GetObjectItem(cfg, "url")) && cJSON_IsString(it))
		copy_str(g_mod.url, sizeof g_mod.url, it->valuestring);
	g_mod.size = (uint32_t)g_mod.data_len;

	SHA256 sh;
	uint8_t dig[32];
	sha256_init(&sh);
	sha256_update(&sh, g_mod.data, g_mod.data_len);
	sha256_final(&sh, dig);
	for (int i = 0; i < 32; i++)
		snprintf(g_mod.sha256 + i * 2, 3, "%02x", dig[i]);

	/* settings: the manifest's defaults, the owner's values over them (same type only) */
	g_settings = cJSON_CreateObject();
	cJSON *defs = cJSON_GetObjectItem(man, "settings");
	cJSON *own = cJSON_GetObjectItem(cfg, "settings");
	cJSON *s;
	cJSON_ArrayForEach(s, defs)
	{
		cJSON *key = cJSON_GetObjectItem(s, "key"), *def = cJSON_GetObjectItem(s, "default");
		if (!cJSON_IsString(key) || !def)
			continue;
		cJSON *mine = own ? cJSON_GetObjectItem(own, key->valuestring) : NULL;
		cJSON *use = def;
		if (mine && ((cJSON_IsNumber(mine) && cJSON_IsNumber(def)) || (cJSON_IsBool(mine) && cJSON_IsBool(def)) ||
			(cJSON_IsString(mine) && cJSON_IsString(def))))
			use = mine;
		cJSON_AddItemToObject(g_settings, key->valuestring, cJSON_Duplicate(use, true));
	}

	/* the maps, in file order (the web game numbers them the same way) */
	for (int i = 0; i < g_entry_count && g_mod.map_count < MOD_MAX_MAPS; i++)
	{
		const char *n = g_entries[i].name;
		size_t nl = strlen(n);
		if (strncmp(n, "maps/", 5) || nl < 10 || strcmp(n + nl - 5, ".json"))
			continue;
		cJSON *mj = mod_json(n);
		if (!mj)
			continue;
		ModMap *m = &g_mod.maps[g_mod.map_count++];
		copy_str(m->file, sizeof m->file, n);
		cJSON *nm = cJSON_GetObjectItem(mj, "name"), *tm = cJSON_GetObjectItem(mj, "time");
		copy_str(m->name, sizeof m->name, cJSON_IsString(nm) ? nm->valuestring : n + 5);
		m->time = cJSON_IsNumber(tm) && tm->valueint >= 10 ? (uint16_t)(tm->valueint > MOD_TIME_MAX ? MOD_TIME_MAX : tm->valueint) : 180;
		cJSON *o;
		cJSON_ArrayForEach(o, cJSON_GetObjectItem(mj, "objects"))
		{
			cJSON *t = cJSON_GetObjectItem(o, "type");
			if (!cJSON_IsString(t))
				continue;
			if (!strcmp(t->valuestring, "ring") && m->rings < 255)
				m->rings++;
			else if (!strcmp(t->valuestring, "exit") && m->exits < 255)
				m->exits++;
		}
		cJSON_Delete(mj);
	}
	qsort(g_mod.maps, (size_t)g_mod.map_count, sizeof(ModMap), map_file_cmp);
	for (int i = 0; i < MOD_MAX_MAPS; i++)
	{
		char old[64];
		snprintf(old, sizeof old, "bots/maps/zz_mod_%d.json", i);
		remove(old);  /* (a previous mod's) */
	}
	if (g_mod.bots)
		for (int i = 0; i < g_mod.map_count; i++)
			write_bot_map(i, g_mod.maps[i].file);
	for (int i = 0; i < g_mod.map_count; i++)
	{
		static char names[MOD_MAX_MAPS][64];
		snprintf(names[i], sizeof names[i], CLRCODE_BLU "%s", g_mod.maps[i].name);
		Map *mp = &g_mapList[MOD_MAP_BASE + i];
		mp->name = names[i];
		mp->cb.init = mod_map_init_cb;
		mp->cb.tick = mod_map_tick_cb;
		mp->cb.tcp_msg = mod_map_tcp_cb;
		mp->cb.left = mod_map_left_cb;
		mp->spawn_red_rings = 0;
		mp->ring_count = g_mod.maps[i].rings ? g_mod.maps[i].rings : 1;
	}

	g_mod.active = true;
	Info(LOG_YLW "Mod " LOG_GRN "%s %s" LOG_RST " (%s): %d map(s)%s, web players only",
		g_mod.id, g_mod.version, g_mod.name, g_mod.map_count, g_mod.maps_only ? " (only these)" : "");
	cJSON_Delete(man);
	cJSON_Delete(cfg);
	return true;
}

/* ------------------------------------------------------------------ the lobby's Lua */

typedef struct
{
	lua_State *L;
	Server *server;
	size_t mem;
	int budget;
	bool in_call;            /* a script runs: the memory cap holds (not for the server's own pushes) */
	int tick_spent;          /* instruction steps used since this tick began */
	int timer_count;
	int errors;
	bool resting;
	uint32_t log_sec, say_sec;   /* rate limits: the second (server tick / TICKSPERSEC) and the count */
	int log_count, say_count;
	uint32_t frame;          /* hook ticks (timers) */
	uint32_t round_frame;    /* on_tick's frame: from 0 each round */
	uint32_t timer_seq;      /* timers run in the order they were made */
	int pending_end;         /* end_round() asked: Ending + 1 */
	uint16_t pending_exe;    /* choose_exe() asked: id + 1 */
	int sync_count;
	char sync_key[MOD_MAX_SYNC][48];
	uint8_t sync_type[MOD_MAX_SYNC];
	char sync_val[MOD_MAX_SYNC][128];
	/* objects (mod api 2): the engine, and what the players were told of each slot */
	bool has_objects;
	ObjEngine objs;
	struct ObjSent
	{
		uint16_t id;          /* 0: nothing told */
		bool created;         /* told it exists */
		float x, y, vx, vy, xscale, yscale, angle;
		char sprite[OBJ_SPRITE_LEN];
		int frame;
		bool visible, solid_off, blend_add;
		uint32_t color;
		uint8_t alpha;
	} sent[OBJ_MAX];
	uint16_t gone[OBJ_MAX];  /* destroyed since the last send */
	int gone_count;
	bool clear_pending;      /* tell them all are gone first */
	/* the mod characters' own animations now (CLIENT_MOD_ANIM), for players who come later */
	struct { uint16_t id; char sprite[48]; } anims[64];
	int anim_count;
} ModState;

static ModState *ms_of(Server *s) { return s ? (ModState *)s->mod : NULL; }

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	ModState *ms = (ModState *)ud;
	size_t old = ptr ? osize : 0;
	if (nsize == 0)
	{
		free(ptr);
		ms->mem -= old;
		return NULL;
	}
	if (ms->in_call && nsize > old && ms->mem + (nsize - old) > MOD_MEMORY_CAP)
		return NULL;
	void *p = realloc(ptr, nsize);
	if (p)
		ms->mem = ms->mem - old + nsize;
	return p;
}

static void l_hook(lua_State *L, lua_Debug *ar)
{
	(void)ar;
	ModState *ms;
	lua_getallocf(L, (void **)&ms);
	if (--ms->budget <= 0)
		luaL_error(L, "the script took too long (endless loop?)");
}

static ModState *ms_from(lua_State *L)
{
	ModState *ms;
	lua_getallocf(L, (void **)&ms);
	return ms;
}

static void ms_error(ModState *ms, const char *where)
{
	const char *msg = lua_tostring(ms->L, -1);
	Warn("[mod %s] %s: %s", g_mod.id, where, msg ? msg : "error");
	lua_pop(ms->L, 1);
	if (++ms->errors >= MOD_MAX_ERRORS && !ms->resting)
	{
		ms->resting = true;
		Err("[mod %s] too many script errors: its hooks rest until the next round", g_mod.id);
	}
}

static int l_traceback(lua_State *L)
{
	const char *msg = lua_tostring(L, 1);
	if (!msg)
		msg = lua_pushfstring(L, "(an error with a %s, not a text)", luaL_typename(L, 1));
	luaL_traceback(L, L, msg, 1);
	return 1;
}

/* call the function on the stack (below its nargs arguments) */
static bool ms_call(ModState *ms, int nargs, int nres, const char *where)
{
	lua_State *L = ms->L;
	int base = lua_gettop(L) - nargs;
	if (ms->tick_spent >= MOD_TICK_LIMIT)
	{
		lua_pop(L, nargs + 1);
		lua_pushstring(L, "the mod's scripts used too much time in this tick (skipped)");
		ms_error(ms, where);
		return false;
	}
	lua_pushcfunction(L, l_traceback);
	lua_insert(L, base);
	ms->budget = MOD_INSTR_LIMIT;
	bool was = ms->in_call;
	ms->in_call = true;
	int r = lua_pcall(L, nargs, nres, base);
	ms->in_call = was;
	ms->tick_spent += MOD_INSTR_LIMIT - (ms->budget > 0 ? ms->budget : 0);
	lua_remove(L, base);
	if (r != LUA_OK)
	{
		ms_error(ms, where);
		return false;
	}
	return true;
}

/* push the hook function if the script has it (and the mod is not resting) */
static bool hook(ModState *ms, const char *name)
{
	if (!ms || !ms->L || ms->resting)
		return false;
	lua_getglobal(ms->L, name);
	if (lua_isfunction(ms->L, -1))
		return true;
	lua_pop(ms->L, 1);
	return false;
}

/* ------------------------------------------------------------------ players */

static const char *SURV_NAMES[] = { "tails", "knuckles", "eggman", "amy", "cream", "sally" };
static const char *EXE_NAMES[] = { "exe", "chaos", "exetior", "exeller" };

static bool peer_web_ready(PeerData *v);

static void push_player(lua_State *L, PeerData *v)
{
	Server *s = v->server;
	bool exe = (s->state == ST_GAME || s->state == ST_CHARSELECT) && server_is_exe(s, v->id);
	lua_createtable(L, 0, 16);
	lua_pushinteger(L, v->id);
	lua_setfield(L, -2, "id");
	lua_pushstring(L, v->nickname.value);
	lua_setfield(L, -2, "name");
	lua_pushboolean(L, v->is_bot);
	lua_setfield(L, -2, "bot");
	lua_pushboolean(L, exe);
	lua_setfield(L, -2, "exe");
	const char *ch = NULL;
	if (exe && v->exe_char >= EX_ORIGINAL && v->exe_char <= EX_EXELLER)
		ch = EXE_NAMES[v->exe_char];
	else if (!exe && v->surv_char >= CH_TAILS && v->surv_char <= CH_SALLY)
		ch = SURV_NAMES[v->surv_char];
	if (ch)
	{
		lua_pushstring(L, ch);
		lua_setfield(L, -2, "character");
	}
	lua_pushinteger(L, v->plr.net_valid ? v->plr.net_hp : 100);
	lua_setfield(L, -2, "hp");
	lua_pushinteger(L, v->plr.rings);
	lua_setfield(L, -2, "rings");
	lua_pushnumber(L, v->plr.pos.x);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, v->plr.pos.y);
	lua_setfield(L, -2, "y");
	bool dead = (v->plr.flags & PLAYER_DEAD) != 0, esc = (v->plr.flags & PLAYER_ESCAPED) != 0;
	lua_pushboolean(L, dead);
	lua_setfield(L, -2, "dead");
	lua_pushboolean(L, esc);
	lua_setfield(L, -2, "escaped");
	lua_pushboolean(L, (v->plr.flags & PLAYER_DEMONIZED) != 0);
	lua_setfield(L, -2, "demon");
	lua_pushboolean(L, v->in_game && !dead && !esc && s->state == ST_GAME);
	lua_setfield(L, -2, "alive");
	lua_pushboolean(L, v->in_game && s->state == ST_GAME);
	lua_setfield(L, -2, "playing");
	lua_pushboolean(L, peer_web_ready(v));
	lua_setfield(L, -2, "ready");
}

static PeerData *peer_by_id(Server *s, lua_Integer id)
{
	if (id <= 0 || id > 0xFFFF)
		return NULL;
	return server_find_peer(s, (uint16_t)id);
}

static PeerData *check_peer(lua_State *L, int arg)
{
	ModState *ms = ms_from(L);
	lua_Integer id;
	if (lua_istable(L, arg))
	{
		lua_getfield(L, arg, "id");
		id = luaL_checkinteger(L, -1);
		lua_pop(L, 1);
	}
	else
		id = luaL_checkinteger(L, arg);
	return peer_by_id(ms->server, id);
}

/* ------------------------------------------------------------------ to the web players */

/* a mod's commands to one player (SERVER_MOD_CMD); bots get nothing */
enum { CMD_KILL = 1, CMD_HP, CMD_RINGS, CMD_TELEPORT, CMD_FREEZE, CMD_LAUNCH, CMD_STAT, CMD_RESET_STATS,
	CMD_MESSAGE, CMD_SOUND };

static bool peer_web_ready(PeerData *v)
{
	return v && !v->is_bot && v->mod_ready;
}

static bool send_to(PeerData *v, Packet *p)
{
	if (!v || v->is_bot || v->disconnecting || v->mod_api < 1)
		return false;
	uint32_t sec = v->server->tick / TICKSPERSEC;
	if (sec != v->mod_out_sec)
	{
		v->mod_out_sec = sec;
		v->mod_out_count = 0;
	}
	if (v->mod_out_count >= MOD_OUT_PER_SEC)
	{
		if (v->mod_out_count++ == MOD_OUT_PER_SEC)
			Warn("[mod %s] too many packets to %s this second: some are dropped", g_mod.id, v->nickname.value);
		return false;
	}
	v->mod_out_count++;
	packet_send(v->peer, p, true);
	return true;
}

/* a limit of n a second (counter in *count, its second in *sec); false: over it */
static bool rate_ok(Server *s, uint32_t *sec, int *count, int n)
{
	uint32_t now = s->tick / TICKSPERSEC;
	if (now != *sec)
	{
		*sec = now;
		*count = 0;
	}
	return ++*count <= n;
}

static void send_all(Server *s, Packet *p)
{
	for (size_t i = 0; i < s->peers.capacity; i++)
		send_to((PeerData *)s->peers.ptr[i], p);
}

static String str_cut(const char *text, size_t max)
{
	String s;
	size_t n = strlen(text);
	if (n > max)
		n = max;
	/* (never cut a UTF-8 character in half) */
	while (n > 0 && n < strlen(text) && ((unsigned char)text[n] & 0xC0) == 0x80)
		n--;
	memcpy(s.value, text, n);
	s.value[n] = 0;
	s.len = (uint16_t)(n + 1);
	return s;
}

static bool cmd_begin(Packet *p, uint8_t cmd)
{
	if (!packet_new(p, SERVER_MOD_CMD))
		return false;
	return packet_write8(p, cmd);
}

/* ------------------------------------------------------------------ game.* */

static int g_players(lua_State *L)
{
	ModState *ms = ms_from(L);
	Server *s = ms->server;
	lua_newtable(L);
	int n = 0;
	for (size_t i = 0; i < s->peers.capacity; i++)
	{
		PeerData *v = (PeerData *)s->peers.ptr[i];
		if (!v || !v->verified)
			continue;
		push_player(L, v);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int g_player(lua_State *L)
{
	PeerData *v = check_peer(L, 1);
	if (!v || !v->verified)
		return 0;
	push_player(L, v);
	return 1;
}

static int g_state(lua_State *L)
{
	static const char *NAMES[] = { "lobby", "vote", "charselect", "game", "results" };
	Server *s = ms_from(L)->server;
	lua_pushstring(L, NAMES[s->state <= ST_RESULTS ? s->state : 0]);
	return 1;
}

static int g_map(lua_State *L)
{
	Server *s = ms_from(L)->server;
	int m = s->state == ST_GAME ? s->game.map : s->lobby.map;
	lua_pushinteger(L, m);
	const ModMap *mm = mod_map(m);
	if (mm)
		lua_pushstring(L, mm->file + 5);
	else if (m >= 0 && m <= MAP_COUNT)
		lua_pushstring(L, g_mapList[m].name);
	else
		lua_pushnil(L);
	lua_pushboolean(L, mm != NULL);
	return 3;
}

static void broadcast_time(Server *s)
{
	Packet p;
	if (!packet_new(&p, SERVER_GAME_TIME_SYNC))
		return;
	packet_write16(&p, (uint16_t)s->game.time_sec * TICKSPERSEC);
	server_broadcast(s, &p, true);
}

static int g_time(lua_State *L)
{
	Server *s = ms_from(L)->server;
	lua_pushinteger(L, s->state == ST_GAME ? s->game.time_sec : 0);
	return 1;
}

static int g_set_time(lua_State *L)
{
	Server *s = ms_from(L)->server;
	lua_Integer t = luaL_checkinteger(L, 1);
	if (t < 1)
		t = 1;
	if (t > MOD_TIME_MAX)
		t = MOD_TIME_MAX;  /* (the clients' clock is 16-bit ticks) */
	s->game.time_sec = (uint16_t)t;
	if (s->state == ST_GAME && s->game.started)
		broadcast_time(s);
	return 0;
}

static int g_elapsed(lua_State *L)
{
	Server *s = ms_from(L)->server;
	lua_pushnumber(L, s->state == ST_GAME ? s->game.elapsed / TICKSPERSEC : 0);
	return 1;
}

/* game.exes(): the EXEs' ids (a mod's round can have several, or none) */
static int g_exes(lua_State *L)
{
	Server *s = ms_from(L)->server;
	lua_newtable(L);
	int n = 0;
	if (s->state == ST_GAME || s->state == ST_CHARSELECT)
	{
		const uint16_t *ids = s->state == ST_GAME ? s->game.exes : s->lobby.exes;
		int cnt = s->state == ST_GAME ? s->game.exe_count : s->lobby.exe_count;
		for (int i = 0; i < cnt; i++)
		{
			lua_pushinteger(L, ids[i]);
			lua_rawseti(L, -2, ++n);
		}
	}
	return 1;
}

static int g_exe(lua_State *L)
{
	Server *s = ms_from(L)->server;
	uint16_t id = s->state == ST_GAME ? (uint16_t)s->game.exe : s->lobby.exe;
	if (id == 0 || id == UINT16_MAX)
		return 0;
	lua_pushinteger(L, id);
	return 1;
}

static const char *text_arg(lua_State *L, int arg)
{
	return luaL_tolstring(L, arg, NULL);
}

static int g_say(lua_State *L)
{
	lua_settop(L, 1);  /* (text_arg pushes: no stray value where an argument is missing) */
	ModState *ms = ms_from(L);
	Server *s = ms->server;
	const char *t = text_arg(L, 1);
	if (!rate_ok(s, &ms->say_sec, &ms->say_count, MOD_SAY_PER_SEC))
		return 0;
	String m = str_cut(t, 120);
	server_broadcast_msg(s, m.value);
	return 0;
}

static int g_tell(lua_State *L)
{
	lua_settop(L, 2);
	ModState *ms = ms_from(L);
	PeerData *v = check_peer(L, 1);
	const char *t = text_arg(L, 2);
	if (v && !v->is_bot && rate_ok(ms->server, &ms->say_sec, &ms->say_count, MOD_SAY_PER_SEC))
	{
		String m = str_cut(t, 120);
		server_send_msg(v->server, v->peer, m.value);
	}
	return 0;
}

static int g_end_round(lua_State *L)
{
	ModState *ms = ms_from(L);
	const char *w = luaL_optstring(L, 1, "time");
	Ending e = !strcmp(w, "exe") ? ED_EXEWIN : !strcmp(w, "survivors") ? ED_SURVWIN : ED_TIMEOVER;
	ms->pending_end = (int)e + 1;  /* done after this hook, at the next tick */
	return 0;
}

static int cmd_simple(lua_State *L, uint8_t cmd)
{
	PeerData *v = check_peer(L, 1);
	Packet p;
	if (v && cmd_begin(&p, cmd))
		send_to(v, &p);
	return 0;
}

static int g_kill(lua_State *L) { return cmd_simple(L, CMD_KILL); }
static int g_reset_stats(lua_State *L) { return cmd_simple(L, CMD_RESET_STATS); }

static int g_set_hp(lua_State *L)
{
	PeerData *v = check_peer(L, 1);
	lua_Integer hp = luaL_checkinteger(L, 2);
	if (hp < 0)
		hp = 0;
	if (hp > 127)
		hp = 127;  /* (sent as a signed byte) */
	Packet p;
	if (v && cmd_begin(&p, CMD_HP))
	{
		packet_write8(&p, (uint8_t)hp);
		send_to(v, &p);
	}
	return 0;
}

static void set_rings(PeerData *v, lua_Integer n)
{
	if (n < 0)
		n = 0;
	if (n > 9999)
		n = 9999;
	v->plr.rings = (uint16_t)n;
	Packet p;
	if (cmd_begin(&p, CMD_RINGS))
	{
		packet_write16(&p, (uint16_t)n);
		send_to(v, &p);
	}
}

static int g_set_rings(lua_State *L)
{
	PeerData *v = check_peer(L, 1);
	lua_Integer n = luaL_checkinteger(L, 2);
	if (v)
		set_rings(v, n);
	return 0;
}

static int g_give_rings(lua_State *L)
{
	PeerData *v = check_peer(L, 1);
	lua_Integer n = luaL_checkinteger(L, 2);
	if (v)
		set_rings(v, (lua_Integer)v->plr.rings + n);
	return 0;
}

static int g_teleport(lua_State *L)
{
	PeerData *v = check_peer(L, 1);
	lua_Number x = luaL_checknumber(L, 2), y = luaL_checknumber(L, 3);
	Packet p;
	if (v && cmd_begin(&p, CMD_TELEPORT))
	{
		v->plr.ex_teleport = 30;  /* (not a cheat: the position anticheat lets it go) */
		packet_write16(&p, (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x));
		packet_write16(&p, (uint16_t)(y < 0 ? 0 : y > 65535 ? 65535 : y));
		send_to(v, &p);
	}
	return 0;
}

static int g_freeze(lua_State *L)
{
	PeerData *v = check_peer(L, 1);
	lua_Number sec = luaL_checknumber(L, 2);
	Packet p;
	if (v && cmd_begin(&p, CMD_FREEZE))
	{
		lua_Number f = sec * TICKSPERSEC;
		packet_write16(&p, (uint16_t)(f < 0 ? 0 : f > 60000 ? 60000 : f));
		send_to(v, &p);
	}
	return 0;
}

static uint16_t s16(lua_Number v)
{
	v *= 100;
	if (v < -32000)
		v = -32000;
	if (v > 32000)
		v = 32000;
	return (uint16_t)(int16_t)v;
}

static int g_launch(lua_State *L)
{
	PeerData *v = check_peer(L, 1);
	lua_Number vx = luaL_checknumber(L, 2), vy = luaL_checknumber(L, 3);
	Packet p;
	if (v && cmd_begin(&p, CMD_LAUNCH))
	{
		v->plr.ex_teleport = 60;
		packet_write16(&p, s16(vx));
		packet_write16(&p, s16(vy));
		send_to(v, &p);
	}
	return 0;
}

static const char *STAT_NAMES[] = { "speed", "acceleration", "jump", "gravity", "damage_taken", NULL };

static int g_set_stat(lua_State *L)
{
	PeerData *v = check_peer(L, 1);
	int stat = luaL_checkoption(L, 2, NULL, STAT_NAMES);
	lua_Number k = luaL_checknumber(L, 3);
	if (k < 0)
		k = 0;
	if (k > 10)
		k = 10;
	Packet p;
	if (v && cmd_begin(&p, CMD_STAT))
	{
		packet_write8(&p, (uint8_t)stat);
		packet_write16(&p, (uint16_t)(k * 1000));
		send_to(v, &p);
	}
	return 0;
}

static void target_send(lua_State *L, int arg, Packet *p)
{
	Server *s = ms_from(L)->server;
	if (lua_isnoneornil(L, arg) || (lua_isinteger(L, arg) && lua_tointeger(L, arg) == 0))
		send_all(s, p);
	else
	{
		PeerData *v = check_peer(L, arg);
		if (v)
			send_to(v, p);
	}
}

static int g_message(lua_State *L)
{
	lua_settop(L, 3);
	lua_Number sec = luaL_optnumber(L, 3, 3);
	const char *t = text_arg(L, 2);
	Packet p;
	if (cmd_begin(&p, CMD_MESSAGE))
	{
		lua_Number f = sec * TICKSPERSEC;
		packet_write16(&p, (uint16_t)(f < 1 ? 1 : f > 60000 ? 60000 : f));
		packet_writestr(&p, str_cut(t, 120));
		target_send(L, 1, &p);
	}
	return 0;
}

static int g_sound(lua_State *L)
{
	const char *n = luaL_checkstring(L, 2);
	Packet p;
	if (cmd_begin(&p, CMD_SOUND))
	{
		packet_writestr(&p, str_cut(n, 64));
		target_send(L, 1, &p);
	}
	return 0;
}

static int g_event(lua_State *L)
{
	lua_settop(L, 3);
	const char *n = luaL_checkstring(L, 2);
	const char *d = lua_isnoneornil(L, 3) ? "" : text_arg(L, 3);
	Packet p;
	if (packet_new(&p, SERVER_MOD_EVENT))
	{
		packet_writestr(&p, str_cut(n, 32));
		packet_writestr(&p, str_cut(d, 180));
		target_send(L, 1, &p);
	}
	return 0;
}

static void sync_packet(Packet *p, const char *key, uint8_t type, const char *val)
{
	if (!packet_new(p, SERVER_MOD_SYNC))
		return;
	packet_writestr(p, str_cut(key, 40));
	packet_write8(p, type);
	packet_writestr(p, str_cut(val, 120));
}

/* game.sync(key, value): every web player's client.lua sees mod.synced[key] (and on_sync) */
static int g_sync(lua_State *L)
{
	ModState *ms = ms_from(L);
	size_t klen;
	const char *key = luaL_checklstring(L, 1, &klen);
	if (klen > 40)
		return luaL_argerror(L, 1, "keys are up to 40 characters");
	if (!strncmp(key, "config.", 7))
		return luaL_argerror(L, 1, "keys starting with \"config.\" are the owner's settings");
	uint8_t type;
	char val[128];
	switch (lua_type(L, 2))
	{
		case LUA_TNIL: case LUA_TNONE: type = 0; val[0] = 0; break;
		case LUA_TNUMBER: type = 1; snprintf(val, sizeof val, "%.17g", lua_tonumber(L, 2)); break;
		case LUA_TBOOLEAN: type = 3; snprintf(val, sizeof val, "%d", lua_toboolean(L, 2)); break;
		case LUA_TSTRING: type = 2; snprintf(val, sizeof val, "%s", lua_tostring(L, 2)); break;
		default: return luaL_argerror(L, 2, "nil, a number, a string or a boolean");
	}
	int k;
	for (k = 0; k < ms->sync_count; k++)
		if (!strcmp(ms->sync_key[k], key))
			break;
	if (k == ms->sync_count)
	{
		if (k >= MOD_MAX_SYNC)
			return luaL_error(L, "game.sync: at most %d keys", MOD_MAX_SYNC);
		ms->sync_count++;
		snprintf(ms->sync_key[k], sizeof ms->sync_key[k], "%s", key);
	}
	ms->sync_type[k] = type;
	snprintf(ms->sync_val[k], sizeof ms->sync_val[k], "%s", val);
	Packet p;
	sync_packet(&p, key, type, val);
	Server *s = ms->server;
	for (size_t i = 0; i < s->peers.capacity; i++)
	{
		PeerData *v = (PeerData *)s->peers.ptr[i];
		if (!v || v->is_bot || v->mod_api < 1)
			continue;
		if (send_to(v, &p))
			v->mod_sync_dirty[k / 8] &= (uint8_t)~(1u << (k % 8));
		else
			v->mod_sync_dirty[k / 8] |= (uint8_t)(1u << (k % 8));  /* (sent when the limit allows) */
	}
	return 0;
}

/* the synced values a player could not get yet (over the packet limit): the latest ones, now if the limit allows */
static void sync_flush(ModState *ms)
{
	Server *s = ms->server;
	for (size_t i = 0; i < s->peers.capacity; i++)
	{
		PeerData *v = (PeerData *)s->peers.ptr[i];
		if (!v || v->is_bot || v->mod_api < 1)
			continue;
		for (int k = 0; k < ms->sync_count; k++)
		{
			if (!(v->mod_sync_dirty[k / 8] & (1u << (k % 8))))
				continue;
			Packet p;
			sync_packet(&p, ms->sync_key[k], ms->sync_type[k], ms->sync_val[k]);
			if (!send_to(v, &p))
				break;
			v->mod_sync_dirty[k / 8] &= (uint8_t)~(1u << (k % 8));
		}
	}
}

/* timers: game.timer(seconds, fn) once, game.every(seconds, fn) repeating; game.cancel(handle) */
static int add_timer(lua_State *L, bool repeat)
{
	ModState *ms = ms_from(L);
	lua_Number sec = luaL_checknumber(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_Integer frames = sec * TICKSPERSEC < 1 ? 1 : sec * TICKSPERSEC > 1e9 ? 1000000000 : (lua_Integer)(sec * TICKSPERSEC);
	if (ms->timer_count >= MOD_MAX_TIMERS)
		return luaL_error(L, "game.timer / game.every: at most %d at a time", MOD_MAX_TIMERS);
	ms->timer_count++;
	lua_getfield(L, LUA_REGISTRYINDEX, "mod_timers");
	lua_createtable(L, 0, 3);
	lua_pushinteger(L, (lua_Integer)ms->frame + frames);
	lua_setfield(L, -2, "at");
	lua_pushinteger(L, (lua_Integer)++ms->timer_seq);
	lua_setfield(L, -2, "seq");
	if (repeat)
	{
		lua_pushinteger(L, frames);
		lua_setfield(L, -2, "every");
	}
	lua_pushvalue(L, 2);
	lua_setfield(L, -2, "fn");
	lua_pushvalue(L, -1);
	lua_pushboolean(L, true);
	lua_settable(L, -4);   /* timers[t] = true */
	return 1;              /* the handle: the timer's table */
}

static int g_timer(lua_State *L) { return add_timer(L, false); }
static int g_every(lua_State *L) { return add_timer(L, true); }

static int g_cancel(lua_State *L)
{
	if (!lua_istable(L, 1))
		return 0;
	lua_getfield(L, LUA_REGISTRYINDEX, "mod_timers");
	lua_pushvalue(L, 1);
	lua_gettable(L, -2);
	bool there = !lua_isnil(L, -1);
	lua_pop(L, 1);
	if (there)
	{
		lua_pushvalue(L, 1);
		lua_pushnil(L);
		lua_settable(L, -3);
		ms_from(L)->timer_count--;
	}
	return 0;
}

static uint64_t g_rng;

static uint64_t rng_next(void)
{
	if (!g_rng)
		g_rng = ((uint64_t)time(NULL) << 32) ^ (uint64_t)clock() ^ 0x9E3779B97F4A7C15ull;
	/* splitmix64 */
	uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

static int g_random(lua_State *L)
{
	lua_Integer a = luaL_checkinteger(L, 1), b = luaL_optinteger(L, 2, a);
	if (lua_gettop(L) < 2)
	{
		b = a;
		a = 1;
	}
	if (b < a)
	{
		lua_Integer t = a;
		a = b;
		b = t;
	}
	uint64_t span = (uint64_t)b - (uint64_t)a, r = rng_next();  /* (b - a: the range minus one) */
	if (span != UINT64_MAX)
		r %= span + 1;
	lua_pushinteger(L, (lua_Integer)((uint64_t)a + r));
	return 1;
}

static int g_distance(lua_State *L)
{
	PeerData *a = check_peer(L, 1), *b = check_peer(L, 2);
	if (!a || !b)
		return 0;
	lua_pushnumber(L, vector2_dist(&a->plr.pos, &b->plr.pos));
	return 1;
}

static int g_choose_exe(lua_State *L)
{
	ModState *ms = ms_from(L);
	PeerData *v = check_peer(L, 1);
	ms->pending_exe = v ? (uint16_t)(v->id + 1) : 0;
	return 0;
}

static int g_log(lua_State *L)
{
	ModState *ms = ms_from(L);
	if (!rate_ok(ms->server, &ms->log_sec, &ms->log_count, MOD_LOG_PER_SEC))
	{
		if (ms->log_count == MOD_LOG_PER_SEC + 1)
			Warn("[mod %s] too many log lines this second: the rest are skipped", g_mod.id);
		return 0;
	}
	int n = lua_gettop(L);
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	for (int i = 1; i <= n; i++)
	{
		if (i > 1)
			luaL_addchar(&b, ' ');
		luaL_tolstring(L, i, NULL);
		luaL_addvalue(&b);
	}
	luaL_pushresult(&b);
	Info("[mod %s] %s", g_mod.id, lua_tostring(L, -1));
	return 0;
}

/* require("name"): lua/name.lua from the package, once */
static int g_require(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	for (const char *c = name; *c; c++)
		if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_'))
			return luaL_error(L, "require: module names are a-z 0-9 _ (lua/<name>.lua)");
	lua_getfield(L, LUA_REGISTRYINDEX, "mod_loaded");
	lua_getfield(L, -1, name);
	if (!lua_isnil(L, -1))
		return 1;
	lua_pop(L, 1);
	char path[96];
	snprintf(path, sizeof path, "lua/%s.lua", name);
	uint32_t size;
	const uint8_t *src = mod_file(path, &size);
	if (!src)
		return luaL_error(L, "require: no %s in the mod", path);
	char chunk[100];
	snprintf(chunk, sizeof chunk, "=%s", path);
	if (luaL_loadbufferx(L, (const char *)src, size, chunk, "t") != LUA_OK)
		return lua_error(L);
	lua_call(L, 0, 1);
	if (lua_isnil(L, -1))
	{
		lua_pop(L, 1);
		lua_pushboolean(L, true);
	}
	lua_pushvalue(L, -1);
	lua_setfield(L, -3, name);
	return 1;
}

static int g_print(lua_State *L) { return g_log(L); }

static const luaL_Reg GAME_FUNCS[] = {
	{ "players", g_players }, { "player", g_player }, { "state", g_state }, { "map", g_map },
	{ "time", g_time }, { "set_time", g_set_time }, { "elapsed", g_elapsed }, { "exe", g_exe }, { "exes", g_exes },
	{ "say", g_say }, { "tell", g_tell }, { "end_round", g_end_round }, { "kill", g_kill },
	{ "set_hp", g_set_hp }, { "set_rings", g_set_rings }, { "give_rings", g_give_rings },
	{ "teleport", g_teleport }, { "freeze", g_freeze }, { "launch", g_launch }, { "set_stat", g_set_stat },
	{ "reset_stats", g_reset_stats }, { "message", g_message }, { "sound", g_sound }, { "event", g_event },
	{ "sync", g_sync }, { "timer", g_timer }, { "every", g_every }, { "cancel", g_cancel },
	{ "random", g_random }, { "distance", g_distance }, { "choose_exe", g_choose_exe }, { "log", g_log },
	{ NULL, NULL }
};

static void push_json(lua_State *L, cJSON *j)
{
	if (cJSON_IsNumber(j) && j->valuedouble == floor(j->valuedouble) && fabs(j->valuedouble) < 1e15)
		lua_pushinteger(L, (lua_Integer)j->valuedouble);  /* (8, not 8.0) */
	else if (cJSON_IsNumber(j))
		lua_pushnumber(L, j->valuedouble);
	else if (cJSON_IsBool(j))
		lua_pushboolean(L, cJSON_IsTrue(j));
	else if (cJSON_IsString(j))
		lua_pushstring(L, j->valuestring);
	else
		lua_pushnil(L);
}

/* pcall / xpcall as the base library's, except that running out of time (or memory) can't be caught:
   a loop around pcall(endless loop) would never give the lobby back */
static int finish_pcall(lua_State *L, int status, lua_KContext extra)
{
	if (status != LUA_OK && status != LUA_YIELD)
	{
		if (ms_from(L)->budget <= 0 || status == LUA_ERRMEM)
			return lua_error(L);
		lua_pushboolean(L, 0);
		lua_pushvalue(L, -2);
		return 2;
	}
	return lua_gettop(L) - (int)extra;
}

static int l_pcall(lua_State *L)
{
	luaL_checkany(L, 1);
	lua_pushboolean(L, 1);
	lua_insert(L, 1);
	int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finish_pcall);
	return finish_pcall(L, status, 0);
}

/* xpcall's handler, unless the error is "took too long": that one is raised inside the instruction hook,
   where Lua runs the handler with hooks off (an endless handler would never stop) */
static int xpcall_handler(lua_State *L)
{
	if (ms_from(L)->budget <= 0)
		return 1;
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	return 1;
}

/* setmetatable without __gc: Lua runs finalizers with hooks off (an endless one would never stop) */
static int l_setmetatable(lua_State *L)
{
	if (lua_istable(L, 2))
	{
		lua_pushliteral(L, "__gc");
		if (lua_rawget(L, 2) != LUA_TNIL)
			return luaL_error(L, "setmetatable: __gc is not allowed in mods");
		lua_pop(L, 1);
	}
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	lua_call(L, lua_gettop(L) - 1, 1);
	return 1;
}

static int l_xpcall(lua_State *L)
{
	int n = lua_gettop(L);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_pushvalue(L, 2);
	lua_pushcclosure(L, xpcall_handler, 1);
	lua_replace(L, 2);
	lua_pushboolean(L, 1);
	lua_pushvalue(L, 1);
	lua_rotate(L, 3, 2);
	int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finish_pcall);
	return finish_pcall(L, status, 2);
}

static lua_CFunction g_co_resume;

static int l_resume(lua_State *L)
{
	int n = g_co_resume(L);
	if (ms_from(L)->budget <= 0)
		return luaL_error(L, "the script took too long (endless loop?)");
	return n;
}

static void open_safe_libs(lua_State *L)
{
	static const luaL_Reg LIBS[] = {
		{ LUA_GNAME, luaopen_base }, { LUA_TABLIBNAME, luaopen_table }, { LUA_STRLIBNAME, luaopen_string },
		{ LUA_MATHLIBNAME, luaopen_math }, { LUA_UTF8LIBNAME, luaopen_utf8 }, { LUA_COLIBNAME, luaopen_coroutine },
		{ NULL, NULL }
	};
	for (const luaL_Reg *l = LIBS; l->func; l++)
	{
		luaL_requiref(L, l->name, l->func, 1);
		lua_pop(L, 1);
	}
	/* no files, no bytecode, no raw access to other chunks' environments */
	static const char *DROP[] = { "dofile", "loadfile", "load", "collectgarbage", "rawequal", "rawget", "rawset",
		"rawlen", NULL };
	for (const char **d = DROP; *d; d++)
	{
		lua_pushnil(L);
		lua_setglobal(L, *d);
	}
	lua_getglobal(L, "string");
	lua_pushnil(L);
	lua_setfield(L, -2, "dump");
	lua_pop(L, 1);
	lua_pushcfunction(L, l_pcall);
	lua_setglobal(L, "pcall");
	lua_pushcfunction(L, l_xpcall);
	lua_setglobal(L, "xpcall");
	lua_getglobal(L, "setmetatable");
	lua_pushcclosure(L, l_setmetatable, 1);
	lua_setglobal(L, "setmetatable");
	lua_getglobal(L, "coroutine");
	lua_getfield(L, -1, "resume");
	g_co_resume = lua_tocfunction(L, -1);
	lua_pop(L, 1);
	lua_pushcfunction(L, l_resume);
	lua_setfield(L, -2, "resume");
	lua_pop(L, 1);
}

/* ------------------------------------------------------------------ objects (mod api 2: modobj.c) */

static const uint8_t *obj_file(void *ud, const char *name, uint32_t *size)
{
	(void)ud;
	return mod_file(name, size);
}

static bool obj_call(void *ud, lua_State *L, int nargs, int nres, const char *where)
{
	(void)L;
	return ms_call((ModState *)ud, nargs, nres, where);
}

static void obj_push_player(void *ud, lua_State *L, uint16_t id)
{
	PeerData *v = server_find_peer(((ModState *)ud)->server, id);
	if (v && v->verified)
		push_player(L, v);
	else
		lua_pushnil(L);
}

/* a character's box from its x, y: its idle sprite's in the game (spr_tails_idle...), centred */
static void obj_char_box(bool exe, int ch, ObjPlayer *o)
{
	static const float SURV[6][3] = {   /* width, top, height: Tails Knuckles Eggman Amy Cream Sally */
		{ 21, -13, 31 }, { 28, -22, 40 }, { 31, -31, 49 }, { 23, -21, 39 }, { 23, -14, 32 }, { 22, -25, 43 } };
	static const float EXE[4][3] = {    /* Exe Chaos Exetior Exeller */
		{ 41, -26, 44 }, { 48, -24, 42 }, { 53, -44, 62 }, { 67, -24, 43 } };
	const float *b = exe ? (ch >= 0 && ch < 4 ? EXE[ch] : EXE[0]) : (ch >= 0 && ch < 6 ? SURV[ch] : SURV[0]);
	o->bw = b[0];
	o->bl = -b[0] / 2;
	o->bt = b[1];
	o->bh = b[2];
}

static int obj_players(void *ud, ObjPlayer *out, int cap)
{
	Server *s = ((ModState *)ud)->server;
	int n = 0;
	for (size_t i = 0; i < s->peers.capacity && n < cap; i++)
	{
		PeerData *v = (PeerData *)s->peers.ptr[i];
		if (!v || !v->verified || !v->in_game || s->state != ST_GAME)
			continue;
		bool dead = (v->plr.flags & PLAYER_DEAD) != 0, esc = (v->plr.flags & PLAYER_ESCAPED) != 0;
		out[n].id = v->id;
		out[n].x = v->plr.pos.x;
		out[n].y = v->plr.pos.y;
		out[n].alive = !dead && !esc;
		if (server_is_exe(s, v->id))
			obj_char_box(true, (int)v->exe_char, &out[n]);
		else
			obj_char_box(false, (int)v->surv_char, &out[n]);
		n++;
	}
	return n;
}

static void obj_warn(void *ud, const char *text)
{
	(void)ud;
	Warn("[mod %s] %s", g_mod.id, text);
}

static void obj_changed(void *ud, ObjInst *o, int what)
{
	ModState *ms = (ModState *)ud;
	int slot = (int)(o - ms->objs.inst);
	struct ObjSent *s = &ms->sent[slot];
	if (what == OBJ_DESTROYED)
	{
		if (s->created && ms->gone_count < OBJ_MAX)
			ms->gone[ms->gone_count++] = s->id;
		memset(s, 0, sizeof *s);
		return;
	}
	memset(s, 0, sizeof *s);
	s->id = o->id;   /* (created: told at the next send) */
}

/* objects' records (SERVER_MOD_OBJECTS): 1 made (id, type, then its state with every field), 2 changed (id,
   which fields, the fields), 3 gone (id), 4 all gone. Fields: 1 x y, 2 vx vy, 4 sprite, 8 frame + 1 (0:
   animated), 16 flags (1: visible), 32 xscale yscale, 64 alpha (0-255), 128 angle */
enum { OBJF_POS = 1, OBJF_VEL = 2, OBJF_SPRITE = 4, OBJF_FRAME = 8, OBJF_FLAGS = 16, OBJF_SCALE = 32, OBJF_ALPHA = 64,
	OBJF_ANGLE = 128, OBJF_ALL = 255 };

typedef struct
{
	Packet p;
	bool open;
	PeerData *only;   /* one player (a late joiner), else everyone ready */
	Server *server;
} ObjOut;

static void obj_out_send(ObjOut *w)
{
	if (!w->open)
		return;
	w->open = false;
	if (w->only)
	{
		if (!w->only->is_bot && !w->only->disconnecting && w->only->mod_api >= 2)
			packet_send(w->only->peer, &w->p, true);
		return;
	}
	for (size_t i = 0; i < w->server->peers.capacity; i++)
	{
		PeerData *v = (PeerData *)w->server->peers.ptr[i];
		if (v && !v->is_bot && !v->disconnecting && v->mod_api >= 2 && v->mod_ready)
			packet_send(v->peer, &w->p, true);
	}
}

/* room for n more bytes (a new packet when this one is full) */
static void obj_room(ObjOut *w, int n)
{
	if (w->open && (int)w->p.pos + n >= PACKET_MAXSIZE - 2)
		obj_out_send(w);
	if (!w->open)
	{
		packet_new(&w->p, SERVER_MOD_OBJECTS);
		w->open = true;
	}
}

static void obj_put_str(ObjOut *w, const char *s)
{
	String v = str_cut(s, OBJ_SPRITE_LEN - 1);
	packet_writestr(&w->p, v);
	if (!v.len)
		packet_write8(&w->p, 0);   /* (an empty text: its end) */
}

static void obj_put_state(ObjOut *w, const ObjInst *o, uint8_t mask)
{
	packet_write8(&w->p, mask);
	if (mask & OBJF_POS) { packet_writefloat(&w->p, o->x); packet_writefloat(&w->p, o->y); }
	if (mask & OBJF_VEL) { packet_writefloat(&w->p, o->vx); packet_writefloat(&w->p, o->vy); }
	if (mask & OBJF_SPRITE) obj_put_str(w, o->sprite);
	if (mask & OBJF_FRAME) packet_write16(&w->p, (uint16_t)(o->frame + 1));
	if (mask & OBJF_FLAGS)
	{   /* 1 visible, 2 not solid, 4 a glow (adds its colours), 8 tinted: its colour follows */
		bool tint = o->color != 0xFFFFFF;
		packet_write8(&w->p, (o->visible ? 1 : 0) | (o->solid_off ? 2 : 0) | (o->blend_add ? 4 : 0) | (tint ? 8 : 0));
		if (tint)
		{
			packet_write8(&w->p, (uint8_t)(o->color >> 16));
			packet_write8(&w->p, (uint8_t)(o->color >> 8));
			packet_write8(&w->p, (uint8_t)o->color);
		}
	}
	if (mask & OBJF_SCALE) { packet_writefloat(&w->p, o->xscale); packet_writefloat(&w->p, o->yscale); }
	if (mask & OBJF_ALPHA) packet_write8(&w->p, (uint8_t)(o->alpha * 255 + 0.5f));
	if (mask & OBJF_ANGLE) packet_writefloat(&w->p, o->angle);
}

static void obj_remember(struct ObjSent *s, const ObjInst *o)
{
	s->id = o->id;
	s->created = true;
	s->x = o->x; s->y = o->y; s->vx = o->vx; s->vy = o->vy;
	s->xscale = o->xscale; s->yscale = o->yscale; s->angle = o->angle;
	memcpy(s->sprite, o->sprite, sizeof s->sprite);
	s->frame = o->frame;
	s->visible = o->visible;
	s->solid_off = o->solid_off;
	s->blend_add = o->blend_add;
	s->color = o->color;
	s->alpha = (uint8_t)(o->alpha * 255 + 0.5f);
}

#define OBJ_REC_MAX (1 + 2 + 33 + 1 + 8 + 8 + OBJ_SPRITE_LEN + 2 + 1 + 3 + 8 + 1 + 4)

static void obj_create_rec(ModState *ms, ObjOut *w, const ObjInst *o)
{
	obj_room(w, OBJ_REC_MAX);
	packet_write8(&w->p, 1);
	packet_write16(&w->p, o->id);
	obj_put_str(w, ms->objs.types[o->type].name);
	obj_put_state(w, o, OBJF_ALL);
}

/* the changes since the last send, to everyone ready */
static void obj_send(ModState *ms)
{
	ObjOut w = { .server = ms->server };
	if (ms->clear_pending)
	{
		obj_room(&w, 1);
		packet_write8(&w.p, 4);
		ms->clear_pending = false;
	}
	for (int i = 0; i < ms->gone_count; i++)
	{
		obj_room(&w, 3);
		packet_write8(&w.p, 3);
		packet_write16(&w.p, ms->gone[i]);
	}
	ms->gone_count = 0;
	for (int i = 0; i < OBJ_MAX; i++)
	{
		ObjInst *o = &ms->objs.inst[i];
		struct ObjSent *s = &ms->sent[i];
		if (!o->used || o->dead)
			continue;
		if (!s->created || s->id != o->id)
		{
			obj_create_rec(ms, &w, o);
			obj_remember(s, o);
			continue;
		}
		uint8_t mask = 0;
		if (fabsf(o->x - s->x) > 0.01f || fabsf(o->y - s->y) > 0.01f) mask |= OBJF_POS;
		if (o->vx != s->vx || o->vy != s->vy) mask |= OBJF_VEL;
		if (strcmp(o->sprite, s->sprite)) mask |= OBJF_SPRITE;
		if (o->frame != s->frame) mask |= OBJF_FRAME;
		if (o->visible != s->visible || o->solid_off != s->solid_off || o->blend_add != s->blend_add || o->color != s->color)
			mask |= OBJF_FLAGS;
		if (o->xscale != s->xscale || o->yscale != s->yscale) mask |= OBJF_SCALE;
		if ((uint8_t)(o->alpha * 255 + 0.5f) != s->alpha) mask |= OBJF_ALPHA;
		if (o->angle != s->angle) mask |= OBJF_ANGLE;
		if (mask & OBJF_VEL)
			mask |= OBJF_POS;   /* (a new speed: where it is now too, else the games' guess runs on) */
		if (!mask)
			continue;
		obj_room(&w, OBJ_REC_MAX);
		packet_write8(&w.p, 2);
		packet_write16(&w.p, o->id);
		obj_put_state(&w, o, mask);
		obj_remember(s, o);
	}
	obj_out_send(&w);
}

/* a player who just loaded the mod: every object there is now */
static void obj_send_all_to(ModState *ms, PeerData *v)
{
	ObjOut w = { .server = ms->server, .only = v };
	obj_room(&w, 1);
	packet_write8(&w.p, 4);
	for (int i = 0; i < OBJ_MAX; i++)
	{
		ObjInst *o = &ms->objs.inst[i];
		if (o->used && !o->dead && ms->sent[i].created)
		{
			/* (as the others were told: the next changes come to everyone) */
			ObjInst told = *o;
			struct ObjSent *s = &ms->sent[i];
			told.x = s->x; told.y = s->y; told.vx = s->vx; told.vy = s->vy; told.xscale = s->xscale; told.yscale = s->yscale;
			told.angle = s->angle; told.frame = s->frame; told.visible = s->visible; told.solid_off = s->solid_off; told.alpha = s->alpha / 255.0f;
			told.blend_add = s->blend_add; told.color = s->color;
			memcpy(told.sprite, s->sprite, sizeof told.sprite);
			obj_create_rec(ms, &w, &told);
		}
	}
	obj_out_send(&w);
}

static bool has_objects(void)
{
	for (int i = 0; i < g_entry_count; i++)
		if (!strncmp(g_entries[i].name, "objects/", 8))
			return true;
	return false;
}

bool mod_server_init(Server *server)
{
	server->mod = NULL;
	if (!g_mod.active)
		return true;
	ModState *ms = (ModState *)calloc(1, sizeof(ModState));
	RAssert(ms);
	ms->server = server;
	server->mod = ms;
	uint32_t size;
	const uint8_t *src = mod_file("server.lua", &size);
	ms->has_objects = g_mod.api >= 2 && has_objects();
	if (!src && !ms->has_objects)
		return true;  /* maps / art only */
	ms->L = lua_newstate(l_alloc, ms);
	if (!ms->L)
		return true;
	lua_State *L = ms->L;
	lua_sethook(L, l_hook, LUA_MASKCOUNT, MOD_INSTR_STEP);
	open_safe_libs(L);
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "mod_timers");
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "mod_loaded");
	luaL_newlib(L, GAME_FUNCS);
	lua_setglobal(L, "game");
	lua_pushcfunction(L, g_require);
	lua_setglobal(L, "require");
	lua_pushcfunction(L, g_print);
	lua_setglobal(L, "print");
	/* mod = { id, version, name, config = { owner's settings } } */
	lua_createtable(L, 0, 4);
	lua_pushstring(L, g_mod.id);
	lua_setfield(L, -2, "id");
	lua_pushstring(L, g_mod.version);
	lua_setfield(L, -2, "version");
	lua_pushstring(L, g_mod.name);
	lua_setfield(L, -2, "name");
	lua_newtable(L);
	cJSON *s;
	cJSON_ArrayForEach(s, g_settings)
	{
		push_json(L, s);
		lua_setfield(L, -2, s->string);
	}
	lua_setfield(L, -2, "config");
	lua_setglobal(L, "mod");

	if (ms->has_objects)
	{
		const char *names[4000];
		for (int i = 0; i < g_entry_count; i++)
			names[i] = g_entries[i].name;
		ObjHost host = { ms, obj_file, obj_call, obj_push_player, obj_players, obj_warn, obj_changed };
		obj_init(&ms->objs, L, &host, names, g_entry_count);
		Info("[mod %s] %d object type(s)", g_mod.id, ms->objs.ntypes);
	}
	if (!src)
		return true;  /* objects only */
	if (luaL_loadbufferx(L, (const char *)src, size, "=server.lua", "t") != LUA_OK)
	{
		ms_error(ms, "server.lua");
		ms->resting = true;
		return true;
	}
	if (ms_call(ms, 0, 0, "server.lua") && hook(ms, "on_load"))
		ms_call(ms, 0, 0, "on_load");
	return true;
}

void mod_server_free(Server *server)
{
	ModState *ms = ms_of(server);
	if (!ms)
		return;
	if (ms->L)
		lua_close(ms->L);
	free(ms);
	server->mod = NULL;
}

/* ------------------------------------------------------------------ players joining */

bool mod_client_ok(PeerData *v)
{
	return !g_mod.active || v->is_bot || v->mod_api >= g_mod.api;
}

/* a disconnect with a text the game shows ("Disconnected: ..."), not only a reason code */
static void kick_with_text(PeerData *v, DisconnectReason reason, const char *text, const char *log)
{
	if (v->disconnecting)
		return;
	Packet p;
	if (packet_new(&p, SERVER_PLAYER_FORCE_DISCONNECT))
	{
		packet_writestr(&p, str_cut(text, 120));
		packet_send(v->peer, &p, true);
	}
	/* (_later: enet_peer_disconnect would drop the text still waiting to go out) */
	enet_peer_disconnect_later(v->peer, reason);
	v->disconnecting = true;
	Info("Disconnected id %d %d: %s.", v->id, (int)reason, log);
}

void mod_refuse(PeerData *v)
{
	char msg[160];
	if (v->mod_api >= 1)   /* a web game from before this mod's api */
		snprintf(msg, sizeof msg, "This server's mod \"%s\" needs a newer game: reload the page (Ctrl+F5 / pull down).", g_mod.name);
	else
		snprintf(msg, sizeof msg, "This server runs the mod \"%s\": play it on the web version (sonicweb.dr2.site) or on a phone.", g_mod.name);
	kick_with_text(v, DR_VERMISMATCH, msg, "a mod is on: web players only");
}

void mod_send_info(PeerData *v)
{
	if (!g_mod.active || v->is_bot)
		return;
	Packet p;
	if (packet_new(&p, SERVER_MOD_INFO))
	{
		packet_writestr(&p, str_cut(g_mod.id, 39));
		packet_writestr(&p, str_cut(g_mod.version, 15));
		packet_writestr(&p, str_cut(g_mod.sha256, 64));
		packet_write32(&p, g_mod.size);
		packet_writestr(&p, str_cut(g_mod.name, 47));
		send_to(v, &p);
	}
	if (packet_new(&p, SERVER_MOD_URL))
	{
		packet_writestr(&p, str_cut(g_mod.url, 127));
		send_to(v, &p);
	}
	/* the owner's settings (mod.config on the client too), then the values synced so far */
	cJSON *s;
	cJSON_ArrayForEach(s, g_settings)
	{
		char key[48], val[128];
		uint8_t type = cJSON_IsNumber(s) ? 1 : cJSON_IsBool(s) ? 3 : 2;
		snprintf(key, sizeof key, "config.%s", s->string);
		if (cJSON_IsNumber(s))
			snprintf(val, sizeof val, "%.17g", s->valuedouble);
		else if (cJSON_IsBool(s))
			snprintf(val, sizeof val, "%d", cJSON_IsTrue(s));
		else
			snprintf(val, sizeof val, "%s", cJSON_IsString(s) ? s->valuestring : "");
		sync_packet(&p, key, type, val);
		send_to(v, &p);
	}
	ModState *ms = ms_of(v->server);
	for (int k = 0; ms && k < ms->sync_count; k++)
	{
		sync_packet(&p, ms->sync_key[k], ms->sync_type[k], ms->sync_val[k]);
		send_to(v, &p);
	}
}

/* ------------------------------------------------------------------ maps */

bool mod_is_map(int map)
{
	return g_mod.active && map >= MOD_MAP_BASE && map < MOD_MAP_BASE + g_mod.map_count;
}

const ModMap *mod_map(int map)
{
	return mod_is_map(map) ? &g_mod.maps[map - MOD_MAP_BASE] : NULL;
}

void mod_add_vote_maps(uint8_t *source, int *count, int cap)
{
	if (!g_mod.active || !g_mod.map_count)
		return;
	if (g_mod.maps_only)
		*count = 0;
	for (int i = 0; i < g_mod.map_count && *count < cap; i++)
		source[(*count)++] = (uint8_t)(MOD_MAP_BASE + i);
}

void mod_round_time(Server *server)
{
	const ModMap *m = mod_map(server->game.map);
	if (m)
	{
		server->game.time_sec = m->time;
		server->game.time = 0;
	}
}

bool mod_bots_allowed(Server *server)
{
	(void)server;
	return !g_mod.active || g_mod.bots;
}

/* ------------------------------------------------------------------ hooks */

static void hook_player(PeerData *v, const char *name)
{
	ModState *ms = v ? ms_of(v->server) : NULL;
	if (!hook(ms, name))
		return;
	push_player(ms->L, v);
	ms_call(ms, 1, 0, name);
}

void mod_on_join(PeerData *v) { hook_player(v, "on_join"); }
void mod_on_leave(PeerData *v) { hook_player(v, "on_leave"); }
void mod_on_death(PeerData *v) { hook_player(v, "on_death"); }
void mod_on_escape(PeerData *v) { hook_player(v, "on_escape"); }
void mod_on_demonize(PeerData *v) { hook_player(v, "on_demonize"); }

void mod_on_ring(PeerData *v)
{
	ModState *ms = ms_of(v->server);
	if (!hook(ms, "on_ring"))
		return;
	push_player(ms->L, v);
	lua_pushinteger(ms->L, v->plr.rings);
	ms_call(ms, 2, 0, "on_ring");
}

void mod_on_lobby(Server *server)
{
	ModState *ms = ms_of(server);
	if (!ms)
		return;
	ms->resting = false;
	ms->errors = 0;
	if (hook(ms, "on_lobby"))
		ms_call(ms, 0, 0, "on_lobby");
}

void mod_on_round_start(Server *server)
{
	ModState *ms = ms_of(server);
	if (!ms)
		return;
	ms->resting = false;
	ms->errors = 0;
	ms->pending_end = 0;
	ms->round_frame = 0;
	ms->anim_count = 0;   /* (a new round: everyone's own animations start from none) */
	int m = server->game.map;
	const ModMap *mm = mod_map(m);
	if (ms->has_objects && ms->L)
	{
		obj_clear(&ms->objs);
		memset(ms->sent, 0, sizeof ms->sent);
		ms->gone_count = 0;
		ms->clear_pending = true;
		if (mm)
		{
			cJSON *mj = mod_json(mm->file);
			if (mj)
			{
				obj_spawn_map(&ms->objs, cJSON_GetObjectItem(mj, "objects"));
				cJSON_Delete(mj);
			}
		}
	}
	if (!hook(ms, "on_round_start"))
		return;
	if (mm)
		lua_pushstring(ms->L, mm->file + 5);
	else
		lua_pushinteger(ms->L, m);
	ms_call(ms, 1, 0, "on_round_start");
}

void mod_on_round_end(Server *server, int ending)
{
	ModState *ms = ms_of(server);
	if (hook(ms, "on_round_end"))
	{
		lua_pushstring(ms->L, ending == ED_EXEWIN ? "exe" : ending == ED_SURVWIN ? "survivors" : "time");
		ms_call(ms, 1, 0, "on_round_end");
	}
	if (ms && ms->has_objects && ms->L)
	{
		obj_clear(&ms->objs);
		memset(ms->sent, 0, sizeof ms->sent);
		ms->gone_count = 0;
		ms->clear_pending = true;
		obj_send(ms);
	}
}

static void run_timers(ModState *ms)
{
	lua_State *L = ms->L;
	if (!L || ms->resting)
		return;
	lua_getfield(L, LUA_REGISTRYINDEX, "mod_timers");
	int timers = lua_gettop(L);
	/* collect the due ones first (the table changes while they run) */
	lua_newtable(L);
	int due = lua_gettop(L), n = 0;
	lua_pushnil(L);
	while (lua_next(L, timers))
	{
		lua_pop(L, 1);
		lua_getfield(L, -1, "at");
		lua_Integer at = lua_tointeger(L, -1);
		lua_pop(L, 1);
		if (at <= (lua_Integer)ms->frame)
		{
			lua_pushvalue(L, -1);
			lua_rawseti(L, due, ++n);
		}
	}
	/* in the order they were made (insertion sort on seq: few are due at once) */
	for (int i = 2; i <= n; i++)
	{
		for (int j = i; j > 1; j--)
		{
			lua_rawgeti(L, due, j);
			lua_getfield(L, -1, "seq");
			lua_Integer sj = lua_tointeger(L, -1);
			lua_rawgeti(L, due, j - 1);
			lua_getfield(L, -1, "seq");
			lua_Integer sk = lua_tointeger(L, -1);
			if (sk <= sj)
			{
				lua_pop(L, 4);
				break;
			}
			lua_pop(L, 1);          /* ... tj seqj tk */
			lua_rawseti(L, due, j); /* due[j] = tk */
			lua_pop(L, 1);          /* ... tj */
			lua_rawseti(L, due, j - 1);
		}
	}
	for (int i = 1; i <= n; i++)
	{
		lua_rawgeti(L, due, i);
		int t = lua_gettop(L);
		lua_pushvalue(L, t);
		lua_gettable(L, timers);
		bool there = !lua_isnil(L, -1);  /* (not cancelled by an earlier one of this tick) */
		lua_pop(L, 1);
		if (!there)
		{
			lua_pop(L, 1);
			continue;
		}
		lua_getfield(L, t, "every");
		lua_Integer every = lua_tointeger(L, -1);
		lua_pop(L, 1);
		if (every > 0)
		{
			lua_pushinteger(L, (lua_Integer)ms->frame + every);
			lua_setfield(L, t, "at");
		}
		else
		{
			lua_pushvalue(L, t);
			lua_pushnil(L);
			lua_settable(L, timers);
			ms->timer_count--;
		}
		lua_getfield(L, t, "fn");
		ms_call(ms, 0, 0, "timer");
		lua_pop(L, 1);
	}
	lua_settop(L, timers - 1);
}

void mod_on_tick(Server *server)
{
	ModState *ms = ms_of(server);
	if (!ms || !ms->L)
		return;
	ms->frame++;
	ms->tick_spent = 0;
	sync_flush(ms);
	if (ms->pending_end && server->state == ST_GAME && server->game.started && server->game.end <= 0)
	{
		Ending e = (Ending)(ms->pending_end - 1);
		ms->pending_end = 0;
		game_end(server, e, true);
	}
	run_timers(ms);
	bool playing = server->state == ST_GAME && server->game.started;
	if (playing && hook(ms, "on_tick"))
	{
		lua_pushinteger(ms->L, (lua_Integer)ms->round_frame++);
		ms_call(ms, 1, 0, "on_tick");
	}
	if (ms->has_objects)
	{
		if (playing && !ms->resting)
			obj_tick(&ms->objs);
		if (ms->frame % MOD_OBJ_SEND_TICKS == 0 || ms->clear_pending)
			obj_send(ms);
	}
}

bool mod_on_chat(PeerData *v, const char *text)
{
	ModState *ms = ms_of(v->server);
	if (!hook(ms, "on_chat"))
		return true;
	push_player(ms->L, v);
	lua_pushstring(ms->L, text);
	if (!ms_call(ms, 2, 1, "on_chat"))
		return true;
	bool show = !(lua_isboolean(ms->L, -1) && !lua_toboolean(ms->L, -1));
	lua_pop(ms->L, 1);
	return show;
}

void mod_on_hurt(PeerData *attacker, PeerData *target, int damage, int stun)
{
	ModState *ms = ms_of(target->server);
	if (!attacker || !hook(ms, "on_hurt"))
		return;
	push_player(ms->L, attacker);
	push_player(ms->L, target);
	lua_pushinteger(ms->L, damage);
	lua_pushinteger(ms->L, stun);
	ms_call(ms, 4, 0, "on_hurt");
}

void mod_on_hp(PeerData *v, int old_hp, int hp)
{
	ModState *ms = ms_of(v->server);
	if (!hook(ms, "on_hp"))
		return;
	push_player(ms->L, v);
	lua_pushinteger(ms->L, old_hp);
	lua_pushinteger(ms->L, hp);
	ms_call(ms, 3, 0, "on_hp");
}

uint16_t mod_pick_exe(Server *server, uint16_t chosen)
{
	ModState *ms = ms_of(server);
	if (!ms)
		return chosen;
	uint16_t pick = chosen;
	if (ms->pending_exe)
	{
		PeerData *v = server_find_peer(server, (uint16_t)(ms->pending_exe - 1));
		if (v && v->in_game)
			pick = v->id;
		ms->pending_exe = 0;
	}
	if (hook(ms, "on_choose_exe"))
	{
		lua_pushinteger(ms->L, pick);
		if (ms_call(ms, 1, 1, "on_choose_exe"))
		{
			if (lua_isinteger(ms->L, -1))
			{
				PeerData *v = server_find_peer(server, (uint16_t)lua_tointeger(ms->L, -1));
				if (v && v->in_game)
					pick = v->id;
			}
			lua_pop(ms->L, 1);
		}
	}
	return pick;
}

void mod_on_packet(PeerData *v, Packet *packet, uint8_t type)
{
	if (!g_mod.active)
		return;
	if (type == CLIENT_MOD_READY)
	{
		uint8_t ok = 0;
		packet_read8(packet, &ok);
		v->mod_ready = ok != 0;
		if (!ok)
		{
			/* (not ready, ever: the lobby would wait for them; joining again downloads it again) */
			Warn("[mod] %s could not load the mod: disconnected (they can join again)", v->nickname.value);
			kick_with_text(v, DR_OTHER, "Your game could not load this server's mod. Join again to try once more.",
				"could not load the mod");
			return;
		}
		ModState *ms = ms_of(v->server);
		if (ms && ms->has_objects && ms->L)
			obj_send_all_to(ms, v);
		for (int k = 0; ms && k < ms->anim_count; k++)   /* (the others' own animations now) */
		{
			Packet p;
			if (ms->anims[k].sprite[0] && server_find_peer(v->server, ms->anims[k].id) && packet_new(&p, SERVER_MOD_ANIM))
			{
				packet_write16(&p, ms->anims[k].id);
				packet_writestr(&p, str_cut(ms->anims[k].sprite, 47));
				packet_send(v->peer, &p, true);
			}
		}
		hook_player(v, "on_ready");
		return;
	}
	if (type == CLIENT_MOD_ANIM)
	{
		/* a mod character's own animation (its moves): to the others; at most 30 a second a player */
		ModState *ms = ms_of(v->server);
		String spr;
		if (!ms || !packet_readstr(packet, &spr))
			return;
		if (!rate_ok(v->server, &v->mod_anim_sec, &v->mod_anim_count, 30))
			return;
		char name[48];
		copy_str(name, sizeof name, spr.value);
		for (const char *c = name; *c; c++)
			if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_' || *c == '/' || *c == '.' || *c == '-'))
				return;
		int k;
		for (k = 0; k < ms->anim_count && ms->anims[k].id != v->id; k++)
			;
		if (k == ms->anim_count && k < 64)
			ms->anim_count++;
		if (k < 64)
		{
			ms->anims[k].id = v->id;
			copy_str(ms->anims[k].sprite, sizeof ms->anims[k].sprite, name);
		}
		Packet p;
		if (!packet_new(&p, SERVER_MOD_ANIM))
			return;
		packet_write16(&p, v->id);
		packet_writestr(&p, str_cut(name, 47));
		for (size_t i = 0; i < v->server->peers.capacity; i++)
		{
			PeerData *o = (PeerData *)v->server->peers.ptr[i];
			if (o && o != v && !o->is_bot && !o->disconnecting && o->mod_api >= 2)
				packet_send(o->peer, &p, true);
		}
		return;
	}
	if (type == CLIENT_MOD_EVENT)
	{
		/* (at most 60 a second per player: the web game sends 30, room for the network's bunching) */
		uint32_t now = v->server->tick;
		if (now - v->mod_event_tick >= TICKSPERSEC)
		{
			v->mod_event_tick = now;
			v->mod_event_count = 0;
		}
		if (v->mod_event_count >= 60)
			return;
		v->mod_event_count++;
		String name, data;
		if (!packet_readstr(packet, &name))
			return;
		if (!packet_readstr(packet, &data))
			data.value[0] = 0;
		ModState *ms = ms_of(v->server);
		if (!hook(ms, "on_client_event"))
			return;
		push_player(ms->L, v);
		lua_pushstring(ms->L, name.value);
		lua_pushstring(ms->L, data.value);
		ms_call(ms, 3, 0, "on_client_event");
	}
}
