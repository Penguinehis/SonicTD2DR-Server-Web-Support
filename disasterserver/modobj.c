/* Mods' objects (mod API 2): see modobj.h. The same file on the server and in the web game. */
#include "modobj.h"

#if defined(__has_include)
#if __has_include("lua/lua.h")
#include "lua/lauxlib.h"
#include "lua/lua.h"
#else
#include <lauxlib.h>
#include <lua.h>
#endif
#else
#include <lauxlib.h>
#include <lua.h>
#endif
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *EV_NAMES[OBJ_EV_COUNT] = { "create", "step", "touch", "leave", "destroy" };

static void warnf(ObjEngine *e, const char *fmt, const char *a, const char *b)
{
	char t[200];
	snprintf(t, sizeof t, fmt, a, b ? b : "");
	if (e->host.warn)
		e->host.warn(e->host.ud, t);
}

static bool valid_name(const char *s, size_t n)
{
	if (n == 0 || n > 31)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
			return false;
	return true;
}

int obj_type_index(const ObjEngine *e, const char *name)
{
	for (int i = 0; i < e->ntypes; i++)
		if (!strcmp(e->types[i].name, name))
			return i;
	return -1;
}

static float num_or(lua_State *L, int idx, const char *k, float def)
{
	lua_getfield(L, idx, k);
	float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
	lua_pop(L, 1);
	return isfinite(v) ? v : def;
}

/* ------------------------------------------------------------------ the instances' state (self) */

static void read_back(ObjEngine *e, ObjInst *o)
{
	lua_State *L = e->L;
	lua_rawgeti(L, LUA_REGISTRYINDEX, o->ref);
	int t = lua_gettop(L);
	o->x = num_or(L, t, "x", o->x);
	o->y = num_or(L, t, "y", o->y);
	o->vx = num_or(L, t, "vx", 0);
	o->vy = num_or(L, t, "vy", 0);
	o->xscale = num_or(L, t, "xscale", 1);
	o->yscale = num_or(L, t, "yscale", 1);
	o->alpha = num_or(L, t, "alpha", 1);
	if (o->alpha < 0) o->alpha = 0;
	if (o->alpha > 1) o->alpha = 1;
	o->angle = num_or(L, t, "angle", 0);
	/* color: 0xRRGGBB or "#rrggbb"; blend: "add" */
	lua_getfield(L, t, "color");
	o->color = 0xFFFFFF;
	if (lua_type(L, -1) == LUA_TNUMBER)
		o->color = (uint32_t)((int64_t)lua_tonumber(L, -1) & 0xFFFFFF);
	else if (lua_type(L, -1) == LUA_TSTRING)
	{
		const char *c = lua_tostring(L, -1);
		unsigned v;
		if (c[0] == '#' && strlen(c) == 7 && sscanf(c + 1, "%6x", &v) == 1)
			o->color = v;
	}
	lua_pop(L, 1);
	lua_getfield(L, t, "blend");
	o->blend_add = lua_type(L, -1) == LUA_TSTRING && !strcmp(lua_tostring(L, -1), "add");
	lua_pop(L, 1);
	lua_getfield(L, t, "sprite");
	const char *s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
	snprintf(o->sprite, sizeof o->sprite, "%s", s);
	lua_pop(L, 1);
	lua_getfield(L, t, "frame");
	o->frame = lua_isnumber(L, -1) ? (int)lua_tonumber(L, -1) : -1;
	if (o->frame < -1) o->frame = -1;
	if (o->frame > 4095) o->frame = 4095;
	lua_pop(L, 1);
	lua_getfield(L, t, "visible");
	o->visible = lua_isnil(L, -1) || lua_toboolean(L, -1);
	lua_pop(L, 1);
	lua_getfield(L, t, "solid");
	o->solid_off = !lua_isnil(L, -1) && !lua_toboolean(L, -1);
	lua_pop(L, 2);
}

static void set_num(lua_State *L, int t, const char *k, lua_Number v)
{
	lua_pushnumber(L, v);
	lua_setfield(L, t, k);
}

ObjInst *obj_find(ObjEngine *e, uint16_t id)
{
	for (int i = 0; i < OBJ_MAX; i++)
		if (e->inst[i].used && !e->inst[i].dead && e->inst[i].id == id)
			return &e->inst[i];
	return NULL;
}

int obj_count(const ObjEngine *e)
{
	int n = 0;
	for (int i = 0; i < OBJ_MAX; i++)
		n += e->inst[i].used && !e->inst[i].dead;
	return n;
}

