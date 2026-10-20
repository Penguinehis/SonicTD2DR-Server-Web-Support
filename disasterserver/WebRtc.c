#include <WebRtc.h>
#include <Log.h>

#ifndef HAVE_WEBRTC
bool wrtc_available(void) { return false; }
void wrtc_init(int slots) { (void)slots; }
bool wrtc_open(int slot, unsigned gen, const char *offer, uint16_t mux_port, wrtc_sock_t udp, uint16_t game_port)
{
	(void)slot; (void)gen; (void)offer; (void)mux_port; (void)udp; (void)game_port;
	return false;
}
bool wrtc_take_answer(int slot, unsigned gen, char *out, size_t cap) { (void)slot; (void)gen; (void)out; (void)cap; return false; }
bool wrtc_pending(void) { return false; }
bool wrtc_send(int slot, unsigned gen, const uint8_t *data, size_t len) { (void)slot; (void)gen; (void)data; (void)len; return false; }
void wrtc_close(int slot) { (void)slot; }
time_t wrtc_last_seen(int slot, unsigned gen) { (void)slot; (void)gen; return 0; }
#else

#include <rtc/rtc.h>
#include <stdlib.h>
#include <string.h>
#include <io/Threads.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#define WRTC_ANSWER_CAP 4096
#define WRTC_MAX_BUFFERED (32 * 1024)   /* a channel this backed up drops datagrams (UDP-like) */

typedef struct
{
	bool        used;
	unsigned    gen;
	int         pc, dc;
	bool        dc_open;
	wrtc_sock_t udp;
	uint16_t    game_port;
	time_t      last_seen;    /* the browser's last datagram on the channel */
	bool        answer_ready;
	char        answer[WRTC_ANSWER_CAP];
} Slot;

static Slot  *g_slots;
static int    g_count;
static Mutex  g_lock;          /* g_slots: the libdatachannel threads and WebGate's */
static int    g_pending;       /* answers being gathered */

/* user pointers carry slot + generation: a callback for a slot that was closed (or reused)
   finds a different generation and does nothing */
static void *key_of(int slot, unsigned gen) { return (void *)(uintptr_t)(((uintptr_t)gen << 12) | (uintptr_t)slot); }
static Slot *slot_of(void *key)
{
	uintptr_t k = (uintptr_t)key;
	int slot = (int)(k & 0xFFF);
	unsigned gen = (unsigned)(k >> 12);
	if (slot < 0 || slot >= g_count || !g_slots[slot].used || g_slots[slot].gen != gen) return NULL;
	return &g_slots[slot];
}

bool wrtc_available(void) { return g_slots != NULL; }

void wrtc_init(int slots)
{
	if (slots <= 0 || slots > 4095) return;
	g_slots = calloc((size_t)slots, sizeof(Slot));
	if (!g_slots) return;
	g_count = slots;
	MutexCreate(g_lock);
	rtcInitLogger(RTC_LOG_WARNING, NULL);
	Info("WebGate: WebRTC data channels on (game port + %d, UDP)", WRTC_PORT_OFFSET);
}

/* ---------------------------------------------------------------- callbacks (libdatachannel threads) */

static void on_gathering(int pc, rtcGatheringState state, void *key)
{
	if (state != RTC_GATHERING_COMPLETE) return;
	MutexLock(g_lock);
	Slot *s = slot_of(key);
	if (s && s->pc == pc && !s->answer_ready)
	{
		/* the answer with every candidate in it (no trickle: one message back) */
		if (rtcGetLocalDescription(pc, s->answer, sizeof(s->answer)) > 0) s->answer_ready = true;
		if (g_pending > 0) g_pending--;
	}
	MutexUnlock(g_lock);
}

static void on_message(int dc, const char *msg, int size, void *key)
{
	if (size < 0 || size > 2048) return;  /* binary only (a negative size is a text message) */
	MutexLock(g_lock);
	Slot *s = slot_of(key);
	if (s && s->dc == dc)
	{
		struct sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		to.sin_port = htons(s->game_port);
		sendto(s->udp, msg, size, 0, (struct sockaddr *)&to, sizeof(to));
		s->last_seen = time(NULL);
	}
	MutexUnlock(g_lock);
}

static void on_open(int dc, void *key)
{
	MutexLock(g_lock);
	Slot *s = slot_of(key);
	if (s && s->dc == dc) s->dc_open = true;
	MutexUnlock(g_lock);
	Debug("WebGate: a data channel opened");
}

