#include <Config.h>
#include <Log.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io/Dir.h>

#ifdef SYS_ANDROID
	#include <Android.h>
#endif

#ifdef SYS_USE_SDL2
#include <ui/Main.h>
#endif

SERVER_API Config g_config =
{
	.port = 8606,
	.max_simultaneous_players = 100,
	.server_count = 1,

	.ping_limit = 500,

	.log_debug = false,
	.log_file = false,
	.map_list = { true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true },
	.motd = { "welcome to `Viinnyzz~'s bestestserver" },
	.motd_lines = 1,
	.anticheat = false,
	.pride = true,
	.vanilla_balancing = false,
.gamemodes = { false, false, false, false, false, false, false, false, false, false },
	.retained_velocity_decay = 0.85f,
	.retained_velocity_decay_start_ticks = 3,
	.retained_extrapolation_limit_ticks_x = 6,
	.retained_extrapolation_limit_ticks_y = 3,
	.relay_fill_interval_ticks = 3,
	.relay_heartbeat_interval_ticks = 12,
	.relay_correction_interval_ticks = 5 * 60,
	.relay_priority_interval_ticks = 1,

	.votekick_vote_seconds = 20,
	.votekick_start_cooldown_seconds = 30,
	.votekick_lobby_block_seconds = 600,
	.report_cooldown_seconds = 60,

	.web_clients = true,
	.web_proxies = "sonicmb.dr2.site",

	.relay_enabled = false,
	.relay_directory_host = "relaysc.dr2.site",
	.relay_directory_port = 9000,
	.relay_username = "",
	.relay_password = ""
};

static const char *gamemode_names[10] =
{
"protect_the_cream", "oops_all_demons", "oops_no_demons",
"inf_time", "last_survivor_wins", "boss_fight", "timeoverwin", "norevives", "norings",
"onehitchecklist"
};

cJSON*	g_bans = NULL;
cJSON*	g_timeouts = NULL;
cJSON*	g_ops = NULL;
cJSON*	g_reports = NULL;
cJSON*	g_lobbyTimeouts = NULL;
Mutex	g_banMut;
Mutex	g_timeoutMut;
Mutex	g_opMut;
Mutex	g_reportMut;
Mutex	g_lobbyTimeoutMut;

bool write_default(const char* filename, const char* default_str)
{
	FILE* file = fopen(filename, "r");
	if (!file)
	{
		file = fopen(filename, "w");

		if (!file)
		{
			Warn("Failed to open %s for writing.", filename);
			return false;
		}

		fwrite(default_str, 1, strlen(default_str), file);
		fclose(file);
	}

	return true;
}

bool collection_save(const char* file, cJSON* value)
{
	char* buffer = cJSON_Print(value);
	FILE* f = fopen(file, "w");
	if (!f)
	{
		Warn("Failed to open %s for writing.", file);
		return false;
	}

	fwrite(buffer, 1, strlen(buffer), f);
	fclose(f);
	free(buffer);

	return true;
}

bool collection_init(cJSON** output, const char* file, const char* default_value)
{
	RAssert(write_default(file, default_value));

	FILE* f = fopen(file, "r");
	if (!f)
	{
		Warn("Failed to allocate file read buffer.");
		return false;
	}

	fseek(f, 0, SEEK_END);
	size_t len = ftell(f);
	char* buffer = malloc(len);
	if (!buffer)
	{
		Warn("Failed to allocate buffer for a list!");
		return false;
	}

	fseek(f, 0, SEEK_SET);
	fread(buffer, 1, len, f);
	fclose(f);

	*output = cJSON_ParseWithLength(buffer, len);
	if (!(*output))
	{
		Err("Failed to parse %s: %s", file, cJSON_GetErrorPtr());
		*output = cJSON_CreateObject();
		return false;
	}
	else
		Debug("%s loaded.", file);

	free(buffer);
	return true;
}

static cJSON *config_value(cJSON *root, const char *section, const char *nested_key, const char *legacy_key)
{
    cJSON *nested = cJSON_GetObjectItemCaseSensitive(root, section);
    cJSON *value = nested ? cJSON_GetObjectItemCaseSensitive(nested, nested_key) : NULL;
    if (value)
        return value;
    return cJSON_GetObjectItemCaseSensitive(root, legacy_key);
}