/* calls the event on the instance (with the extra arguments already on the stack) */
static bool event(ObjEngine *e, ObjInst *o, int ev, int nargs)
{
	lua_State *L = e->L;
	ObjType *t = &e->types[o->type];
	if (!t->has[ev])
	{
		lua_pop(L, nargs);
		return true;
	}
	lua_rawgeti(L, LUA_REGISTRYINDEX, t->env);
	lua_getfield(L, -1, EV_NAMES[ev]);
	lua_remove(L, -2);
	if (!lua_isfunction(L, -1))
	{
		lua_pop(L, nargs + 1);
		return true;
	}
	lua_insert(L, -1 - nargs);
	lua_rawgeti(L, LUA_REGISTRYINDEX, o->ref);
	lua_insert(L, -1 - nargs);
	char where[64];
	snprintf(where, sizeof where, "objects/%s.lua %s", t->name, EV_NAMES[ev]);
	return e->host.call(e->host.ud, L, nargs + 1, 0, where);
}

static void destroy(ObjEngine *e, ObjInst *o, bool events)
{
	if (!o->used || o->dead)
		return;
	o->dead = true;
	if (events)
		event(e, o, OBJ_EV_DESTROY, 0);
	if (e->host.changed && !e->clearing)
		e->host.changed(e->host.ud, o, OBJ_DESTROYED);
	luaL_unref(e->L, LUA_REGISTRYINDEX, o->ref);
	o->ref = LUA_NOREF;
	o->used = false;
}

ObjInst *obj_spawn(ObjEngine *e, int type, float x, float y)
{
	if (type < 0 || type >= e->ntypes)
		return NULL;
	ObjInst *o = NULL;
	for (int i = 0; i < OBJ_MAX && !o; i++)
		if (!e->inst[i].used)
			o = &e->inst[i];
	if (!o)
		return NULL;
	do
		e->next_id = (uint16_t)(e->next_id + 1);
	while (e->next_id == 0 || obj_find(e, e->next_id));
	ObjType *t = &e->types[type];
	lua_State *L = e->L;
	memset(o, 0, sizeof *o);
	o->used = true;
	o->id = e->next_id;
	o->type = (uint8_t)type;
	o->x = x;
	o->y = y;
	o->xscale = o->yscale = o->alpha = 1;
	o->visible = true;
	o->color = t->color;
	o->blend_add = t->blend_add;
	o->frame = -1;
	snprintf(o->sprite, sizeof o->sprite, "%s", t->sprite);
	lua_createtable(L, 0, 16);
	int s = lua_gettop(L);
	if (t->vars != LUA_NOREF)
	{
		lua_rawgeti(L, LUA_REGISTRYINDEX, t->vars);
		lua_pushnil(L);
		while (lua_next(L, -2))
		{
			lua_pushvalue(L, -2);
			lua_insert(L, -2);
			lua_settable(L, s);
		}
		lua_pop(L, 1);
	}
	lua_pushinteger(L, o->id);
	lua_setfield(L, s, "id");
	lua_pushstring(L, t->name);
	lua_setfield(L, s, "type");
	set_num(L, s, "x", x);
	set_num(L, s, "y", y);
	set_num(L, s, "vx", 0);
	set_num(L, s, "vy", 0);
	set_num(L, s, "xscale", 1);
	set_num(L, s, "yscale", 1);
	set_num(L, s, "alpha", 1);
	set_num(L, s, "angle", 0);
	lua_pushboolean(L, 1);
	lua_setfield(L, s, "visible");
	if (t->color != 0xFFFFFF)
	{
		char hex[8];
		snprintf(hex, sizeof hex, "#%06x", (unsigned)t->color);
		lua_pushstring(L, hex);
		lua_setfield(L, s, "color");
	}
	if (t->blend_add)
	{
		lua_pushstring(L, "add");
		lua_setfield(L, s, "blend");
	}
	if (t->sprite[0])
	{
		lua_pushstring(L, t->sprite);
		lua_setfield(L, s, "sprite");
	}
	o->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	uint16_t id = o->id;
	event(e, o, OBJ_EV_CREATE, 0);
	o = obj_find(e, id);   /* (create may have destroyed it) */
	if (!o)
		return NULL;
	read_back(e, o);
	if (e->host.changed)
		e->host.changed(e->host.ud, o, OBJ_CREATED);
	return o;
}

/* ------------------------------------------------------------------ game.spawn / destroy / objects / object */

static ObjEngine *eng(lua_State *L) { return (ObjEngine *)lua_touserdata(L, lua_upvalueindex(1)); }

