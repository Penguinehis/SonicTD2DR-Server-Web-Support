#include <Mod.h>
#include <enet/enet.h>
#include <io/Dir.h>
#include <io/Threads.h>
#include <Lib.h>
#include <Relay.h>
#include <WebGate.h>
#include <RelayProto.h>
#include <Log.h>
#include <Server.h>
#include <States.h>
#include <DyList.h>
#include <Config.h>
#include <io/Socket.h>

ThreadVar		g_threadName;
DyList			servers;
static atomic_bool running = false;
static bool initialized = false;
static Thread *g_workers = NULL;
static bool *g_worker_started = NULL;

bool allocate_server(uint16_t base_port, uint16_t n)
{
	const Server templ = {
		.state = ST_LOBBY,
		.last_map = -1,
		.id = n,
		.running = true,
		.delta = 1,
		.game = {
			.exe = -1
		},
		.lobby = {
			.vote = {
				.ongoing = 0,
			}
		}
	};

	Server* server = malloc(sizeof(Server));
	RAssert(server);
	memcpy(server, &templ, sizeof(Server));
	atomic_init(&server->running, true);

	
	MutexCreate(server->state_lock);
	RAssert(dylist_create(&server->peers, (size_t)g_config.max_simultaneous_players));
	RAssert(mod_server_init(server));
	
	ENetAddress addr;
	addr.host = ENET_HOST_ANY;
	addr.port = base_port + n;
	server->host = enet_host_create(&addr, (size_t)g_config.max_simultaneous_players, 2, 0, 0);

	Info("Listening on port %d.", base_port + n);
	RAssert(server->host != NULL);

	/* When the relay carries our traffic, every datagram is wrapped with a
	   nonce and an authentication tag. ENet's default MTU of 1400 leaves no
	   room for that, so full-size packets could not be forwarded and clients
	   failed partway through joining. ENet takes the smaller of the two MTUs
	   during the handshake, so pinning ours here keeps every client's
	   datagrams inside what the tunnel can carry. */
	if (g_config.relay_enabled && server->host->mtu > RELAY_MAX_PAYLOAD)
		server->host->mtu = RELAY_MAX_PAYLOAD;

int bufsize = 512 * 1024;
setsockopt(server->host->socket, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));
setsockopt(server->host->socket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));

int tos = 0xB8;
setsockopt(server->host->socket, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof(tos));

	RAssert(lobby_init(server));
	RAssert(dylist_push(&servers, server));

	return true;
}

bool disaster_init(void)
{
	if (initialized)
		return true;

	RAssert(enet_initialize() == 0);
time_init();

	
	ThreadVarCreate(g_threadName);
	ThreadVarSet(g_threadName, "Main Thr");

#ifdef SYS_ANDROID
	log_hook(log_android);
#endif

	Info("--------------------------------");
	Info(LOG_RED "Bestest" LOG_BLU "Server " LOG_RST "v" STRINGIFY(BUILD_VERSION));
	Info("Build from " LOG_PUR __DATE__ " " LOG_GRN __TIME__);
	Info("(c) 2024 Team Exe Empire");
	Info("--------------------------------");
	Info("");

	RAssert(config_init());
	RAssert(log_init());
	RAssert(server_global_init());
	RAssert(mod_global_init());

	RAssert(dylist_create(&servers, g_config.server_count));
	for (int32_t i = 0; i < g_config.server_count; i++)
		RAssert(allocate_server((uint16_t)g_config.port, i));

	initialized = true;
	return true;
}

bool disaster_start(void)
{
	if (atomic_load_explicit(&running, memory_order_acquire))
		return false;

	g_workers = malloc(sizeof(Thread) * (size_t)g_config.server_count);
	g_worker_started = calloc((size_t)g_config.server_count, sizeof(bool));
	if (!g_workers || !g_worker_started)
	{
		free(g_workers);
		free(g_worker_started);
		g_workers = NULL;
		g_worker_started = NULL;
		return false;
	}

	atomic_store_explicit(&running, true, memory_order_release);
	Debug("Starting server workers...");

	/* Built in, so a host runs no extra program: off unless Config.json says so. */
	relay_start();
	/* Browser players: WebSocket on the same port numbers over TCP. */
	webgate_start();

	for (int32_t i = 0; i < g_config.server_count; i++)
	{
		Server *server = servers.ptr[i];
		if (!server)
			continue;

		atomic_store_explicit(&server->running, true, memory_order_release);
		ThreadSpawn(g_workers[i], server_worker, server);
		g_worker_started[i] = true;
	}

	return true;
}