static bool config_number(cJSON *root, const char *section, const char *nested_key, const char *legacy_key, int32_t minimum, int32_t maximum, int32_t *out)
{
    cJSON *value = config_value(root, section, nested_key, legacy_key);
    if (!value || !cJSON_IsNumber(value))
    {
        Err("Config field %s.%s must be numeric.", section, nested_key);
        return false;
    }
    double number = cJSON_GetNumberValue(value);
    if (number < minimum || number > maximum || number != (int32_t)number)
    {
        Err("Config field %s.%s must be an integer in [%d, %d].", section, nested_key, minimum, maximum);
        return false;
    }
    *out = (int32_t)number;
    return true;
}

static bool g_config_upgraded = false;

/* For settings introduced after a config file was first written: a missing key
   is not an error, it just keeps the built-in default and flags the file to be
   rewritten so the option becomes visible and editable. A key that IS present
   is validated exactly like a required one. */
static bool config_number_optional(cJSON *root, const char *section, const char *nested_key, const char *legacy_key, int32_t minimum, int32_t maximum, int32_t *out)
{
    cJSON *value = config_value(root, section, nested_key, legacy_key);
    if (!value)
    {
        Info("Config field %s.%s is missing, using the default (%d).", section, nested_key, *out);
        g_config_upgraded = true;
        return true;
    }

    if (!cJSON_IsNumber(value))
    {
        Err("Config field %s.%s must be numeric.", section, nested_key);
        return false;
    }

    double number = cJSON_GetNumberValue(value);
    if (number < minimum || number > maximum || number != (int32_t)number)
    {
        Err("Config field %s.%s must be an integer in [%d, %d].", section, nested_key, minimum, maximum);
        return false;
    }

    *out = (int32_t)number;
    return true;
}

static bool config_string_optional(cJSON *root, const char *section, const char *nested_key, const char *legacy_key, char *out, size_t out_size)
{
    cJSON *value = config_value(root, section, nested_key, legacy_key);
    if (!value)
    {
        g_config_upgraded = true;
        return true;
    }

    const char *text = cJSON_GetStringValue(value);
    if (!text)
    {
        Err("Config field %s.%s must be a string.", section, nested_key);
        return false;
    }

    snprintf(out, out_size, "%s", text);
    return true;
}

static bool config_boolean_optional(cJSON *root, const char *section, const char *nested_key, const char *legacy_key, bool *out)
{
    cJSON *value = config_value(root, section, nested_key, legacy_key);
    if (!value)
    {
        g_config_upgraded = true;
        return true;
    }

    if (!cJSON_IsTrue(value) && !cJSON_IsFalse(value))
    {
        Err("Config field %s.%s must be boolean.", section, nested_key);
        return false;
    }

    *out = cJSON_IsTrue(value);
    return true;
}

static bool config_boolean(cJSON *root, const char *section, const char *nested_key, const char *legacy_key, bool *out)
{
    cJSON *value = config_value(root, section, nested_key, legacy_key);
    if (!value || (!cJSON_IsTrue(value) && !cJSON_IsFalse(value)))
    {
        Err("Config field %s.%s must be boolean.", section, nested_key);
        return false;
    }
    *out = cJSON_IsTrue(value);
    return true;
}

static bool config_string(cJSON *root, const char *section, const char *nested_key, const char *legacy_key, char *out, size_t out_size)
{
    cJSON *value = config_value(root, section, nested_key, legacy_key);
    const char *text = value ? cJSON_GetStringValue(value) : NULL;
    if (!text)
    {
        Err("Config field %s.%s must be a string.", section, nested_key);
        return false;
    }
    snprintf(out, out_size, "%s", text);
    return true;
}

/* message_of_the_day accepts either a single string (the historical format)
   or an array of strings, one chat line each. */
static bool config_motd(cJSON *root)
{
    cJSON *value = config_value(root, "server", "message_of_the_day", "motd");
    g_config.motd_lines = 0;

    if (value && cJSON_IsArray(value))
    {
        cJSON *line = NULL;
        cJSON_ArrayForEach(line, value)
        {
            const char *text = cJSON_GetStringValue(line);
            if (!text)
            {
                Err("Config field server.message_of_the_day must only contain strings.");
                return false;
            }

            if (g_config.motd_lines >= MOTD_MAX_LINES)
            {
                Warn("message_of_the_day has more than %d lines; the rest are ignored.", MOTD_MAX_LINES);
                break;
            }

            snprintf(g_config.motd[g_config.motd_lines], MOTD_LINE_SIZE, "%s", text);
            g_config.motd_lines++;
        }
        return true;
    }

    if (!value)
    {
        Info("Config field server.message_of_the_day is missing, using the default.");
        g_config_upgraded = true;
        g_config.motd_lines = 1;
        return true;
    }

    const char *text = cJSON_GetStringValue(value);
    if (!text)
    {
        Err("Config field server.message_of_the_day must be a string or an array of strings.");
        return false;
    }

    snprintf(g_config.motd[0], MOTD_LINE_SIZE, "%s", text);
    g_config.motd_lines = 1;
    return true;
}