static ObjInst *arg_inst(lua_State *L, int arg)
{
	lua_Integer id;
	if (lua_istable(L, arg))
	{
		lua_getfield(L, arg, "id");
		id = lua_tointeger(L, -1);
		lua_pop(L, 1);
	}
	else
		id = luaL_checkinteger(L, arg);
	return id > 0 && id <= 0xFFFF ? obj_find(eng(L), (uint16_t)id) : NULL;
}

static int l_spawn(lua_State *L)
{
	ObjEngine *e = eng(L);
	const char *name = luaL_checkstring(L, 1);
	float x = (float)luaL_checknumber(L, 2), y = (float)luaL_checknumber(L, 3);
	int type = obj_type_index(e, name);
	if (type < 0)
		return luaL_error(L, "game.spawn: no object \"%s\" (objects/%s.json)", name, name);
	if (!isfinite(x) || !isfinite(y))
		return luaL_error(L, "game.spawn: x and y must be numbers");
	if (e->clearing)
		return 0;
	ObjInst *o = obj_spawn(e, type, x, y);
	if (!o)
	{
		if (obj_count(e) >= OBJ_MAX)
			return luaL_error(L, "game.spawn: %d objects at most", OBJ_MAX);
		lua_pushnil(L);
		return 1;
	}
	lua_rawgeti(L, LUA_REGISTRYINDEX, o->ref);
	if (lua_istable(L, 4))   /* game.spawn(name, x, y, {fields}) */
	{
		int s = lua_gettop(L);
		lua_pushnil(L);
		while (lua_next(L, 4))
		{
			lua_pushvalue(L, -2);
			lua_insert(L, -2);
			lua_settable(L, s);
		}
		read_back(e, o);
	}
	return 1;
}

static int l_destroy(lua_State *L)
{
	ObjEngine *e = eng(L);
	ObjInst *o = arg_inst(L, 1);
	if (o)
		destroy(e, o, true);
	return 0;
}