int disaster_wait(void)
{
	if (!g_workers || !g_worker_started)
		return 0;

	for (int32_t i = 0; i < g_config.server_count; i++)
	{
		if (g_worker_started[i])
			ThreadJoin(g_workers[i]);
	}

	free(g_worker_started);
	free(g_workers);
	g_worker_started = NULL;
	g_workers = NULL;
	atomic_store_explicit(&running, false, memory_order_release);
	return 0;
}

int disaster_run(void)
{
	if (!disaster_start())
		return 1;
	return disaster_wait();
}

void disaster_shutdown(void)
{
	if (!atomic_load_explicit(&running, memory_order_acquire))
		return;

	for (size_t i = 0; i < servers.capacity; i++)
	{
		Server *server = servers.ptr[i];
		if (server)
			atomic_store_explicit(&server->running, false, memory_order_release);
	}
}

Server* disaster_get(int i)
{
	if (i < 0 || (size_t)i >= servers.capacity)
		return NULL;

	return (Server*)servers.ptr[i];
}

int disaster_count(void)
{
	return (int)servers.capacity;
}

bool disaster_server_lock(Server* server)
{
	RAssert(server);
	MutexLock(server->state_lock);
	return true;
}

bool disaster_server_unlock(Server* server)
{
	RAssert(server);
	MutexUnlock(server->state_lock);
	return true;
}

uint8_t disaster_server_state(Server* server)
{
	RAssert(server);
	return (uint8_t)server->state;
}

bool disaster_server_ban(Server* server, uint16_t id)
{
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* v = (PeerData*)server->peers.ptr[i];
		if (!v)
			continue;

		if (v->id == id)
		{
			server_disconnect(server, v->peer, DR_BANNEDBYHOST, NULL);
			return ban_add(v->nickname.value, v->udid.value, v->ip.value);
		}
	}

	return false;
}

bool disaster_server_op(Server* server, uint16_t id)
{
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* v = (PeerData*)server->peers.ptr[i];
		if (!v)
			continue;

		if (v->id == id)
		{
			v->op = true;

			server_send_msg(server, v->peer, CLRCODE_GRN "you're an operator now");
			return op_add(v->nickname.value, v->ip.value);
		}
	}

	return false;
}

bool disaster_server_timeout(Server* server, uint16_t id, double timeout)
{
	for (size_t i = 0; i < server->peers.capacity; i++)
	{
		PeerData* v = (PeerData*)server->peers.ptr[i];
		if (!v)
			continue;

		if (v->id == id)
		{
			server_disconnect(server, v->peer, DR_KICKEDBYHOST, NULL);
			return timeout_set(v->nickname.value, v->udid.value, v->ip.value, time(NULL) + (uint64_t)(round(timeout)));
		}
	}

	return false;
}

bool disaster_server_peer(Server* server, int index, PeerInfo* info)
{
	info->character = -1;
	if (index < 0 || index >= server->peers.capacity)
		return false;
	else
	{
		PeerData* v = (PeerData*)server->peers.ptr[index];
		if (!v)
			return false;
		else
		{
			info->peer = v;
			info->is_exe = server_is_exe(server, v->id);
			info->ip_addr = v->ip;

			if (v->in_game && server->state >= ST_GAME)
			{
				if (info->is_exe)
					info->character = v->exe_char;
				else
					info->character = v->surv_char;
			}
		}
	}

	return true;
}

bool disaster_server_peer_disconnect(Server* server, uint16_t id, DisconnectReason reason, const char* text)
{
	return server_disconnect_id(server, (int)id, reason, text);
}

int disaster_server_peer_count(Server* server)
{
	return server_total(server);
}

int disaster_server_peer_ingame(Server* server)
{
	return server_ingame(server);
}

int8_t disaster_game_map(Server* server)
{
	return server->game.map;
}

double disaster_game_time(Server* server)
{
	return server->game.time;
}

uint16_t disaster_game_time_sec(Server* server)
{
	return server->game.time_sec;
}