bool config_init(void)
{
	MutexCreate(g_config.map_list_lock);
	
	
	FILE* file = fopen(CONFIG_FILE, "r");
	if (!file)
	{
		RAssert(config_save());

		
		file = fopen(CONFIG_FILE, "r");
		if (!file)
		{
			Warn("Failed to save default config file properly!");
			goto init_balls;
		}
	}

	fseek(file, 0, SEEK_END);
	long file_length = ftell(file);
	if (file_length <= 0)
	{
		fclose(file);
		Err("Config file is empty.");
		return false;
	}
	rewind(file);

	char *buffer = malloc((size_t)file_length + 1);
	if (!buffer)
	{
		fclose(file);
		Err("Failed to allocate config read buffer.");
		return false;
	}

	size_t len = fread(buffer, 1, (size_t)file_length, file);
	fclose(file);
	buffer[len] = '\0';

	cJSON *json = cJSON_ParseWithLength(buffer, len);
	free(buffer);

	if (!json)
	{
		Err("Failed to parse %s: %s", CONFIG_FILE, cJSON_GetErrorPtr());
		return false;
	}
	else
		Debug("%s loaded.", CONFIG_FILE);

	bool valid = true;

	valid &= config_number(json, "server", "port", "port", 1, 65535, &g_config.port);
	valid &= config_number(json, "server", "max_simultaneous_players", "max_simultaneous_players", 1, 100, &g_config.max_simultaneous_players);
	valid &= config_number(json, "server", "server_instance_count", "server_count", 1, 255, &g_config.server_count);
	valid &= config_number(json, "server", "connection_timeout_ping_limit", "ping_limit", 1, 65535, &g_config.ping_limit);
	valid &= config_boolean(json, "logging", "write_logs_to_file", "log_file", &g_config.log_file);
	valid &= config_boolean(json, "logging", "verbose_debug_logging", "log_debug", &g_config.log_debug);
	valid &= config_boolean(json, "gameplay", "anticheat_enabled", "anticheat", &g_config.anticheat);
	valid &= config_boolean(json, "gameplay", "pride_colors_enabled", "pride", &g_config.pride);
	valid &= config_boolean(json, "gameplay", "vanilla_balancing", "vanilla_balancing", &g_config.vanilla_balancing);
	valid &= config_motd(json);

	valid &= config_number_optional(json, "moderation", "votekick_vote_seconds", "votekick_vote_seconds",
		5, 300, &g_config.votekick_vote_seconds);
	valid &= config_number_optional(json, "moderation", "votekick_start_cooldown_seconds", "votekick_start_cooldown_seconds",
		0, 3600, &g_config.votekick_start_cooldown_seconds);
	valid &= config_number_optional(json, "moderation", "votekick_lobby_block_seconds", "votekick_lobby_block_seconds",
		0, 604800, &g_config.votekick_lobby_block_seconds);
	valid &= config_number_optional(json, "moderation", "report_cooldown_seconds", "report_cooldown_seconds",
		0, 3600, &g_config.report_cooldown_seconds);

	valid &= config_boolean_optional(json, "server", "web_clients", "web_clients", &g_config.web_clients);
	valid &= config_string_optional(json, "server", "web_proxies", "web_proxies",
		g_config.web_proxies, sizeof(g_config.web_proxies));
	valid &= config_boolean_optional(json, "relay", "enabled", "relay_enabled", &g_config.relay_enabled);
	valid &= config_string_optional(json, "relay", "directory_host", "relay_directory_host",
		g_config.relay_directory_host, sizeof(g_config.relay_directory_host));
	valid &= config_number_optional(json, "relay", "directory_port", "relay_directory_port",
		1, 65535, &g_config.relay_directory_port);
	valid &= config_string_optional(json, "relay", "username", "relay_username",
		g_config.relay_username, sizeof(g_config.relay_username));
	valid &= config_string_optional(json, "relay", "password", "relay_password",
		g_config.relay_password, sizeof(g_config.relay_password));

cJSON *gamemodes = cJSON_GetObjectItemCaseSensitive(json, "gamemodes");
if (gamemodes)
for (int i = 0; i < 10; i++)
{
cJSON *value = cJSON_GetObjectItemCaseSensitive(gamemodes, gamemode_names[i]);
if (value && cJSON_IsBool(value))
g_config.gamemodes[i] = cJSON_IsTrue(value);
else if (value)
valid = false;
}

	if (!valid)
	{
		cJSON_Delete(json);
		return false;
	}

	/* Reorder an existing config once, retaining its values and any extra keys. */
	if (!g_config_upgraded)
	{
		cJSON *moderation = cJSON_GetObjectItemCaseSensitive(json, "moderation");
		cJSON *relay = cJSON_GetObjectItemCaseSensitive(json, "relay");
		cJSON *last = NULL;
		cJSON *second_last = NULL;
		for (cJSON *item = json->child; item; item = item->next)
		{
			second_last = last;
			last = item;
		}
		if (moderation && relay && (second_last != moderation || last != relay))
		{
			cJSON_AddItemToObject(json, "moderation",
				cJSON_DetachItemFromObjectCaseSensitive(json, "moderation"));
			cJSON_AddItemToObject(json, "relay",
				cJSON_DetachItemFromObjectCaseSensitive(json, "relay"));
			if (!collection_save(CONFIG_FILE, json))
				Warn("Could not reorder moderation and relay in %s.", CONFIG_FILE);
		}
	}
	cJSON_Delete(json);

	/* Bring a config written by an older build up to date on disk, so newly
	   added settings appear instead of silently staying at their defaults. */
	if (g_config_upgraded)
	{
		if (config_save())
			Info("%s was updated with settings added in this build.", CONFIG_FILE);
		else
			Warn("Could not rewrite %s with the new settings.", CONFIG_FILE);
	}

init_balls:
	MutexCreate(g_timeoutMut);
	MutexCreate(g_banMut);
	MutexCreate(g_opMut);
	MutexCreate(g_reportMut);
	MutexCreate(g_lobbyTimeoutMut);

	RAssert(collection_init(&g_timeouts,	TIMEOUTS_FILE,	"{}"));
	RAssert(collection_init(&g_bans,		BANS_FILE,		"{}"));
	RAssert(collection_init(&g_ops,		OPERATORS_FILE, "{ \"127.0.0.1\": \"Host (127.0.0.1)\" }"));
	RAssert(collection_init(&g_reports,	REPORTS_FILE,	"{}"));
	RAssert(collection_init(&g_lobbyTimeouts, LOBBY_TIMEOUTS_FILE, "{}"));

	if (!g_config.anticheat)
	{
		Info(LOG_YLW "Anticheat is disabled, client modifications are allowed.");
	}

	return true;
}