static int l_objects(lua_State *L)
{
	ObjEngine *e = eng(L);
	const char *only = luaL_optstring(L, 1, NULL);
	int type = only ? obj_type_index(e, only) : -1;
	lua_newtable(L);
	int n = 0;
	if (only && type < 0)
		return 1;
	for (int i = 0; i < OBJ_MAX; i++)
	{
		ObjInst *o = &e->inst[i];
		if (!o->used || o->dead || (only && o->type != type))
			continue;
		lua_rawgeti(L, LUA_REGISTRYINDEX, o->ref);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int l_object(lua_State *L)
{
	ObjInst *o = arg_inst(L, 1);
	if (o)
		lua_rawgeti(L, LUA_REGISTRYINDEX, o->ref);
	else
		lua_pushnil(L);
	return 1;
}

/* ------------------------------------------------------------------ the types */

static void load_type(ObjEngine *e, const char *name)
{
	lua_State *L = e->L;
	char path[64];
	snprintf(path, sizeof path, "objects/%s.json", name);
	uint32_t size;
	const uint8_t *d = e->host.file(e->host.ud, path, &size);
	cJSON *j = d ? cJSON_ParseWithLength((const char *)d, size) : NULL;
	if (!cJSON_IsObject(j))
	{
		warnf(e, "%s is not valid JSON: the object is left out%s", path, NULL);
		cJSON_Delete(j);
		return;
	}
	ObjType *t = &e->types[e->ntypes];
	memset(t, 0, sizeof *t);
	snprintf(t->name, sizeof t->name, "%s", name);
	cJSON *v = cJSON_GetObjectItem(j, "sprite");
	if (cJSON_IsString(v))
		snprintf(t->sprite, sizeof t->sprite, "%s", v->valuestring);
	t->box[0] = -8;
	t->box[1] = -8;
	t->box[2] = 16;
	t->box[3] = 16;
	v = cJSON_GetObjectItem(j, "box");
	if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 4)
		for (int i = 0; i < 4; i++)
		{
			cJSON *n = cJSON_GetArrayItem(v, i);
			if (cJSON_IsNumber(n) && isfinite(n->valuedouble) && fabs(n->valuedouble) < 4096)
				t->box[i] = (float)n->valuedouble;
		}
	if (t->box[2] < 1) t->box[2] = 1;
	if (t->box[3] < 1) t->box[3] = 1;
	t->front = cJSON_IsTrue(cJSON_GetObjectItem(j, "front"));
	t->color = 0xFFFFFF;
	v = cJSON_GetObjectItem(j, "color");
	unsigned col;
	if (cJSON_IsString(v) && v->valuestring[0] == '#' && strlen(v->valuestring) == 7 && sscanf(v->valuestring + 1, "%6x", &col) == 1)
		t->color = col;
	v = cJSON_GetObjectItem(j, "blend");
	t->blend_add = cJSON_IsString(v) && !strcmp(v->valuestring, "add");
	t->vars = LUA_NOREF;
	v = cJSON_GetObjectItem(j, "vars");
	if (cJSON_IsObject(v))
	{
		lua_newtable(L);
		cJSON *f;
		cJSON_ArrayForEach(f, v)
		{
			if (cJSON_IsNumber(f))
				lua_pushnumber(L, f->valuedouble);
			else if (cJSON_IsBool(f))
				lua_pushboolean(L, cJSON_IsTrue(f));
			else if (cJSON_IsString(f))
				lua_pushstring(L, f->valuestring);
			else
				continue;
			lua_setfield(L, -2, f->string);
		}
		t->vars = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	cJSON_Delete(j);

	/* its events: objects/<name>.lua in an environment of its own (it reads the globals: game, mod, ...) */
	lua_newtable(L);
	lua_newtable(L);
	lua_pushglobaltable(L);
	lua_setfield(L, -2, "__index");
	lua_setmetatable(L, -2);
	t->env = luaL_ref(L, LUA_REGISTRYINDEX);
	snprintf(path, sizeof path, "objects/%s.lua", name);
	d = e->host.file(e->host.ud, path, &size);
	if (d)
	{
		char chunk[72];
		snprintf(chunk, sizeof chunk, "=%s", path);
		if (luaL_loadbufferx(L, (const char *)d, size, chunk, "t") != LUA_OK)
		{
			warnf(e, "%s: %s", path, lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		else
		{
			lua_rawgeti(L, LUA_REGISTRYINDEX, t->env);
			lua_setupvalue(L, -2, 1);   /* (_ENV) */
			e->host.call(e->host.ud, L, 0, 0, path);
		}
		lua_rawgeti(L, LUA_REGISTRYINDEX, t->env);
		for (int i = 0; i < OBJ_EV_COUNT; i++)
		{
			lua_getfield(L, -1, EV_NAMES[i]);
			t->has[i] = lua_isfunction(L, -1);
			lua_pop(L, 1);
		}
		/* from now on a new name an event sets is a global of the game rules (COUNT = COUNT + 1), as in
		   server.lua; what the file set at its top (its events, its own variables) stays its own */
		lua_getmetatable(L, -1);
		lua_pushglobaltable(L);
		lua_setfield(L, -2, "__newindex");
		lua_pop(L, 2);
	}
	e->ntypes++;
}

bool obj_init(ObjEngine *e, lua_State *L, const ObjHost *host, const char *const *names, int count)
{
	memset(e, 0, sizeof *e);
	e->host = *host;
	e->L = L;
	for (int i = 0; i < OBJ_MAX; i++)
		e->inst[i].ref = LUA_NOREF;
	for (int i = 0; i < count && e->ntypes < OBJ_TYPES; i++)
	{
		const char *n = names[i];
		size_t len = strlen(n);
		if (strncmp(n, "objects/", 8) || len < 14 || strcmp(n + len - 5, ".json"))
			continue;
		char name[40];
		size_t nl = len - 13;
		if (!valid_name(n + 8, nl))
			continue;
		memcpy(name, n + 8, nl);
		name[nl] = 0;
		load_type(e, name);
	}
	lua_getglobal(L, "game");
	if (lua_istable(L, -1))
	{
		static const struct { const char *name; lua_CFunction fn; } F[] = {
			{ "spawn", l_spawn }, { "destroy", l_destroy }, { "objects", l_objects }, { "object", l_object } };
		for (size_t i = 0; i < sizeof F / sizeof F[0]; i++)
		{
			lua_pushlightuserdata(L, e);
			lua_pushcclosure(L, F[i].fn, 1);
			lua_setfield(L, -2, F[i].name);
		}
	}
	lua_pop(L, 1);
	return true;
}

void obj_spawn_map(ObjEngine *e, const cJSON *objects)
{
	const cJSON *o;
	cJSON_ArrayForEach(o, objects)
	{
		const cJSON *t = cJSON_GetObjectItem(o, "type"), *n = cJSON_GetObjectItem(o, "object");
		if (!cJSON_IsString(t) || strcmp(t->valuestring, "object") || !cJSON_IsString(n))
			continue;
		int type = obj_type_index(e, n->valuestring);
		const cJSON *x = cJSON_GetObjectItem(o, "x"), *y = cJSON_GetObjectItem(o, "y");
		if (type < 0 || !cJSON_IsNumber(x) || !cJSON_IsNumber(y))
			continue;
		if (!obj_spawn(e, type, (float)x->valuedouble, (float)y->valuedouble) && obj_count(e) >= OBJ_MAX)
			break;
	}
}

void obj_clear(ObjEngine *e)
{
	e->clearing = true;
	for (int i = 0; i < OBJ_MAX; i++)
		if (e->inst[i].used)
			destroy(e, &e->inst[i], false);
	e->clearing = false;
}

/* ------------------------------------------------------------------ a tick */

static bool overlap(const ObjType *t, const ObjInst *o, const ObjPlayer *p)
{
	float sx = fabsf(o->xscale), sy = fabsf(o->yscale);
	float l = t->box[0] * sx, w = t->box[2] * sx, tp = t->box[1] * sy, h = t->box[3] * sy;
	if (o->xscale < 0)
		l = -l - w;   /* (mirrored) */
	if (o->yscale < 0)
		tp = -tp - h;
	float ax = o->x + l, ay = o->y + tp;
	bool own = p->bw > 0 && p->bh > 0;
	float bx = p->x + (own ? p->bl : OBJ_PLAYER_BOX_L), by = p->y + (own ? p->bt : OBJ_PLAYER_BOX_T);
	float bw = own ? p->bw : OBJ_PLAYER_BOX_W, bh = own ? p->bh : OBJ_PLAYER_BOX_H;
	return ax < bx + bw && bx < ax + w && ay < by + bh && by < ay + h;
}

void obj_tick(ObjEngine *e)
{
	lua_State *L = e->L;
	uint16_t ids[OBJ_MAX];
	int n = 0;
	for (int i = 0; i < OBJ_MAX; i++)
		if (e->inst[i].used && !e->inst[i].dead)
			ids[n++] = e->inst[i].id;
	/* moved by their speed, then their step */
	for (int i = 0; i < n; i++)
	{
		ObjInst *o = obj_find(e, ids[i]);
		if (!o)
			continue;
		lua_rawgeti(L, LUA_REGISTRYINDEX, o->ref);
		int s = lua_gettop(L);
		float vx = num_or(L, s, "vx", 0), vy = num_or(L, s, "vy", 0);
		if (vx != 0 || vy != 0)
		{
			set_num(L, s, "x", num_or(L, s, "x", o->x) + vx);
			set_num(L, s, "y", num_or(L, s, "y", o->y) + vy);
		}
		lua_pop(L, 1);
		event(e, o, OBJ_EV_STEP, 0);
	}
	/* touches: a player's box over the object's */
	ObjPlayer pl[64];
	memset(pl, 0, sizeof pl);   /* (a host that gives no box: the default one) */
	int np = e->host.players ? e->host.players(e->host.ud, pl, 64) : 0;
	for (int i = 0; i < n; i++)
	{
		ObjInst *o = obj_find(e, ids[i]);
		if (!o)
			continue;
		read_back(e, o);
		const ObjType *t = &e->types[o->type];
		bool want = t->has[OBJ_EV_TOUCH] || t->has[OBJ_EV_LEAVE];
		uint16_t now[OBJ_PLAYERS];
		int nn = 0;
		for (int k = 0; k < np && nn < OBJ_PLAYERS; k++)
			if (pl[k].alive && o->visible && overlap(t, o, &pl[k]))
				now[nn++] = pl[k].id;
		uint16_t before[OBJ_PLAYERS];
		int nb = o->ntouch;
		memcpy(before, o->touching, sizeof before);
		memcpy(o->touching, now, sizeof now);
		o->ntouch = nn;
		if (!want)
			continue;
		for (int k = 0; k < nn; k++)
		{
			bool was = false;
			for (int m = 0; m < nb && !was; m++)
				was = before[m] == now[k];
			if (was)
				continue;
			e->host.push_player(e->host.ud, L, now[k]);
			event(e, o, OBJ_EV_TOUCH, 1);
			if (!(o = obj_find(e, ids[i])))
				break;
		}
		if (!o)
			continue;
		for (int m = 0; m < nb; m++)
		{
			bool still = false;
			for (int k = 0; k < nn && !still; k++)
				still = now[k] == before[m];
			if (still)
				continue;
			e->host.push_player(e->host.ud, L, before[m]);
			if (lua_isnil(L, -1))
			{
				lua_pop(L, 1);   /* (they left the server) */
				continue;
			}
			event(e, o, OBJ_EV_LEAVE, 1);
			if (!(o = obj_find(e, ids[i])))
				break;
		}
	}
	for (int i = 0; i < OBJ_MAX; i++)
		if (e->inst[i].used && !e->inst[i].dead)
			read_back(e, &e->inst[i]);
}
