/*
 * np.c: PSN on the host -- a local, signed-in account in place of Sony's network (whose servers a game no longer needs:
 * a community server takes its own game server's place, net.c's redirect).
 *
 *   identity  the online ID: "ntv_" and the player's name -- settings "online_id" (RECOMP_ONLINE_ID wins), at most 10
 *             letters, digits, '-' or '_', set from the launcher's setup or Settings or the F10 menu -- or, with none
 *             chosen, "ntv_" and a tag of this install's own (from its account ID). PSN's 16 in all, the last two kept for
 *             a suffix that tells two players with one name apart (notes/shipping-shape.md, "Online names": a game can't
 *             tell them apart, online_duo.sh NAME_A=NAME_B). A Nativ player is told from a Vita's at a
 *             glance (the tag first: a scoreboard that cuts names short cuts the end), and nobody's computer user name
 *             goes online unasked. A 64-bit account ID, random, made once and kept (settings "np_account_id"), is what a
 *             server tells players apart by; the console id behind a server's machine id is apputil.c's open_psid.
 *   service   sceNpGetServiceState: ONLINE (3); a registered state callback hears ONLINE at the next sceNpCheckCallback.
 *   manager   sceNpManagerGetNpId/GetOnlineId (the identity), GetAccountRegion (us, English), GetContentRatingFlag (not
 *             restricted; age: settings "np_age", default 30 -- Killzone refused online play to anyone under 17),
 *             GetChatRestrictionFlag (not restricted).
 *   auth      sceNpAuthCreateStartRequest makes a ticket at once and hands it to the request's callback at the next
 *             sceNpCheckCallback (id, its size, the argument); sceNpAuthGetTicket copies it. The ticket has a PSN ticket's
 *             layout in the version the request asks for -- 2.1 (PS3's): a body blob of typed fields (serial, issuer,
 *             issued/expiry time, account ID, online ID, region, domain, service ID, status, two empty ones) and a footer
 *             blob with a cipher ID and signature; 3.0 (a Vita's: Killzone asks for it), as a real Vita's ticket has it
 *             (its structure read on a real Vita, 2026-10-22): the same up to the service ID, then a date-of-birth
 *             blob 0x3011 (4 raw bytes: year, month, day), the status (its top byte the account's age), an entitlements
 *             blob 0x3010 (a Vita's held one record: a 7-character id, a time in ms, 8 more bytes; ours is empty: no
 *             Sony entitlements) and one empty field. (The open parsers -- LittleBigRefresh's NPTicket, HugeSpaceship's
 *             -- call 0x3010 "age" and skip it.) Mirage's developer: entitlements empty; its login accepts this (2026-10-22:
 *             Killzone 1.12 logged in and reached its Multiplayer menu). RECOMP_NP_TICKET_V2=1 sends 2.1 whatever is asked.
 *             Unsigned: only a server that accepts that (a community one: Mirage checks none) takes it. No entitlements.
 *   basic     friends and block lists empty; presence accepted; in-game data messages go to the other Nativ copy whose
 *             player they are for (online/basic_msg.c) and arrive through the game's handler at sceNpBasicCheckCallback.
 *   lookup    a user profile lookup finishes at once with the ID it was asked about.
 * Matching2 (rooms) and RUDP: Sony's own modules where a pack loads them (ONLINE.md), stubbed otherwise (main.c's
 * not-initialised errors).
 *
 * RECOMP_NP_LOG=1: each call that changes or reports state. RECOMP_NP_ACCOUNT_ID=<hex> (with RECOMP_ONLINE_ID): another
 * player on this machine, for two copies playing each other.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "hle.h"
#include "net.h"
#include "online/m2.h"
#include "online/overlay.h"
#include "settings.h"

static int np_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_NP_LOG") != 0; return v; }

/* ---- the account */
uint64_t np_account_id(void);
void np_online_id(char out[17]) {
	char b[64]; const char *e = getenv("RECOMP_ONLINE_ID");
	if (e) snprintf(b, sizeof b, "%s", e); else settings_str("online_id", "", b, sizeof b);
	const char *p = b;   /* (a name given with the tag, or the tags before it, nativ_ and Project Ativ's ativ_: once is enough) */
	static const char *const tags[] = {"ntv_", "nativ_", "ativ_"};
	for (size_t t = 0; t < sizeof tags / sizeof *tags; t++) if (!strncasecmp(p, tags[t], strlen(tags[t]))) { p += strlen(tags[t]); break; }
	char name[11] = {0}; int n = 0;
	for (; *p && n < 10; p++) if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') name[n++] = *p;
	if (!n) {   /* none chosen: six letters and digits of this install's account ID (no 0/O, 1/I lookalikes) */
		static const char al[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		uint64_t v = np_account_id();
		for (n = 0; n < 6; n++, v >>= 5) name[n] = al[v & 31];
	}
	/* a name clash's two digits, this game's own (discovery.c: another player in it had the name first); not with a name
	 * given by RECOMP_ONLINE_ID (a test's, exactly as it says) */
	char sfx[8] = ""; if (!e) settings_str("online_suffix", "", sfx, sizeof sfx);
	if (strlen(sfx) == 2 && isdigit((unsigned char)sfx[0]) && isdigit((unsigned char)sfx[1])) snprintf(out, 17, "ntv_%s%s", name, sfx);
	else snprintf(out, 17, "ntv_%s", name);
}
uint64_t np_account_id(void) {
	const char *e = getenv("RECOMP_NP_ACCOUNT_ID"); if (e && strtoull(e, 0, 16)) return strtoull(e, 0, 16);
	char b[32]; settings_str("np_account_id", "", b, sizeof b);
	uint64_t v = strtoull(b, 0, 16);
	if (!v) {
		FILE *f = fopen("/dev/urandom", "rb"); if (f) { if (fread(&v, 8, 1, f) != 1) v = 0; fclose(f); }
		if (!v) v = ((uint64_t)time(0) << 32) ^ (uint64_t)(uintptr_t)&v;
		v |= 1;
		snprintf(b, sizeof b, "%016llx", (unsigned long long)v); settings_set_str("np_account_id", b); settings_save();
	}
	return v;
}
/* SceNpId: SceNpOnlineId {data[16], 4 bytes}, opt[8], reserved[8]. A console's is built by np_common (0x9000313c) from
 * the account's PSN address, <online id>@a1.us.np.playstation.net: opt = the server shard and the region ("a1", "us"),
 * then the platform ("psp2"), and reserved[0] = 1 marks it valid. Sony's own np_matching2 refuses an id without it
 * (np_common's platform check, 0x80550605: "The matchmaking service failed" in Killzone). */
static void write_np_id(uint32_t at) {
	char id[17]; np_online_id(id);
	memset(G(at), 0, 0x24); memcpy(G(at), id, strlen(id));
	memcpy(G(at + 0x14), "a1uspsp2", 8); WR8(at + 0x1c, 1);
}

/* ---- service state and callbacks */
static pthread_mutex_t np_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t state_cb, state_arg; static int state_pending;
#define MAXAUTH 16
static struct { int used, pending; uint32_t cb, arg; uint8_t ticket[512]; uint32_t size; } auths[MAXAUTH];
static void h_np_init(Cpu *c) {
	if (np_log()) { char id[17]; np_online_id(id); fprintf(stderr, "    np: init (comm id %.9s), signed in as %s\n", A0 && RD32(A0) ? (const char *)G(RD32(A0)) : "-", id); }
	/* online over the overlay: discovery, the overlay and the DHT start here, as the game starts, not when it first reaches
	 * the matching service -- the minute or more in its menus is when copies on the internet meet (the first internet
	 * test: the DHT started 20 s before the game searched, and the two copies linked after both had made rooms of their
	 * own). The id as the matching service gets it: SceNpCommunicationId {data[9], term, num} -> "NPWR02882_00". */
	if (A0 && RD32(A0) && net_online() && ov_on()) {
		const uint32_t id = RD32(A0); char comm[16]; snprintf(comm, sizeof comm, "%.9s_%02u", (const char *)G(id), (unsigned)RD8(id + 10));
		m2_online_start(comm);
	}
	RET(0);
}
static void h_np_term(Cpu *c) { RET(0); }
static void h_np_state(Cpu *c) { if (A0) WR32(A0, 3); RET(0); }   /* SCE_NP_SERVICE_STATE_ONLINE */
static void h_np_register_state(Cpu *c) { pthread_mutex_lock(&np_lock); state_cb = A0; state_arg = A1; state_pending = A0 != 0; pthread_mutex_unlock(&np_lock); RET(0); }
static void h_np_unregister_state(Cpu *c) { pthread_mutex_lock(&np_lock); state_cb = 0; state_pending = 0; pthread_mutex_unlock(&np_lock); RET(0); }
static void h_np_check_callback(Cpu *c) {
	pthread_mutex_lock(&np_lock);
	uint32_t cb = state_pending ? state_cb : 0, arg = state_arg; state_pending = 0;
	pthread_mutex_unlock(&np_lock);
	if (cb) { if (np_log()) fprintf(stderr, "    np: service state ONLINE -> callback %#x\n", cb); guest_call4(c, cb, 3, arg, 0, 0); }
	for (int i = 0; i < MAXAUTH; i++) {
		pthread_mutex_lock(&np_lock);
		int go = auths[i].used && auths[i].pending; uint32_t acb = auths[i].cb, aarg = auths[i].arg, sz = auths[i].size; auths[i].pending = 0;
		pthread_mutex_unlock(&np_lock);
		if (go && acb) { if (np_log()) fprintf(stderr, "    np: auth request %d: ticket of %u bytes -> callback %#x\n", i + 1, sz, acb); guest_call4(c, acb, (uint32_t)(i + 1), sz, aarg, 0); }
	}
	RET(0);
}

/* ---- manager */
static void h_np_get_np_id(Cpu *c) { if (A0) write_np_id(A0); RET(0); }
static void h_np_get_online_id(Cpu *c) { if (A0) { char id[17]; np_online_id(id); memset(G(A0), 0, 0x14); memcpy(G(A0), id, strlen(id)); } RET(0); }
static void h_np_region(Cpu *c) { if (A0) { memset(G(A0), 0, 4); memcpy(G(A0), "us", 2); } if (A1) WR32(A1, 1); RET(0); }
static void h_np_rating(Cpu *c) {   /* (int *isRestricted, int *age) */
	int age = settings_int("np_age", 30); if (age < 1) age = 30;
	if (A0) WR32(A0, 0);
	if (A1) WR32(A1, (uint32_t)age);
	RET(0);
}
static void h_np_chat_restriction(Cpu *c) { if (A0) WR32(A0, 0); RET(0); }

/* ---- auth: a local ticket */
static uint32_t put16(uint8_t *p, uint32_t at, uint16_t v) { p[at] = (uint8_t)(v >> 8); p[at + 1] = (uint8_t)v; return at + 2; }
static uint32_t put_field(uint8_t *p, uint32_t at, uint16_t type, const void *data, uint16_t len) {
	at = put16(p, at, type); at = put16(p, at, len); if (len) memcpy(p + at, data, len); return at + len;
}
static uint32_t make_ticket(uint8_t *t, const char *service_id, int major) {
	char id[17]; np_online_id(id);
	uint8_t body[256]; uint32_t b = 0;
	uint8_t serial[0x14] = {0}; uint64_t acc = np_account_id();
	for (int i = 0; i < 8; i++) serial[i] = (uint8_t)(acc >> (56 - 8 * i));
	uint8_t be32[4], be64[8];
	b = put_field(body, b, 8, serial, sizeof serial);                                          /* serial id */
	be32[0] = 0; be32[1] = 0; be32[2] = 1; be32[3] = 0; b = put_field(body, b, 1, be32, 4);     /* issuer id */
	uint64_t now = (uint64_t)time(0) * 1000;
	for (int i = 0; i < 8; i++) be64[i] = (uint8_t)(now >> (56 - 8 * i)); b = put_field(body, b, 7, be64, 8);   /* issued */
	uint64_t exp = now + 3600000ull;
	for (int i = 0; i < 8; i++) be64[i] = (uint8_t)(exp >> (56 - 8 * i)); b = put_field(body, b, 7, be64, 8);   /* expires */
	for (int i = 0; i < 8; i++) be64[i] = (uint8_t)(acc >> (56 - 8 * i)); b = put_field(body, b, 2, be64, 8);   /* account id */
	char oid[0x20] = {0}; memcpy(oid, id, strlen(id)); b = put_field(body, b, 4, oid, sizeof oid);             /* online id */
	char region[4] = { 'u', 's', 0, 0 }; b = put_field(body, b, 8, region, 4);
	char domain[4] = { 'u', 'n', 0, 0 }; b = put_field(body, b, 4, domain, 4);
	char svc[0x18] = {0}; if (service_id) snprintf(svc, sizeof svc, "%s", service_id); b = put_field(body, b, 8, svc, sizeof svc);
	if (major >= 3) {   /* 3.0: date of birth (settings "np_age" years ago, 1 January), the status, entitlements, an empty field */
		time_t tn = time(0); struct tm tm; localtime_r(&tn, &tm);
		int age = settings_int("np_age", 30); if (age < 1 || age > 120) age = 30;
		int year = tm.tm_year + 1900 - age;
		uint8_t dob[4] = { (uint8_t)(year >> 8), (uint8_t)year, 1, 1 };
		b = put_field(body, b, 0x3011, dob, 4);
		be32[0] = (uint8_t)age; be32[1] = be32[2] = be32[3] = 0; b = put_field(body, b, 1, be32, 4);   /* status: the age on top */
		b = put_field(body, b, 0x3010, 0, 0);                                                    /* entitlements: none */
		b = put_field(body, b, 0, 0, 0);
	} else {
		be32[0] = be32[1] = be32[2] = be32[3] = 0; b = put_field(body, b, 1, be32, 4);         /* status */
		b = put_field(body, b, 0, 0, 0); b = put_field(body, b, 0, 0, 0);
	}
	uint8_t foot[80]; uint32_t f = 0; uint8_t sig[0x38] = {0};
	f = put_field(foot, f, 8, "ATIV", 4); f = put_field(foot, f, 8, sig, sizeof sig);
	uint32_t at = 8;
	at = put16(t, at, 0x3000); at = put16(t, at, (uint16_t)b); memcpy(t + at, body, b); at += b;
	at = put16(t, at, 0x3002); at = put16(t, at, (uint16_t)f); memcpy(t + at, foot, f); at += f;
	if (major >= 3) { t[0] = 0x31; t[1] = 0x00; } else { t[0] = 0x21; t[1] = 0x01; }   /* version 3.0 or 2.1 */
	t[2] = 0; t[3] = 0;
	uint32_t rest = at - 8; t[4] = (uint8_t)(rest >> 24); t[5] = (uint8_t)(rest >> 16); t[6] = (uint8_t)(rest >> 8); t[7] = (uint8_t)rest;
	return at;
}
static void h_np_auth_init(Cpu *c) { RET(0); }
/* (const SceNpAuthRequestParameter *): { size, version, serviceId, cookie, cookieSize, entitlementId, consumedCount, ticketCb, cbArg } */
static void h_np_auth_start(Cpu *c) {
	if (!A0) { RET(0x80550302u); return; }
	const uint32_t p = A0, svc = RD32(p + 8), cb = RD32(p + 28), arg = RD32(p + 32);
	pthread_mutex_lock(&np_lock);
	int i; for (i = 0; i < MAXAUTH && auths[i].used; i++) {}
	if (i == MAXAUTH) { pthread_mutex_unlock(&np_lock); RET(0x80550303u); return; }
	auths[i].used = 1; auths[i].pending = 1; auths[i].cb = cb; auths[i].arg = arg;
	const int v2 = getenv("RECOMP_NP_TICKET_V2") && atoi(getenv("RECOMP_NP_TICKET_V2")) > 0;
	auths[i].size = make_ticket(auths[i].ticket, svc ? (const char *)G(svc) : "", v2 ? 2 : RD16(p + 4));
	pthread_mutex_unlock(&np_lock);
	if (np_log()) fprintf(stderr, "    np: auth request %d for service %s (param size %u, ticket version %u.%u, cookie %u bytes, entitlement %s, callback %#x)\n", i + 1,
		svc ? (const char *)G(svc) : "-", RD32(p), RD16(p + 4), RD16(p + 6), RD32(p + 12) ? RD32(p + 16) : 0, RD32(p + 20) ? (const char *)G(RD32(p + 20)) : "-", cb);
	RET(i + 1);
}
static void h_np_auth_get_ticket(Cpu *c) {   /* (id, buf, len) */
	uint32_t i = A0 - 1;
	if (i >= MAXAUTH || !auths[i].used) { RET(0x80550304u); return; }
	if (!A1 || A2 < auths[i].size) { RET(0x80550305u); return; }
	memcpy(G(A1), auths[i].ticket, auths[i].size);
	RET(0);
}
static void h_np_auth_destroy(Cpu *c) { uint32_t i = A0 - 1; if (i < MAXAUTH) { pthread_mutex_lock(&np_lock); auths[i].used = 0; pthread_mutex_unlock(&np_lock); } RET(0); }
static void h_np_auth_entitlement(Cpu *c) { RET(0x80550307u); }   /* not entitled: no add-ons */
/* NpManager's ticket requests (np_matching2's login: the ticket goes to the matching server in tag 0x11; no service id is
 * passed, the shell picks the ticket): create() -> an id, get(id, buf, u32 *size) -> the ticket (buf NULL: its size
 * only), delete(id), abort(id). The same unsigned 3.0 ticket as the auth requests; a community server reads its NP id. */
static void h_np_ticket_create(Cpu *c) { static uint32_t next = 1; RET(__atomic_fetch_add(&next, 1, __ATOMIC_RELAXED)); }
static void h_np_ticket_get(Cpu *c) {
	uint8_t t[512]; const uint32_t n = make_ticket(t, "", 3);
	if (!A2) { RET(0x80550581u); return; }
	if (A1) { if (RD32(A2) < n) { RET(0x80550582u); return; } memcpy(G(A1), t, n); }
	WR32(A2, n);
	RET(0);
}

/* ---- basic, lookup */
static void h_np_zero(Cpu *c) { RET(0); }
static void h_np_count_zero(Cpu *c) { if (A0) WR32(A0, 0); RET(0); }
static void h_np_entries_none(Cpu *c) { if (A3) WR32(A3, 0); RET(0); }   /* (start, entries, num, *retrieved, opt) */
static uint32_t next_lookup = 1;
static void h_np_lookup_create(Cpu *c) { RET(__atomic_fetch_add(&next_lookup, 1, __ATOMIC_RELAXED)); }
static void h_np_lookup_profile(Cpu *c) {   /* (req, npId, userInfo, aboutMe, languages, countryCode, avatarImage, opt): userInfo starts with the SceNpId */
	if (A2 && A1) memcpy(G(A2), G(A1), 0x24);
	RET(0);
}
static void h_np_lookup_poll(Cpu *c) { if (A1) WR32(A1, 0); RET(0); }   /* finished, result 0 */

/* ---- the NP common library's internals Sony's own np_matching2 calls (KZ_NP_MODULES), read from the 3.74 np_common.suprx */
/* sceNpCommonGetNpEnviroment(char env[16]): the NP environment the console is on; "np" is retail (np_common keeps it in a
 * global the system sets at start: "rc" in the module's data before that). Server names are built from it */
static void h_np_environment(Cpu *c) { if (!A0) { RET(0x80550601u); return; } memset(G(A0), 0, 16); memcpy(G(A0), "np", 2); RET(0); }
/* SceNpCommon_E426FEE8(const SceNpCommunicationId *, char *out, size): the id as text, "NPWR02882_00" (9 characters, '_',
 * the two-digit number): np_matching2 names its server list after it */
static void h_np_comm_id_string(Cpu *c) {
	if (!A0 || !A1) { RET(0x80550601u); return; }
	if (A2 < 13) { RET(0x80550602u); return; }
	const uint8_t num = RD8(A0 + 10);
	if (num > 99) { RET(0x8055060au); return; }
	char t[16]; snprintf(t, sizeof t, "%.9s_%02u", (const char *)G(A0), num); memcpy(G(A1), t, 13);
	RET(0);
}
/* SceNpCommon_256670D6(obj, clear): a parsed service record (np_common 0x9000313c fills it, +0x1c = 1 when valid) gets its
 * platform name at +0x18, "psp2" when it named none */
static void h_np_service_platform(Cpu *c) {
	if (!A0) { RET(0x80550601u); return; }
	if (A1) memset(G(A0 + 0x18), 0, 4);
	if (RD8(A0 + 0x1c) != 1) { RET(0x80550605u); return; }
	if (!RD8(A0 + 0x18)) memcpy(G(A0 + 0x18), "psp2", 4);
	RET(0);
}

/* ---- basic: in-game data messages. The handlers the game registers (SceNpBasicEventHandlers, as Killzone's read: the
 * SDK version, then friend list, friend online status, block list, friend game presence, in-game data message, friend
 * context; its argument the third parameter); a message received runs the in-game data message handler at the next
 * sceNpBasicCheckCallback with (const SceNpId *from, const SceNpBasicInGameDataMessage *{data[512], u32 size}, the
 * argument). Each gets its own slot of a ring, in case a game keeps the address. */
enum { BASIC_SLOTS = 32, BASIC_SLOT = 0x240 };   /* (the NpId 0x24, the message 0x204) */
static uint32_t basic_handler, basic_arg, basic_ring; static int basic_next;
static void h_np_basic_register(Cpu *c) {
	pthread_mutex_lock(&np_lock); basic_handler = A0 ? RD32(A0 + 0x14) : 0; basic_arg = A2; pthread_mutex_unlock(&np_lock);
	if (np_log()) fprintf(stderr, "    np: basic handlers registered (in-game data messages -> %#x)\n", basic_handler);
	RET(0);
}
static void h_np_basic_unregister(Cpu *c) { pthread_mutex_lock(&np_lock); basic_handler = 0; pthread_mutex_unlock(&np_lock); RET(0); }
static void h_np_basic_send(Cpu *c) {   /* (const SceNpId *to, const SceNpBasicInGameDataMessage *) */
	if (!A0 || !A1) { RET(0); return; }
	uint8_t from[0x24]; char me[17]; np_online_id(me);
	memset(from, 0, sizeof from); memcpy(from, me, strlen(me)); memcpy(from + 0x14, "a1uspsp2", 8); from[0x1c] = 1;   /* (write_np_id's) */
	char to[17]; memcpy(to, G(A0), 16); to[16] = 0;
	uint32_t n = RD32(A1 + 0x200); if (n > 0x200) n = 0x200;
	const int r = bmsg_send(to, from, G(A1), n);
	if (np_log() || r) fprintf(stderr, "    np: in-game data message of %u bytes to %s%s\n", n, to, r ? ": no such player heard of" : "");
	RET(0);
}
static void h_np_basic_check_callback(Cpu *c) {
	uint8_t from[0x24], data[0x200]; int n;
	while ((n = bmsg_poll(from, data, sizeof data)) >= 0) {
		pthread_mutex_lock(&np_lock); const uint32_t h = basic_handler, arg = basic_arg; pthread_mutex_unlock(&np_lock);
		if (!h) continue;
		if (!basic_ring) basic_ring = guest_alloc(BASIC_SLOTS * BASIC_SLOT, 16);
		const uint32_t at = basic_ring + (uint32_t)(basic_next++ % BASIC_SLOTS) * BASIC_SLOT, msg = at + 0x24;
		memcpy(G(at), from, 0x24); memset(G(msg), 0, 0x204); memcpy(G(msg), data, (size_t)n); WR32(msg + 0x200, (uint32_t)n);
		if (np_log()) fprintf(stderr, "    np: in-game data message of %d bytes from %.16s -> handler %#x\n", n, (const char *)from, h);
		guest_call4(c, h, at, msg, arg, 0);
	}
	RET(0);
}

static void h_np_one(Cpu *c) { RET(1); }
const HleEntry hle_np[] = {
	{"vshSblAimgrIsCEX", h_np_one},
	{"SceNpManager_8D7981D4", h_np_ticket_create}, {"SceNpManager_7CE94530", h_np_ticket_get}, {"SceNpManager_80036A44", h_np_zero}, {"SceNpManager_29C9B8A5", h_np_zero},   /* np_common: a retail console (its debug settings then keep their defaults, never the registry) */
	{"sceNpCommonGetNpEnviroment", h_np_environment}, {"SceNpCommon_E426FEE8", h_np_comm_id_string}, {"SceNpCommon_256670D6", h_np_service_platform},
	{"sceNpInit", h_np_init}, {"sceNpTerm", h_np_term}, {"sceNpGetServiceState", h_np_state},
	{"sceNpRegisterServiceStateCallback", h_np_register_state}, {"sceNpUnregisterServiceStateCallback", h_np_unregister_state},
	{"sceNpCheckCallback", h_np_check_callback},
	{"sceNpManagerGetNpId", h_np_get_np_id}, {"sceNpManagerGetOnlineId", h_np_get_online_id}, {"sceNpManagerGetAccountRegion", h_np_region},
	{"sceNpManagerGetContentRatingFlag", h_np_rating}, {"sceNpManagerGetChatRestrictionFlag", h_np_chat_restriction},
	{"sceNpAuthInit", h_np_auth_init}, {"sceNpAuthTerm", h_np_zero}, {"sceNpAuthCreateStartRequest", h_np_auth_start},
	{"sceNpAuthGetTicket", h_np_auth_get_ticket}, {"sceNpAuthDestroyRequest", h_np_auth_destroy}, {"sceNpAuthAbortRequest", h_np_zero},
	{"sceNpAuthGetEntitlementById", h_np_auth_entitlement},
	{"sceNpBasicInit", h_np_zero}, {"sceNpBasicTerm", h_np_zero}, {"sceNpBasicRegisterHandler", h_np_basic_register}, {"sceNpBasicUnregisterHandler", h_np_basic_unregister},
	{"sceNpBasicRegisterJoinGameAckHandler", h_np_zero}, {"sceNpBasicUnregisterJoinGameAckHandler", h_np_zero}, {"sceNpBasicCheckCallback", h_np_basic_check_callback},
	{"sceNpBasicGetFriendListEntryCount", h_np_count_zero}, {"sceNpBasicGetBlockListEntryCount", h_np_count_zero},
	{"sceNpBasicGetFriendListEntries", h_np_entries_none}, {"sceNpBasicGetBlockListEntries", h_np_entries_none},
	{"sceNpBasicSetInGamePresence", h_np_zero}, {"sceNpBasicSendInGameDataMessage", h_np_basic_send}, {"sceNpBasicJoinGameAckResponseSend", h_np_zero},
	{"sceNpLookupInit", h_np_zero}, {"sceNpLookupTerm", h_np_zero}, {"sceNpLookupCreateTitleCtx", h_np_lookup_create}, {"sceNpLookupDeleteTitleCtx", h_np_zero},
	{"sceNpLookupCreateRequest", h_np_lookup_create}, {"sceNpLookupDeleteRequest", h_np_zero}, {"sceNpLookupAbortRequest", h_np_zero},
	{"sceNpLookupUserProfileAsync", h_np_lookup_profile}, {"sceNpLookupPollAsync", h_np_lookup_poll},
	{0, 0},
};