SERVER_API bool config_save(void)
{
	cJSON *json = cJSON_CreateObject();
	cJSON *server = cJSON_CreateObject();
	cJSON *logging = cJSON_CreateObject();
	cJSON *gameplay = cJSON_CreateObject();
cJSON *gamemodes = cJSON_CreateObject();
cJSON *moderation = cJSON_CreateObject();

RAssert(json && server && logging && gameplay && gamemodes && moderation);

	cJSON_AddStringToObject(json, "_readme", "Settings are grouped by topic below. Edit values, save, then restart the server to apply them.");

	cJSON_AddNumberToObject(server, "port", g_config.port);
	cJSON_AddNumberToObject(server, "max_simultaneous_players", g_config.max_simultaneous_players);
	cJSON_AddNumberToObject(server, "server_instance_count", g_config.server_count);
	cJSON_AddNumberToObject(server, "connection_timeout_ping_limit", g_config.ping_limit);
	cJSON *motd = cJSON_CreateArray();
	RAssert(motd);
	for (int i = 0; i < g_config.motd_lines && i < MOTD_MAX_LINES; i++)
		cJSON_AddItemToArray(motd, cJSON_CreateString(g_config.motd[i]));
	cJSON_AddItemToObject(server, "message_of_the_day", motd);
	cJSON_AddStringToObject(server, "_web_clients_note", "web_clients: lets players of the browser version join over WebSocket on the same port numbers (TCP). Forward TCP as well as UDP for them, and UDP on each port + 10000 for their faster WebRTC connection (builds with WebRTC).");
	cJSON_AddBoolToObject(server, "web_clients", g_config.web_clients);
	cJSON_AddStringToObject(server, "_web_proxies_note", "web_proxies: sites that forward phone players (the installed phone app connects through its site). From these addresses the player's real address is taken from the X-Real-IP header. Comma separated names, addresses or a.b.c.d/nn networks; empty to trust none.");
	cJSON_AddStringToObject(server, "web_proxies", g_config.web_proxies);

	cJSON_AddBoolToObject(logging, "write_logs_to_file", g_config.log_file);
	cJSON_AddBoolToObject(logging, "verbose_debug_logging", g_config.log_debug);

	cJSON_AddBoolToObject(gameplay, "anticheat_enabled", g_config.anticheat);
	cJSON_AddBoolToObject(gameplay, "pride_colors_enabled", g_config.pride);
cJSON_AddStringToObject(gameplay, "_vanilla_balancing_note", "vanilla_balancing: set to true to restore vanilla betterserver balance values. Forces max_players=7, reverts custom balancing and restores exclusive character selection. Improved hitboxes are always preserved.");
	cJSON_AddBoolToObject(gameplay, "vanilla_balancing", g_config.vanilla_balancing);
for (int i = 0; i < 10; i++)
cJSON_AddBoolToObject(gamemodes, gamemode_names[i], g_config.gamemodes[i]);

	cJSON_AddNumberToObject(moderation, "votekick_vote_seconds", g_config.votekick_vote_seconds);
	cJSON_AddNumberToObject(moderation, "votekick_start_cooldown_seconds", g_config.votekick_start_cooldown_seconds);
	cJSON_AddStringToObject(moderation, "_votekick_lobby_block_note", "How long a vote-kicked player is kept out of THAT lobby only. They are moved to another lobby instead, and the block survives a restart. 0 disables it.");
	cJSON_AddNumberToObject(moderation, "votekick_lobby_block_seconds", g_config.votekick_lobby_block_seconds);
	cJSON_AddNumberToObject(moderation, "report_cooldown_seconds", g_config.report_cooldown_seconds);

	cJSON *relay = cJSON_CreateObject();
	RAssert(relay);
	cJSON_AddStringToObject(relay, "_note", "Set enabled to true and fill in the account from the website to publish this server through a relay, so no port forwarding is needed. Player addresses are replaced by the relay with stable synthetic ones.");
	cJSON_AddBoolToObject(relay, "enabled", g_config.relay_enabled);
	cJSON_AddStringToObject(relay, "directory_host", g_config.relay_directory_host);
	cJSON_AddNumberToObject(relay, "directory_port", g_config.relay_directory_port);
	cJSON_AddStringToObject(relay, "username", g_config.relay_username);
	cJSON_AddStringToObject(relay, "password", g_config.relay_password);
	cJSON_AddItemToObject(json, "server", server);
	cJSON_AddItemToObject(json, "logging", logging);
	cJSON_AddItemToObject(json, "gameplay", gameplay);
cJSON_AddItemToObject(json, "gamemodes", gamemodes);
	cJSON_AddItemToObject(json, "moderation", moderation);
	cJSON_AddItemToObject(json, "relay", relay);

	bool result = collection_save(CONFIG_FILE, json);
	cJSON_Delete(json);
	return result;
}