static void on_closed(int dc, void *key)
{
	MutexLock(g_lock);
	Slot *s = slot_of(key);
	if (s && s->dc == dc) s->dc_open = false;
	MutexUnlock(g_lock);
}

static void on_datachannel(int pc, int dc, void *key)
{
	MutexLock(g_lock);
	Slot *s = slot_of(key);
	bool ok = s && s->pc == pc && s->dc < 0;
	if (ok) s->dc = dc;
	MutexUnlock(g_lock);
	if (!ok)
	{
		rtcDelete(dc);
		return;
	}
	rtcSetUserPointer(dc, key);
	rtcSetMessageCallback(dc, on_message);
	rtcSetOpenCallback(dc, on_open);
	rtcSetClosedCallback(dc, on_closed);
}

/* ---------------------------------------------------------------- WebGate's thread */

bool wrtc_open(int slot, unsigned gen, const char *offer, uint16_t mux_port, wrtc_sock_t udp, uint16_t game_port)
{
	if (!g_slots || slot < 0 || slot >= g_count || !offer || strncmp(offer, "v=0", 3)) return false;
	wrtc_close(slot);  /* one connection a slot: a second offer replaces the first */

	static const char *stun[] = { "stun:stun.l.google.com:19302" };  /* the public address behind NAT */
	rtcConfiguration cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.iceServers = stun;
	cfg.iceServersCount = 1;
	cfg.enableIceUdpMux = true;          /* every channel of the lobby on one UDP port */
	cfg.portRangeBegin = mux_port;
	cfg.portRangeEnd = mux_port;
	cfg.disableAutoNegotiation = false;
	int pc = rtcCreatePeerConnection(&cfg);
	if (pc < 0) return false;

	MutexLock(g_lock);
	Slot *s = &g_slots[slot];
	memset(s, 0, sizeof(*s));
	s->used = true;
	s->gen = gen;
	s->pc = pc;
	s->dc = -1;
	s->udp = udp;
	s->game_port = game_port;
	g_pending++;
	MutexUnlock(g_lock);

	void *key = key_of(slot, gen);
	rtcSetUserPointer(pc, key);
	rtcSetGatheringStateChangeCallback(pc, on_gathering);
	rtcSetDataChannelCallback(pc, on_datachannel);
	if (rtcSetRemoteDescription(pc, offer, "offer") < 0)
	{
		wrtc_close(slot);
		return false;
	}
	return true;
}

bool wrtc_take_answer(int slot, unsigned gen, char *out, size_t cap)
{
	bool got = false;
	MutexLock(g_lock);
	Slot *s = slot_of(key_of(slot, gen));
	if (s && s->answer_ready)
	{
		snprintf(out, cap, "%s", s->answer);
		s->answer_ready = false;
		s->answer[0] = 0;
		got = true;
	}
	MutexUnlock(g_lock);
	return got;
}

bool wrtc_pending(void) { return g_pending > 0; }

time_t wrtc_last_seen(int slot, unsigned gen)
{
	if (!g_slots) return 0;
	MutexLock(g_lock);
	Slot *s = slot_of(key_of(slot, gen));
	time_t t = s ? s->last_seen : 0;
	MutexUnlock(g_lock);
	return t;
}

bool wrtc_send(int slot, unsigned gen, const uint8_t *data, size_t len)
{
	if (!g_slots) return false;
	bool sent = false;
	MutexLock(g_lock);
	Slot *s = slot_of(key_of(slot, gen));
	if (s && s->dc_open)
	{
		sent = true;  /* the channel is the way now: a backed-up channel drops, like UDP */
		int buffered = rtcGetBufferedAmount(s->dc);
		if (buffered >= 0 && buffered < WRTC_MAX_BUFFERED) rtcSendMessage(s->dc, (const char *)data, (int)len);
	}
	MutexUnlock(g_lock);
	return sent;
}

void wrtc_close(int slot)
{
	if (!g_slots || slot < 0 || slot >= g_count) return;
	int pc = -1, dc = -1;
	MutexLock(g_lock);
	Slot *s = &g_slots[slot];
	if (s->used)
	{
		pc = s->pc;
		dc = s->dc;
		if (!s->answer_ready && g_pending > 0 && !s->dc_open) g_pending--;
		memset(s, 0, sizeof(*s));
	}
	MutexUnlock(g_lock);
	/* deleted outside the lock: libdatachannel may be inside a callback that waits for it */
	if (dc >= 0) rtcDelete(dc);
	if (pc >= 0) rtcDeletePeerConnection(pc);
}
#endif