bool ban_add(const char* nickname, const char* udid, const char* ip)
{
	bool res = true;

	MutexLock(g_banMut);
	{
		bool changed = false;
		if (!cJSON_HasObjectItem(g_bans, ip))
		{
			cJSON* js = cJSON_CreateString(nickname);
			cJSON_AddItemToObject(g_bans, ip, js);
			changed = true;
		}

		if (!cJSON_HasObjectItem(g_bans, udid))
		{
			cJSON* js = cJSON_CreateString(nickname);
			cJSON_AddItemToObject(g_bans, udid, js);
			changed = true;
		}

		if (changed)
			res = collection_save(BANS_FILE, g_bans);
		
	}
	MutexUnlock(g_banMut);

	return res;
}

bool ban_revoke(const char* udid, const char* ip)
{
	bool res = false;

	MutexLock(g_banMut);
	{
		bool changed = false;

		if (cJSON_HasObjectItem(g_bans, ip))
		{
			cJSON_DeleteItemFromObject(g_bans, ip);
			changed = true;
		}

		if (cJSON_HasObjectItem(g_bans, udid))
		{
			cJSON_DeleteItemFromObject(g_bans, udid);
			changed = true;
		}

		if(changed)
			res = collection_save(BANS_FILE, g_bans);
	}
	MutexUnlock(g_banMut);

	return res;
}

bool ban_check(const char* udid, const char* ip, bool* result)
{
	*result = false;

	MutexLock(g_banMut);
	{
		if (cJSON_HasObjectItem(g_bans, udid) || cJSON_HasObjectItem(g_bans, ip))
			*result = true;
	}
	MutexUnlock(g_banMut);

	return true;
}

bool timeout_set(const char* nickname, const char* udid, const char* ip, uint64_t timestamp)
{
	bool res = true;

	MutexLock(g_timeoutMut);
	{
		bool changed = false;

		cJSON* obj = cJSON_GetObjectItem(g_timeouts, ip);
		if (!obj)
		{
			cJSON* root = cJSON_CreateArray();

			
			cJSON* js = cJSON_CreateString(nickname);
			cJSON_AddItemToArray(root, js);

			
			js = cJSON_CreateNumber((double)timestamp);
			cJSON_AddItemToArray(root, js);

			cJSON_AddItemToObject(g_timeouts, ip, root);
			changed = true;
		}
		else
		{
			cJSON* item = cJSON_GetArrayItem(obj, 1);
			if (item)
			{
				cJSON_SetNumberValue(item, timestamp);
				changed = true;
			}
			else
				Warn("Missing timestamp in array");
		}

		obj = cJSON_GetObjectItem(g_timeouts, udid);
		if (!obj)
		{
			cJSON* root = cJSON_CreateArray();

			
			cJSON* js = cJSON_CreateString(nickname);
			cJSON_AddItemToArray(root, js);

			
			js = cJSON_CreateNumber((double)timestamp);
			cJSON_AddItemToArray(root, js);

			cJSON_AddItemToObject(g_timeouts, udid, root);
			changed = true;
		}
		else
		{
			cJSON* item = cJSON_GetArrayItem(obj, 1);
			if (item)
			{
				cJSON_SetNumberValue(item, timestamp);
				changed = true;
			}
			else
				Warn("Missing timestamp in array");
		}

		if (changed)
			res = collection_save(TIMEOUTS_FILE, g_timeouts);
	}
	MutexUnlock(g_timeoutMut);

	return res;
}

bool timeout_revoke(const char* udid, const char* ip)
{
	bool res = false;

	MutexLock(g_timeoutMut);
	{
		bool changed = false;

		if (cJSON_HasObjectItem(g_timeouts, ip))
		{
			cJSON_DeleteItemFromObject(g_timeouts, ip);
			changed = true;
		}

		if (cJSON_HasObjectItem(g_timeouts, udid))
		{
			cJSON_DeleteItemFromObject(g_timeouts, udid);
			changed = true;
		}

		if (changed)
			res = collection_save(TIMEOUTS_FILE, g_timeouts);
	}
	MutexUnlock(g_timeoutMut);

	return res;
}

bool timeout_check(const char* udid, const char* ip, uint64_t* result)
{
	*result = 0;

	MutexLock(g_timeoutMut);
	{
		cJSON* obj = cJSON_GetObjectItem(g_timeouts, ip);

		if(!obj)
			obj = cJSON_GetObjectItem(g_timeouts, udid);

		if (obj)
		{
			cJSON* timeout = cJSON_GetArrayItem(obj, 1);
			
			if (timeout)
				*result = (uint64_t)cJSON_GetNumberValue(timeout);
			else
				Warn("Missing timestamp in array");
		}
	}
	MutexUnlock(g_timeoutMut);

	return true;
}

#define REPORT_MAX_ENTRIES 50

bool report_add(const char* reporter, const char* target_nickname, const char* target_udid,
	const char* target_ip, const char* reason, int32_t server_id)
{
	bool res = false;

	const char* key = (target_udid && target_udid[0]) ? target_udid : target_ip;
	if (!key || !key[0])
		return false;

	MutexLock(g_reportMut);
	{
		cJSON* entry = cJSON_GetObjectItem(g_reports, key);
		if (!entry)
		{
			entry = cJSON_CreateObject();
			if (entry)
			{
				cJSON_AddStringToObject(entry, "nickname", target_nickname ? target_nickname : "");
				cJSON_AddStringToObject(entry, "ip", target_ip ? target_ip : "");
				cJSON_AddNumberToObject(entry, "count", 0);
				cJSON_AddItemToObject(entry, "reports", cJSON_CreateArray());
				cJSON_AddItemToObject(g_reports, key, entry);
			}
		}

		if (entry)
		{
			/* Refresh the display name; players rename between reports. */
			cJSON* name = cJSON_GetObjectItem(entry, "nickname");
			if (name && target_nickname)
				cJSON_SetValuestring(name, target_nickname);

			cJSON* list = cJSON_GetObjectItem(entry, "reports");
			cJSON* record = cJSON_CreateObject();
			if (list && record)
			{
				cJSON_AddStringToObject(record, "by", reporter ? reporter : "");
				cJSON_AddStringToObject(record, "reason", reason ? reason : "");
				cJSON_AddNumberToObject(record, "time", (double)time(NULL));
				cJSON_AddNumberToObject(record, "server", server_id);
				cJSON_AddItemToArray(list, record);

				/* Cap the history so a busy server cannot grow the file forever. */
				while (cJSON_GetArraySize(list) > REPORT_MAX_ENTRIES)
					cJSON_DeleteItemFromArray(list, 0);

				cJSON* count = cJSON_GetObjectItem(entry, "count");
				if (count)
					cJSON_SetNumberValue(count, count->valuedouble + 1);

				res = collection_save(REPORTS_FILE, g_reports);
			}
			else if (record)
				cJSON_Delete(record);
		}
	}
	MutexUnlock(g_reportMut);

	return res;
}

bool report_clear(const char* key)
{
	bool res = false;

	MutexLock(g_reportMut);
	{
		if (key && cJSON_HasObjectItem(g_reports, key))
		{
			cJSON_DeleteItemFromObject(g_reports, key);
			res = collection_save(REPORTS_FILE, g_reports);
		}
	}
	MutexUnlock(g_reportMut);

	return res;
}

bool report_clear_all(void)
{
	bool res;

	MutexLock(g_reportMut);
	{
		cJSON_Delete(g_reports);
		g_reports = cJSON_CreateObject();
		res = g_reports && collection_save(REPORTS_FILE, g_reports);
	}
	MutexUnlock(g_reportMut);

	return res;
}

bool op_add(const char* nickname, const char* ip)
{
	bool res = true;

	MutexLock(g_opMut);
	{
		if (!cJSON_HasObjectItem(g_ops, ip))
		{
			cJSON* js = cJSON_CreateString(nickname);
			cJSON_AddItemToObject(g_ops, ip, js);

			res = collection_save(OPERATORS_FILE, g_ops);
		}
	}
	MutexUnlock(g_opMut);

	return res;
}

bool op_revoke(const char* ip)
{
	bool res = false;

	MutexLock(g_opMut);
	{
		if (cJSON_HasObjectItem(g_ops, ip))
		{
			cJSON_DeleteItemFromObject(g_ops, ip);

			res = collection_save(OPERATORS_FILE, g_ops);
		}
	}
	MutexUnlock(g_opMut);

	return res;
}

bool op_check(const char* ip, bool* result)
{
	*result = false;

	MutexLock(g_opMut);
	{
		if (cJSON_HasObjectItem(g_ops, ip))
			*result = true;
	}
	MutexUnlock(g_opMut);

	return true;
}

/* ---------------------------------------------------------------------------
   Per-lobby timeouts.

   Shape, keyed by the player's UDID and by their IP so either identifies them:

     { "<udid-or-ip>": { "nickname": "x", "lobbies": { "0": 1790000000 } } }

   The number is a unix expiry timestamp, so a restart does not forget the block
   and an expired block disappears the next time it is looked at.
   --------------------------------------------------------------------------- */

static void lobby_timeout_put(const char* key, const char* nickname, int32_t server_id, uint64_t until)
{
	char lobby_key[16];
	snprintf(lobby_key, sizeof(lobby_key), "%d", server_id);

	cJSON* entry = cJSON_GetObjectItem(g_lobbyTimeouts, key);
	if (!entry)
	{
		entry = cJSON_CreateObject();
		if (!entry)
			return;

		cJSON_AddStringToObject(entry, "nickname", nickname ? nickname : "");
		cJSON_AddItemToObject(entry, "lobbies", cJSON_CreateObject());
		cJSON_AddItemToObject(g_lobbyTimeouts, key, entry);
	}

	cJSON* name = cJSON_GetObjectItem(entry, "nickname");
	if (name && nickname)
		cJSON_SetValuestring(name, nickname);

	cJSON* lobbies = cJSON_GetObjectItem(entry, "lobbies");
	if (!lobbies)
	{
		lobbies = cJSON_CreateObject();
		if (!lobbies)
			return;
		cJSON_AddItemToObject(entry, "lobbies", lobbies);
	}

	cJSON* slot = cJSON_GetObjectItem(lobbies, lobby_key);
	if (slot)
		cJSON_SetNumberValue(slot, (double)until);
	else
		cJSON_AddNumberToObject(lobbies, lobby_key, (double)until);
}

bool lobby_timeout_set(const char* nickname, const char* udid, const char* ip,
	int32_t server_id, uint64_t until)
{
	bool res;

	MutexLock(g_lobbyTimeoutMut);
	{
		if (udid && udid[0])
			lobby_timeout_put(udid, nickname, server_id, until);
		if (ip && ip[0])
			lobby_timeout_put(ip, nickname, server_id, until);

		res = collection_save(LOBBY_TIMEOUTS_FILE, g_lobbyTimeouts);
	}
	MutexUnlock(g_lobbyTimeoutMut);

	return res;
}

/* Returns the expiry still in force for this key/lobby, or 0. Prunes anything
   that has already run out. *changed is set when the store was modified. */
static uint64_t lobby_timeout_peek(const char* key, const char* lobby_key, uint64_t now, bool* changed)
{
	if (!key || !key[0])
		return 0;

	cJSON* entry = cJSON_GetObjectItem(g_lobbyTimeouts, key);
	if (!entry)
		return 0;

	cJSON* lobbies = cJSON_GetObjectItem(entry, "lobbies");
	if (!lobbies)
		return 0;

	cJSON* slot = cJSON_GetObjectItem(lobbies, lobby_key);
	if (!slot)
		return 0;

	uint64_t until = (uint64_t)slot->valuedouble;
	if (until > now)
		return until;

	cJSON_DeleteItemFromObject(lobbies, lobby_key);
	if (!cJSON_GetArraySize(lobbies))
		cJSON_DeleteItemFromObject(g_lobbyTimeouts, key);

	*changed = true;
	return 0;
}

bool lobby_timeout_check(const char* udid, const char* ip, int32_t server_id, uint64_t* until)
{
	char lobby_key[16];
	snprintf(lobby_key, sizeof(lobby_key), "%d", server_id);

	uint64_t now = (uint64_t)time(NULL);
	uint64_t found = 0;
	bool changed = false;

	MutexLock(g_lobbyTimeoutMut);
	{
		uint64_t by_udid = lobby_timeout_peek(udid, lobby_key, now, &changed);
		uint64_t by_ip = lobby_timeout_peek(ip, lobby_key, now, &changed);

		found = by_udid > by_ip ? by_udid : by_ip;

		if (changed)
			collection_save(LOBBY_TIMEOUTS_FILE, g_lobbyTimeouts);
	}
	MutexUnlock(g_lobbyTimeoutMut);

	if (until)
		*until = found;

	return true;
}

bool lobby_timeout_revoke(const char* udid, const char* ip, int32_t server_id)
{
	char lobby_key[16];
	snprintf(lobby_key, sizeof(lobby_key), "%d", server_id);

	bool changed = false;

	MutexLock(g_lobbyTimeoutMut);
	{
		const char* keys[2] = { udid, ip };
		for (int i = 0; i < 2; i++)
		{
			if (!keys[i] || !keys[i][0])
				continue;

			cJSON* entry = cJSON_GetObjectItem(g_lobbyTimeouts, keys[i]);
			cJSON* lobbies = entry ? cJSON_GetObjectItem(entry, "lobbies") : NULL;
			if (!lobbies || !cJSON_GetObjectItem(lobbies, lobby_key))
				continue;

			cJSON_DeleteItemFromObject(lobbies, lobby_key);
			if (!cJSON_GetArraySize(lobbies))
				cJSON_DeleteItemFromObject(g_lobbyTimeouts, keys[i]);

			changed = true;
		}

		if (changed)
			collection_save(LOBBY_TIMEOUTS_FILE, g_lobbyTimeouts);
	}
	MutexUnlock(g_lobbyTimeoutMut);

	return changed;
}

bool lobby_timeout_clear_all(void)
{
	bool res;

	MutexLock(g_lobbyTimeoutMut);
	{
		cJSON_Delete(g_lobbyTimeouts);
		g_lobbyTimeouts = cJSON_CreateObject();
		res = g_lobbyTimeouts && collection_save(LOBBY_TIMEOUTS_FILE, g_lobbyTimeouts);
	}
	MutexUnlock(g_lobbyTimeoutMut);

	return res;
}
