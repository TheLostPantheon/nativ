/* Rooms (ONLINE.md). Each copy's service is two things at once:
 *  - its own game's agent: the world list, the search (this copy's rooms and the ones discovery.c heard of), where a room
 *    is (0x1209: this copy's room server for a room it creates, the owner's for one it joins), the room registered;
 *  - the room server of the rooms its own game creates: other players' games connect to it (0x3201), and it keeps the
 *    members and tells each of them what the others do (class 4 events on their room connections).
 * The protocol is np_matching2's (3.65); every layout below is the module's own reader's (scratchpad spec_rooms.md, from
 * the Ghidra export: RoomDataExternal FUN_8100fd72, RoomDataInternal FUN_810130bc, member FUN_8101320c, the events'
 * parsers at 0x810234b8). */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
#include "m2_internal.h"
#include "aes128.h"

void plat_random(void *buf, size_t n);

enum { F_CLOSED = 0x40000000u, F_FULL = 0x20000000u, F_HIDDEN = 0x10000000u, M_OWNER = 0x80000000u };
enum { CAUSE_LEAVE = 1, CAUSE_KICK = 2, CAUSE_GRANT = 3, CAUSE_GONE = 5 };   /* tag 0x62 (the SDK's values) */
#define MAXROOM 8
#define MAXSLOT 64
typedef struct {
	int used, sock; uint16_t id; uint8_t npid[24], key[16]; uint32_t flags; uint8_t team, nat; uint64_t joined;
	uint8_t bin59[80]; uint16_t bin59_len; uint8_t mapped[8]; uint8_t mapped_len;
	int told;   /* (a room taken over: this member has heard the old owner's copy went) */
} Member;
typedef struct {
	int used, registered; uint16_t world, lobby, room; uint32_t flags; uint16_t max; uint64_t pwmask;
	uint8_t key[16], handle[2], sigopt[64]; uint8_t sigopt_len; uint8_t have_ints; uint32_t ints[8]; uint8_t addr_len, addr[16];
	uint8_t binint[2][288]; uint16_t binint_len[2];   /* RoomBinAttrInternal (tags 0x57, 0x58), as the game set them: empty until then */
	Member m[MAXSLOT];   /* by slot - 1; memberId = slot << 4 | 1 */
	uint8_t authority;   /* the slot of the member whose copy serves the room (the creator's, until a takeover) */
	Member gone; uint64_t promoted;   /* a room taken over: the member whose copy went, and when (ms) */
	uint8_t gen;   /* takeovers so far: of two copies serving one room, the higher generation keeps it */
} Room;
static Room rooms[MAXROOM];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void replicate(Room *r);   /* (below: a room outliving its owner's copy) */
static void proxied_drop(int s);
void np_online_id(char out[17]);
void plat_sleep_ns(uint64_t ns);   /* (../platform.h) */

static uint8_t *w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return p + 2; }
static uint8_t *w32(uint8_t *p, uint32_t v) { p = w16(p, (uint16_t)(v >> 16)); return w16(p, (uint16_t)v); }
static uint8_t *w64(uint8_t *p, uint64_t v) { p = w32(p, (uint32_t)(v >> 32)); return w32(p, (uint32_t)v); }
static uint8_t *tlv(uint8_t *p, uint16_t tagword, const void *d, size_t n) { p = w16(p, tagword); p = w16(p, (uint16_t)n); if (n) memcpy(p, d, n); return p + n; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) << 16 | r16(p + 2); }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) << 32 | r32(p + 4); }

/* ---- the shapes the module reads */
static void slot_map(const Room *r, uint8_t out[16]) {   /* 2 bits a slot, slot 1 in the top bits of byte 0: memberId & 3 */
	memset(out, 0, 16);
	for (int s = 1; s <= MAXSLOT; s++) if (r->m[s - 1].used) { const int end = s * 2; out[(end - 1) >> 3] |= (uint8_t)((r->m[s - 1].id & 3) << (8 - ((end - 1) % 8 + 1))); }
}
static int members(const Room *r) { int n = 0; for (int s = 0; s < MAXSLOT; s++) n += r->m[s].used; return n; }
static void update_full(Room *r) { if (members(r) >= r->max) r->flags |= F_FULL; else r->flags &= ~F_FULL; }
static const uint8_t no_groups[32];
static const uint8_t empty_bin_internal[14] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0x01, 0, 0 };   /* u16 member, u64 date, TLV(0x01, nothing) */
static const uint8_t empty_member_bin[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0x01, 0, 0 };           /* u64 date, TLV(0x01, nothing) */
static size_t online_name(const uint8_t npid[24], char out[17]) { memcpy(out, npid, 16); out[16] = 0; return strlen(out); }
/* RoomMemberDataInternal (tag 0x4b), full: what the 0x3200 / 0x3201 replies and the 0x320c event carry */
static size_t member_data(const Member *mb, uint8_t *b) {
	uint8_t *p = w16(b, mb->id); p = w64(p, mb->joined); p = w32(p, mb->flags); *p++ = mb->team; *p++ = mb->nat;
	char name[17]; const size_t nl = online_name(mb->npid, name);
	p = tlv(p, 0x000b, mb->npid, 24); p = tlv(p, 0x100c, name, nl); p = tlv(p, 0x100d, 0, 0);
	p = mb->bin59_len ? tlv(p, 0x0059, mb->bin59, mb->bin59_len) : tlv(p, 0x0059, empty_member_bin, sizeof empty_member_bin);
	p = tlv(p, 0x206e, mb->mapped, mb->mapped_len);
	return (size_t)(p - b);
}
/* the room's internal binary attributes from a create (0x3200) or SetRoomDataInternal (0x3204): tags 0x57 and 0x58, each
 * {u16 member, u64 date, the attribute's TLV}, kept whole. Unit 13 puts its co-op protocol's hash there and a joining game
 * that finds it empty leaves: "A protocol mismatch occurred" (2026-10-26). 1 when one was set */
static int bin_internal(Room *r, const uint8_t *msg, size_t n) {
	int set = 0; M2Field f;
	for (int k = 0; k < 2; k++)
		if (m2_get(msg, n, (uint16_t)(0x57 + k), &f) && f.len <= sizeof r->binint[k]) { memcpy(r->binint[k], f.data, f.len); r->binint_len[k] = f.len; set = 1; }
	return set;
}
/* RoomDataInternal (tag 0x71), full */
static size_t room_internal(const Room *r, uint8_t *b) {
	uint8_t slots[16]; slot_map(r, slots);
	uint8_t *p = b; memcpy(p, r->handle, 2); p += 2; p = w32(p, r->flags); p = w16(p, r->max); memcpy(p, slots, 16); p += 16; p = w64(p, r->pwmask);
	p = tlv(p, 0x7041, no_groups, 32);
	for (int k = 0; k < 2; k++) p = r->binint_len[k] ? tlv(p, (uint16_t)(0x0057 + k), r->binint[k], r->binint_len[k]) : tlv(p, (uint16_t)(0x0057 + k), empty_bin_internal, 14);
	p = tlv(p, 0x206d, r->sigopt, r->sigopt_len);
	const uint8_t groups[4] = { 0x00, 0x47, 0, 0 }; p = tlv(p, 0x0002, groups, 4);   /* (a list of 0x47 groups: none) */
	return (size_t)(p - b);
}
/* RoomDataExternal (tag 0x45): a search result, from an advert's fields */
static size_t room_external(const RoomAd *a, uint8_t *b) {
	uint8_t *p = w16(b, a->world); p = w16(p, a->lobby); p = w16(p, a->room); p = w32(p, a->flags); p = w16(p, a->max);
	memcpy(p, a->slots, 16); p += 16; p = w64(p, a->pwmask);
	p = tlv(p, 0x7041, no_groups, 32);
	for (int i = 0; i < 8; i++) if (a->have_ints >> i & 1) { uint8_t v[4]; w32(v, a->ints[i]); p = tlv(p, (uint16_t)(0x5000 | (0x4c + i)), v, 4); }
	char name[17]; const size_t nl = online_name(a->owner, name);
	p = tlv(p, 0x000b, a->owner, 24); p = tlv(p, 0x100c, name, nl); p = tlv(p, 0x100d, 0, 0);
	if (a->addr_len) p = tlv(p, 0x206c, a->addr, a->addr_len);
	return (size_t)(p - b);
}
static void ad_of(const Room *r, RoomAd *a) {
	memset(a, 0, sizeof *a);
	a->world = r->world; a->lobby = r->lobby; a->room = r->room; a->flags = r->flags; a->max = r->max; slot_map(r, a->slots); a->pwmask = r->pwmask;
	a->have_ints = r->have_ints; memcpy(a->ints, r->ints, sizeof a->ints); a->addr_len = r->addr_len; memcpy(a->addr, r->addr, sizeof a->addr); a->gen = r->gen;
	for (int s = 0; s < MAXSLOT; s++) if (r->m[s].used && (r->m[s].flags & M_OWNER)) memcpy(a->owner, r->m[s].npid, 24);
}
int m2_rooms_snapshot(RoomAd *out, int max) {
	pthread_mutex_lock(&lock);
	int n = 0; for (int i = 0; i < MAXROOM && n < max; i++) if (rooms[i].used && rooms[i].registered && members(&rooms[i])) ad_of(&rooms[i], &out[n++]);
	pthread_mutex_unlock(&lock);
	return n;
}
/* the token (0x1209 reply tag 0x20) and the room's reference (tag 0x3a): which room, for this copy's room server and the
 * owner's ("NVT1", world, room) */
static void room_ref(uint16_t world, uint16_t room, uint8_t out[8]) { memcpy(out, "NVT1", 4); w16(out + 4, world); w16(out + 6, room); }
static int ref_parse(const M2Field *f, uint16_t *world, uint16_t *room) {
	if (!f || f->len != 8 || memcmp(f->data, "NVT1", 4)) return -1;
	*world = r16(f->data + 4); *room = r16(f->data + 6); return 0;
}
static Room *own_room(uint16_t world, uint16_t room) { for (int i = 0; i < MAXROOM; i++) if (rooms[i].used && rooms[i].world == world && rooms[i].room == room) return &rooms[i]; return 0; }
static void address_list(M2Out *o, const char *host, int port) {   /* tag 0x02: one room server */
	uint8_t a[160], *p = w16(a, (uint16_t)port); p = tlv(p, 0x1009, host, strlen(host));
	uint8_t l[180], *q = w16(l, 0x000a); q = w16(q, 1); q = tlv(q, 0x000a, a, (size_t)(p - a));
	m2_put(o, 0x0002, l, (size_t)(q - l));
}
static void error_reply(int s, const M2Head *h, uint8_t err) { uint8_t b[64]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, err, h->id); m2_reply(s, &o); }
static void empty_reply(int s, const M2Head *h) { error_reply(s, h, 0); }

/* ---- the agent's side */
static void world_list(int s, const M2Head *h) {   /* 0x1008: three worlds (Killzone keeps worlds 0..2 as its match types) */
	uint8_t l[256], *p = w16(l, 0x002a); p = w16(p, 3);
	for (uint16_t w = 1; w <= 3; w++) { uint8_t e[18] = { 0 }; w16(e, w); p = tlv(p, 0x002a, e, sizeof e); }
	uint8_t b[512]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id); m2_put(&o, 0x0002, l, (size_t)(p - l)); m2_reply(s, &o);
}
typedef struct { uint8_t op; uint8_t attr; uint32_t v; } IntFilter;
static int int_match(const RoomAd *a, const IntFilter *f) {
	const int i = f->attr - 0x4c; if (i < 0 || i >= 8 || !(a->have_ints >> i & 1)) return 0;
	const uint32_t x = a->ints[i];
	switch (f->op) { case 1: return x == f->v; case 2: return x != f->v; case 3: return x < f->v; case 4: return x <= f->v; case 5: return x > f->v; case 6: return x >= f->v; }
	return 0;
}
static int all_rooms(RoomAd *ads, int max) {   /* this copy's rooms, then the ones heard of */
	int n = m2_rooms_snapshot(ads, max);
	RemoteRoom rr[32]; const int k = disc_rooms(rr, 32);
	for (int i = 0; i < k && n < max; i++) ads[n++] = rr[i].ad;
	return n;
}
static void search(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x1200 / 0x120c */
	uint16_t world = 0, start = 1, max = 20; uint32_t ffilter = 0, fattr = 0; IntFilter fl[8]; int nf = 0;
	const uint8_t *body = msg + M2_PREFIX + M2_HEAD; size_t at = 0; M2Field f;
	while (m2_next_field(body, n - M2_PREFIX - M2_HEAD, &at, &f) == 1) {
		if (f.tag == 0x29 && f.len == 2) world = r16(f.data);
		else if (f.tag == 0x04 && f.len == 4) { start = r16(f.data); max = r16(f.data + 2); }
		else if (f.tag == 0x06 && f.len == 12) { ffilter = r32(f.data); fattr = r32(f.data + 8); }
		else if (f.tag == 0x07 && f.len == 9 && nf < 8) { fl[nf].op = f.data[0]; fl[nf].attr = (uint8_t)(r16(f.data + 1) & 0xfff); fl[nf].v = r32(f.data + 5); nf++; }
	}
	RoomAd ads[40]; const int k = all_rooms(ads, 40);
	RoomAd hit[40]; int total = 0;
	for (int i = 0; i < k; i++) {
		const RoomAd *a = &ads[i];
		if (a->world != world || (a->flags & F_HIDDEN) || (a->flags & ffilter) != (fattr & ffilter)) continue;
		int ok = 1; for (int j = 0; j < nf && ok; j++) ok = int_match(a, &fl[j]);
		if (ok) hit[total++] = *a;
	}
	if (!start) start = 1;
	const int first = start - 1, count = total > first ? (total - first < max ? total - first : max) : 0;
	static uint8_t l[0x8000]; uint8_t *p = w16(l, 0x0045); p = w16(p, (uint16_t)count); p = w16(p, start); p = w16(p, (uint16_t)total);
	for (int i = 0; i < count; i++) { uint8_t e[512]; const size_t el = room_external(&hit[first + i], e); p = tlv(p, 0x0045, e, el); }
	static uint8_t b[0x8100]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id); m2_put(&o, 0x0003, l, (size_t)(p - l)); m2_reply(s, &o);
	fprintf(stderr, "    m2: search world %u: %d of %d rooms\n", world, count, total);
}
static void external_list(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x1208 */
	uint16_t world = 0, want[16]; int nw = 0;
	const uint8_t *body = msg + M2_PREFIX + M2_HEAD; size_t at = 0; M2Field f;
	while (m2_next_field(body, n - M2_PREFIX - M2_HEAD, &at, &f) == 1) {
		if (f.tag == 0x29 && f.len == 2) world = r16(f.data);
		else if (f.tag == 0x3d && f.len == 2 && nw < 16) want[nw++] = r16(f.data);
	}
	RoomAd ads[40]; const int k = all_rooms(ads, 40);
	static uint8_t l[0x4000]; uint8_t *p = w16(l, 0x0045), *cnt = p; p = w16(p, 0); int found = 0;
	for (int j = 0; j < nw; j++) for (int i = 0; i < k; i++) if (ads[i].world == world && ads[i].room == want[j]) { uint8_t e[512]; const size_t el = room_external(&ads[i], e); p = tlv(p, 0x0045, e, el); found++; break; }
	w16(cnt, (uint16_t)found);
	static uint8_t b[0x4100]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id); m2_put(&o, 0x0002, l, (size_t)(p - l)); m2_reply(s, &o);
}
static void locate(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x1209: where the room server is */
	M2Field f; uint16_t world = 0, room = 0;
	if (m2_get(msg, n, 0x29, &f) && f.len == 2) world = r16(f.data);
	if (m2_get(msg, n, 0x3d, &f) && f.len == 2) room = r16(f.data);
	char host[64] = "127.0.0.1"; int port = m2_room_port();
	pthread_mutex_lock(&lock);
	if (!room) {   /* CreateJoinRoom: a room number of our own (unique among the rooms heard of), the room made at 0x3200 */
		RoomAd ads[40]; pthread_mutex_unlock(&lock); const int k = all_rooms(ads, 40); pthread_mutex_lock(&lock);
		for (int tries = 0; tries < 100 && !room; tries++) {
			uint16_t r; plat_random(&r, 2); if (!r) continue;
			int clash = !!own_room(world, r); for (int i = 0; i < k && !clash; i++) clash = ads[i].world == world && ads[i].room == r;
			if (!clash) room = r;
		}
		int i; for (i = 0; i < MAXROOM && rooms[i].used; i++) {}
		if (i == MAXROOM || !room) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x01); return; }
		memset(&rooms[i], 0, sizeof rooms[i]); rooms[i].used = 1; rooms[i].world = world; rooms[i].room = room; rooms[i].max = 8;
	} else if (!own_room(world, room)) {   /* JoinRoom: someone else's room, as discovery heard of it */
		RemoteRoom rr[32]; pthread_mutex_unlock(&lock); const int k = disc_rooms(rr, 32); pthread_mutex_lock(&lock);
		int i; for (i = 0; i < k && !(rr[i].ad.world == world && rr[i].ad.room == room); i++) {}
		if (i == k) { pthread_mutex_unlock(&lock); fprintf(stderr, "    m2: room %u/%u: not heard of\n", world, room); error_reply(s, h, 0x16); return; }
		char me[17]; np_online_id(me);
		if (disc_name_clash() || !strncasecmp((const char *)rr[i].ad.owner, me, 16)) {   /* (two players of one name break a match: discovery.c) */
			pthread_mutex_unlock(&lock); fprintf(stderr, "    m2: room %u/%u: not joined (a name clash: %s)\n", world, room, me); error_reply(s, h, 0x16); return;
		}
		pthread_mutex_unlock(&lock);
		const int pp = m2_proxy_port(world, room, rr[i].host, rr[i].port);   /* (the game reaches it through this copy: m2_proxy.c) */
		fprintf(stderr, "    m2: room %u/%u is at %s:%d%s\n", world, room, rr[i].host, rr[i].port, pp ? " (through this copy's proxy)" : "");
		if (pp) port = pp; else { snprintf(host, sizeof host, "%s", rr[i].host); port = rr[i].port; }
		pthread_mutex_lock(&lock);
	}
	pthread_mutex_unlock(&lock);
	uint8_t ref[8]; room_ref(world, room, ref);
	uint8_t b[512]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id);
	m2_put16(&o, 0x403d, room); address_list(&o, host, port); m2_put(&o, 0x2020, ref, 8);
	m2_reply(s, &o);
	fprintf(stderr, "    m2: room %u/%u is at %s:%d\n", world, room, host, port);
}
static void register_room(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x1202: the room's searchable attributes */
	M2Field f; uint16_t world, room;
	if (ref_parse(m2_get(msg, n, 0x3a, &f), &world, &room)) { empty_reply(s, h); return; }
	pthread_mutex_lock(&lock);
	Room *r = own_room(world, room);
	if (r) {
		const uint8_t *body = msg + M2_PREFIX + M2_HEAD; size_t at = 0;
		while (m2_next_field(body, n - M2_PREFIX - M2_HEAD, &at, &f) == 1)
			if (f.tag >= 0x4c && f.tag <= 0x53 && f.len == 4) { r->ints[f.tag - 0x4c] = r32(f.data); r->have_ints |= (uint8_t)(1 << (f.tag - 0x4c)); }
			else if (f.tag == 0x6c && f.len <= sizeof r->addr) { memcpy(r->addr, f.data, f.len); r->addr_len = (uint8_t)f.len; }
		r->registered = 1;
	}
	pthread_mutex_unlock(&lock);
	empty_reply(s, h);
	if (r) { fprintf(stderr, "    m2: room %u/%u open\n", world, room); disc_poke(); }
}
/* 0x1207: SetRoomDataExternal, the searchable attributes set (again) after the room is registered: the room by world
 * (0x29) and room (0x3d), the ints as 0x1202 has them. Unit 13 sets three right after creating its co-op room, and
 * sends again until it's answered (2026-10-26) */
static void set_external(int s, const uint8_t *msg, size_t n, const M2Head *h) {
	M2Field f; uint16_t world = 0, room = 0;
	const M2Field *w = m2_get(msg, n, 0x29, &f); if (w && f.len == 2) world = r16(f.data);
	const M2Field *rm = m2_get(msg, n, 0x3d, &f); if (rm && f.len == 2) room = r16(f.data);
	pthread_mutex_lock(&lock);
	Room *r = own_room(world, room);
	if (r) {
		const uint8_t *body = msg + M2_PREFIX + M2_HEAD; size_t at = 0;
		while (m2_next_field(body, n - M2_PREFIX - M2_HEAD, &at, &f) == 1)
			if (f.tag >= 0x4c && f.tag <= 0x53 && f.len == 4) { r->ints[f.tag - 0x4c] = r32(f.data); r->have_ints |= (uint8_t)(1 << (f.tag - 0x4c)); }
	}
	pthread_mutex_unlock(&lock);
	empty_reply(s, h);
	if (r) { fprintf(stderr, "    m2: room %u/%u: searchable attributes set\n", world, room); disc_poke(); }
	else fprintf(stderr, "    m2: room %u/%u: attributes for a room this copy doesn't serve (ignored)\n", world, room);
}
void m2_agent_room_op(int s, const uint8_t *msg, size_t n, const M2Head *h, Session *se) {
	(void)se;
	switch (h->op) {
	case 0x1008: world_list(s, h); return;
	case 0x1200: case 0x120c: search(s, msg, n, h); return;
	case 0x1208: external_list(s, msg, n, h); return;
	case 0x1209: locate(s, msg, n, h); return;
	case 0x1202: register_room(s, msg, n, h); return;
	case 0x1207: set_external(s, msg, n, h); return;
	case 0x1203: case 0x1204: case 0x1205: case 0x120a: case 0x120b: empty_reply(s, h); return;   /* (registrations, relays and where a member is: the room server has it all) */
	}
	fprintf(stderr, "    m2: agent op %#06x: not handled\n", h->op);
}

/* ---- the room server's side */
static void send_event(int s, uint16_t op, uint32_t id, M2Out *body) {   /* class 4; body: fields already put after m2_begin */
	(void)op; (void)id;
	if (s >= 0) m2_reply(s, body);
}
static void to_others(Room *r, int except_slot, uint16_t op, uint32_t id, const uint8_t *fields, size_t fn) {
	for (int sl = 0; sl < MAXSLOT; sl++) {
		if (!r->m[sl].used || sl == except_slot) continue;
		uint8_t b[0x1200]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 4, op, 0, id);
		if (fn && o.n + fn <= o.cap) { memcpy(o.p + o.n, fields, fn); o.n += fn; } else if (fn) o.bad = 1;
		send_event(r->m[sl].sock, op, id, &o);
	}
}
static Room *room_of_sock(int s, int *slot) {
	for (int i = 0; i < MAXROOM; i++) if (rooms[i].used) for (int sl = 0; sl < MAXSLOT; sl++) if (rooms[i].m[sl].used && rooms[i].m[sl].sock == s) { *slot = sl; return &rooms[i]; }
	return 0;
}
static void member_fields(Member *mb, const uint8_t *msg, size_t n) {   /* what 0x3200 / 0x3201 / 0x3206 say about the member */
	M2Field f;
	if (m2_get(msg, n, 0x1c, &f) && f.len == 1) mb->team = f.data[0];
	if (m2_get(msg, n, 0x13, &f) && f.len == 1) mb->nat = f.data[0];
	if (m2_get(msg, n, 0x59, &f) && f.len >= 12 && f.len <= sizeof mb->bin59) { memcpy(mb->bin59, f.data, f.len); mb->bin59_len = f.len; const uint64_t now = m2_now_ms(); w64(mb->bin59, now); }
	if (m2_get(msg, n, 0x6e, &f) && f.len <= 8) { memcpy(mb->mapped, f.data, f.len); mb->mapped_len = (uint8_t)f.len; }
}
static int member_of(Room *r, int slot, int sock, const uint8_t *msg, size_t n, uint32_t flags) {
	M2Field f; Member *mb = &r->m[slot]; memset(mb, 0, sizeof *mb);
	if (!m2_get(msg, n, 0x23, &f) || m2_cred_parse(f.data, f.len, mb->key, mb->npid)) return -1;
	mb->used = 1; mb->sock = sock; mb->id = (uint16_t)((slot + 1) << 4 | 1); mb->flags = flags; mb->joined = m2_now_ms();
	member_fields(mb, msg, n);
	return 0;
}
static void put_key(M2Out *o, const Room *r, const Member *mb) { uint8_t k[16]; aes128_encrypt_block(mb->key, r->key, k); m2_put(o, 0x2024, k, 16); }
static void create(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x3200: the room made, its creator its owner */
	M2Field f; uint16_t world, room;
	pthread_mutex_lock(&lock);
	Room *r = ref_parse(m2_get(msg, n, 0x20, &f), &world, &room) ? 0 : own_room(world, room);
	if (!r || members(r)) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x16); return; }
	if (m2_get(msg, n, 0x40, &f) && f.len == 4) r->flags = r32(f.data);
	if (m2_get(msg, n, 0x3f, &f) && f.len == 2 && r16(f.data) >= 1 && r16(f.data) <= MAXSLOT) r->max = r16(f.data);
	if (m2_get(msg, n, 0x6d, &f) && f.len <= sizeof r->sigopt) { memcpy(r->sigopt, f.data, f.len); r->sigopt_len = (uint8_t)f.len; }
	r->binint_len[0] = r->binint_len[1] = 0; bin_internal(r, msg, n);
	plat_random(r->key, 16); plat_random(r->handle, 2); r->handle[0] |= 1;
	if (member_of(r, 0, s, msg, n, M_OWNER)) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x02); return; }
	update_full(r);
	static uint8_t ri[1024], md[512]; const size_t rl = room_internal(r, ri), ml = member_data(&r->m[0], md);
	uint8_t ref[8]; room_ref(world, room, ref); const uint8_t one = 1;
	uint8_t b[0x1000]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id);
	m2_put(&o, 0x0071, ri, rl); m2_put(&o, 0x004b, md, ml); put_key(&o, r, &r->m[0]); m2_put(&o, 0x2021, &one, 1); m2_put(&o, 0x203a, ref, 8);
	m2_reply(s, &o);
	char name[17]; online_name(r->m[0].npid, name);
	fprintf(stderr, "    m2: room %u/%u created by %s (max %u, flags %#x)\n", world, room, name, r->max, r->flags);
	r->authority = 0;
	pthread_mutex_unlock(&lock);
}
static void join(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x3201: a member more, the others told */
	M2Field f; uint16_t world, room;
	pthread_mutex_lock(&lock);
	Room *r = ref_parse(m2_get(msg, n, 0x20, &f), &world, &room) ? 0 : own_room(world, room);
	if (!r || !members(r)) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x16); return; }
	if ((r->flags & (F_FULL | F_CLOSED)) || members(r) >= r->max) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x18); return; }
	int sl; for (sl = 0; sl < r->max && r->m[sl].used; sl++) {}
	if (member_of(r, sl, s, msg, n, 0)) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x02); return; }
	for (int o = 0; o < MAXSLOT; o++) if (o != sl && r->m[o].used && !strncasecmp((const char *)r->m[o].npid, (const char *)r->m[sl].npid, 16)) {   /* a second player of one name: never (it breaks the match) */
		char name[17]; online_name(r->m[sl].npid, name); fprintf(stderr, "    m2: %s: refused in room %u/%u (a player of that name is in it)\n", name, world, room);
		r->m[sl].used = 0; pthread_mutex_unlock(&lock); error_reply(s, h, 0x16); return;
	}
	update_full(r);
	static uint8_t ri[1024], list[0x2000]; const size_t rl = room_internal(r, ri);
	uint8_t *p = w16(list, 0x004b), *cnt = p; p = w16(p, 0); int k = 0;
	for (int i = 0; i < MAXSLOT; i++) if (r->m[i].used) { uint8_t md[512]; const size_t ml = member_data(&r->m[i], md); p = tlv(p, 0x004b, md, ml); k++; }
	w16(cnt, (uint16_t)k);
	uint8_t ref[8]; room_ref(world, room, ref); const uint8_t one = 1;
	static uint8_t b[0x2400]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id);
	m2_put16(&o, 0x4016, r->m[sl].id); m2_put(&o, 0x0071, ri, rl); m2_put(&o, 0x0002, list, (size_t)(p - list));
	put_key(&o, r, &r->m[sl]); m2_put(&o, 0x2021, &one, 1); m2_put(&o, 0x203a, ref, 8);
	m2_reply(s, &o);
	uint8_t ev[600], *e = ev; { uint8_t md[512]; const size_t ml = member_data(&r->m[sl], md); e = tlv(e, 0x004b, md, ml); }
	to_others(r, sl, 0x320c, 0, ev, (size_t)(e - ev));
	char name[17]; online_name(r->m[sl].npid, name);
	fprintf(stderr, "    m2: %s joined room %u/%u as %#x (%d members)\n", name, world, room, r->m[sl].id, members(r));
	replicate(r);
	pthread_mutex_unlock(&lock);
	disc_poke();
}
static void remove_member(Room *r, int sl, uint8_t cause) {   /* (lock held) the others told; a new owner or the room's end */
	const Member gone = r->m[sl]; r->m[sl].used = 0;
	uint8_t ev[64], *e = ev; e = w16(e, 0x4016); e = w16(e, 2); e = w16(e, gone.id); e = tlv(e, 0x000b, gone.npid, 24); e = tlv(e, 0x3062, &cause, 1);
	if (cause != CAUSE_KICK) to_others(r, -1, 0x320f, 0, ev, (size_t)(e - ev));   /* (a kick told everyone, the kicked too, already) */
	if (!members(r)) { fprintf(stderr, "    m2: room %u/%u closed\n", r->world, r->room); memset(r, 0, sizeof *r); return; }
	if (gone.flags & M_OWNER) {
		int no; for (no = 0; no < MAXSLOT && !r->m[no].used; no++) {}
		r->m[no].flags |= M_OWNER;
		uint8_t ow[64], *q = ow; q = w16(q, 0x4016); q = w16(q, 2); q = w16(q, r->m[no].id); q = tlv(q, 0x000b, r->m[no].npid, 24); q = tlv(q, 0x3062, &cause, 1);
		to_others(r, -1, 0x3212, 0, ow, (size_t)(q - ow));
	}
	update_full(r);
	replicate(r);
}
static void leave(int s, const M2Head *h) {   /* 0x3202 */
	pthread_mutex_lock(&lock);
	int sl; Room *r = room_of_sock(s, &sl);
	if (r) { r->m[sl].sock = -1; empty_reply(s, h); remove_member(r, sl, CAUSE_LEAVE); } else empty_reply(s, h);
	pthread_mutex_unlock(&lock);
	disc_poke();
}
void m2_room_conn_closed(int s) {
	pthread_mutex_lock(&lock);
	proxied_drop(s);
	int sl; Room *r = room_of_sock(s, &sl);
	if (r) { r->m[sl].sock = -1; remove_member(r, sl, CAUSE_GONE); }
	pthread_mutex_unlock(&lock);
	if (r) disc_poke();
}
static void set_room_data(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x3204 */
	pthread_mutex_lock(&lock);
	int sl; Room *r = room_of_sock(s, &sl); M2Field f;
	if (!r) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x16); return; }
	uint8_t ev[800], *e = ev;
	if (m2_get(msg, n, 0x06, &f) && f.len == 12) { const uint32_t filt = r32(f.data), v = r32(f.data + 8); r->flags = (r->flags & ~filt) | (v & filt); e = tlv(e, 0x0006, f.data, 12); }
	if (bin_internal(r, msg, n)) for (int k = 0; k < 2; k++) if (r->binint_len[k] && m2_get(msg, n, (uint16_t)(0x57 + k), &f)) e = tlv(e, (uint16_t)(0x0057 + k), r->binint[k], r->binint_len[k]);
	if (m2_get(msg, n, 0x6d, &f) && f.len <= sizeof r->sigopt) { memcpy(r->sigopt, f.data, f.len); r->sigopt_len = (uint8_t)f.len; e = tlv(e, 0x206d, f.data, f.len); }
	if (m2_get(msg, n, 0x6a, &f) && f.len == 8) { r->pwmask = (uint64_t)r32(f.data) << 32 | r32(f.data + 4); e = tlv(e, 0x706a, f.data, 8); }
	uint8_t b[128]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id); uint8_t t[8]; w64(t, m2_now_ms()); m2_put(&o, 0x6070, t, 8); m2_reply(s, &o);
	if (e > ev) to_others(r, sl, 0x320e, 0, ev, (size_t)(e - ev));   /* (only the owner privilege list changed: nothing the event carries) */
	replicate(r);
	pthread_mutex_unlock(&lock);
}
static void set_member_data(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x3206 */
	pthread_mutex_lock(&lock);
	int sl; Room *r = room_of_sock(s, &sl); M2Field f;
	if (!r) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x16); return; }
	int t = sl; if (m2_get(msg, n, 0x16, &f) && f.len == 2) { const int ts = (r16(f.data) >> 4) - 1; if (ts >= 0 && ts < MAXSLOT && r->m[ts].used) t = ts; }
	Member *mb = &r->m[t]; member_fields(mb, msg, n);
	uint8_t ev[256], *e = ev; e = w16(e, 0x4016); e = w16(e, 2); e = w16(e, mb->id);
	if (m2_get(msg, n, 0x06, &f) && f.len == 12) { const uint32_t filt = r32(f.data), v = r32(f.data + 8); mb->flags = (mb->flags & ~filt) | (v & filt); e = tlv(e, 0x0006, f.data, 12); }
	if (m2_get(msg, n, 0x1c, &f)) e = tlv(e, 0x301c, &mb->team, 1);
	if (m2_get(msg, n, 0x13, &f)) e = tlv(e, 0x3013, &mb->nat, 1);
	if (mb->bin59_len) e = tlv(e, 0x0059, mb->bin59, mb->bin59_len);
	if (m2_get(msg, n, 0x6e, &f)) e = tlv(e, 0x206e, mb->mapped, mb->mapped_len);
	uint8_t b[128]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id); uint8_t ts[8]; w64(ts, m2_now_ms()); m2_put(&o, 0x6070, ts, 8); m2_reply(s, &o);
	/* (to every member, its sender too: np_matching2 updates its own copy of its member data only from this event -- Unit
	 * 13's own staging record kept its creation-time value and its player was never counted, 2026-10-26) */
	to_others(r, -1, 0x320d, 0, ev, (size_t)(e - ev));
	replicate(r);
	pthread_mutex_unlock(&lock);
}
static void owner_or_kick(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x3210 GrantRoomOwner, 0x3211 KickoutRoomMember */
	pthread_mutex_lock(&lock);
	int sl; Room *r = room_of_sock(s, &sl); M2Field f;
	const int t = r && m2_get(msg, n, 0x16, &f) && f.len == 2 ? (r16(f.data) >> 4) - 1 : -1;
	if (!r || t < 0 || t >= MAXSLOT || !r->m[t].used || !(r->m[sl].flags & M_OWNER)) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x16); return; }
	empty_reply(s, h);
	const uint8_t cause = h->op == 0x3210 ? CAUSE_GRANT : CAUSE_KICK;
	uint8_t ev[64], *e = ev; e = w16(e, 0x4016); e = w16(e, 2); e = w16(e, r->m[t].id); e = tlv(e, 0x000b, r->m[t].npid, 24); e = tlv(e, 0x3062, &cause, 1);
	if (h->op == 0x3210) { r->m[sl].flags &= ~M_OWNER; r->m[t].flags |= M_OWNER; to_others(r, -1, 0x3212, 0, ev, (size_t)(e - ev)); replicate(r); }
	else { to_others(r, -1, 0x320f, 0, ev, (size_t)(e - ev)); remove_member(r, t, CAUSE_KICK); }
	pthread_mutex_unlock(&lock);
	disc_poke();
}
static void room_message(int s, const uint8_t *msg, size_t n, const M2Head *h) {   /* 0x320a: the module's signaling (and the game's own messages) */
	pthread_mutex_lock(&lock);
	int sl; Room *r = room_of_sock(s, &sl); M2Field f, pay, opt;
	if (!r || !m2_get(msg, n, 0x1b, &pay)) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x16); return; }
	static uint8_t ev[0x500];   /* (the event below; a sender's lengths must fit it: a game's messages are tens of bytes) */
	const int has_opt = m2_get(msg, n, 0x64, &opt) != 0;
	if (6 + 4 + (size_t)pay.len + (has_opt ? 4 + (size_t)opt.len : 0) > sizeof ev) { pthread_mutex_unlock(&lock); error_reply(s, h, 0x16); return; }
	uint8_t mask[16]; const int targeted = m2_get(msg, n, 0x44, &f) && f.len == 16;
	if (targeted) memcpy(mask, f.data, 16); else { const int me = r->m[sl].used; r->m[sl].used = 0; slot_map(r, mask); r->m[sl].used = me; }
	uint8_t b[128]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id); m2_put(&o, 0x7044, mask, 16); m2_reply(s, &o);
	/* the event: the sender, the payload, and (the module's own signaling: header +0x06 = 1) +0x08's top nibble 1, which
	 * the module needs to hand it to its signaling (spec_rooms 6.1) */
	const uint32_t id = h->b6 == 1 ? 0x10000000u : 0;
	uint8_t *e = ev; e = w16(e, 0x4016); e = w16(e, 2); e = w16(e, r->m[sl].id); e = tlv(e, 0x201b, pay.data, pay.len);
	if (has_opt) e = tlv(e, 0x0064, opt.data, opt.len);
	for (int t = 0; t < MAXSLOT; t++) {
		if (!r->m[t].used || t == sl) continue;
		const int end = (t + 1) * 2; if (!((mask[(end - 1) >> 3] >> (8 - ((end - 1) % 8 + 1))) & 3)) continue;
		uint8_t eb[0x600]; M2Out eo = { eb, 0, sizeof eb, 0 }; m2_begin(&eo, 4, 0x320b, 0, id);
		memcpy(eo.p + eo.n, ev, (size_t)(e - ev)); eo.n += (size_t)(e - ev);
		send_event(r->m[t].sock, 0x320b, id, &eo);
	}
	pthread_mutex_unlock(&lock);
}
/* a room request's MAC: the room key, as the module decrypted it from tag 0x24 with its session key (its own AES, NEON
 * code: a match here says the recompiled AES got the room key right). Logged when it doesn't match. */
static void check_room_mac(int s, const uint8_t *msg, size_t n, const M2Head *h) {
	pthread_mutex_lock(&lock);
	int sl; Room *r = room_of_sock(s, &sl);
	if (r) {
		uint8_t mac[16]; m2_mac(r->key, 16, msg, n, mac);
		static int said_ok;
		if (memcmp(mac, h->mac, 16)) fprintf(stderr, "    m2: room op %#06x: the MAC doesn't match the room key\n", h->op);
		else if (!said_ok++) fprintf(stderr, "    m2: room requests signed with the room key (the module's AES decrypted it right)\n");
	}
	pthread_mutex_unlock(&lock);
}
void m2_room_op(int s, const uint8_t *msg, size_t n, const M2Head *h) {
	if (h->op != 0x2001 && h->op != 0x3200 && h->op != 0x3201) check_room_mac(s, msg, n, h);
	switch (h->op) {
	case 0x2001: { uint8_t b[256]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 2, h->op, 0, h->id); address_list(&o, "127.0.0.1", m2_room_port()); m2_reply(s, &o); return; }
	case 0x3200: create(s, msg, n, h); return;
	case 0x3201: join(s, msg, n, h); return;
	case 0x3202: leave(s, h); return;
	case 0x3204: set_room_data(s, msg, n, h); return;
	case 0x3206: set_member_data(s, msg, n, h); return;
	case 0x3210: case 0x3211: owner_or_kick(s, msg, n, h); return;
	case 0x320a: room_message(s, msg, n, h); return;
	}
	fprintf(stderr, "    m2: room op %#06x: not handled\n", h->op);
	error_reply(s, h, 0x01);
}

/* ---- a room outliving its owner's copy (2026-10-24: every member's copy keeps the room and takes it over
 * when the owner's goes). The room lives in one copy, its authority: the creator's room server. A member's game reaches it
 * through its own copy's proxy (m2_proxy.c), which says so first ("NP": hello) and from then on is sent the room as it
 * stands ("NS": snapshot) at every change and every second, which is also the heartbeat. Our frames share the module's
 * framing ({'N', type, u16 BE total length}): the proxy strips them before the game's stream, and only proxies get them.
 * When the authority's copy goes (its connection ends, or no snapshot for 5 s), every proxy picks the same successor from
 * its copy: the room owner if that wasn't the authority's player, else the member in the lowest slot. The successor's copy
 * makes its copy the room (m2_room_promote) and every proxy reconnects there, saying which member it carries ("NR":
 * resume, proven with the room key): the games never see their room connection drop, and hear what PSN sent when a
 * member's connection went -- the member gone (cause GONE) and, when it was the owner, the new owner. A member whose
 * proxy doesn't come back within 15 s is gone too. */
enum { SNAP_MAX = 0x3000, MAXPX = 64, RESUME_TIMEOUT_MS = 15000 };
static int proxied[MAXPX];   /* (lock) room connections from proxies: they get snapshots */
static int is_proxied(int s) { for (int i = 0; i < MAXPX; i++) if (proxied[i] == s + 1) return 1; return 0; }
static void proxied_add(int s) { if (is_proxied(s)) return; for (int i = 0; i < MAXPX; i++) if (!proxied[i]) { proxied[i] = s + 1; return; } }
static void proxied_drop(int s) { for (int i = 0; i < MAXPX; i++) if (proxied[i] == s + 1) proxied[i] = 0; }
static uint64_t mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static int is_me(const Member *m) { char me[17]; np_online_id(me); return !strncmp((const char *)m->npid, me, 16); }

static size_t snapshot(const Room *r, uint8_t *b) {   /* "NS": the room, every field a copy needs to serve it */
	uint8_t *p = b; *p++ = 'N'; *p++ = 'S'; p += 2;
	p = w16(p, r->world); p = w16(p, r->lobby); p = w16(p, r->room); p = w32(p, r->flags); p = w16(p, r->max); p = w64(p, r->pwmask);
	memcpy(p, r->key, 16); p += 16; memcpy(p, r->handle, 2); p += 2;
	*p++ = r->sigopt_len; memcpy(p, r->sigopt, r->sigopt_len); p += r->sigopt_len;
	*p++ = r->have_ints; for (int i = 0; i < 8; i++) p = w32(p, r->ints[i]);
	*p++ = r->addr_len; memcpy(p, r->addr, 16); p += 16; *p++ = (uint8_t)r->registered; *p++ = r->authority; *p++ = r->gen;
	uint8_t *cnt = p++; int k = 0;
	for (int sl = 0; sl < MAXSLOT; sl++) {
		const Member *m = &r->m[sl]; if (!m->used) continue;
		*p++ = (uint8_t)sl; p = w16(p, m->id); memcpy(p, m->npid, 24); p += 24; memcpy(p, m->key, 16); p += 16;
		p = w32(p, m->flags); *p++ = m->team; *p++ = m->nat; p = w64(p, m->joined);
		p = w16(p, m->bin59_len); memcpy(p, m->bin59, m->bin59_len); p += m->bin59_len; *p++ = m->mapped_len; memcpy(p, m->mapped, 8); p += 8; k++;
	}
	*cnt = (uint8_t)k; w16(b + 2, (uint16_t)(p - b));
	return (size_t)(p - b);
}
static int snapshot_parse(const uint8_t *b, size_t n, Room *r) {
	if (n < 4 || b[0] != 'N' || b[1] != 'S') return -1;
	const uint8_t *p = b + 4, *end = b + n; memset(r, 0, sizeof *r);
#define NEED(k) do { if ((size_t)(end - p) < (size_t)(k)) return -1; } while (0)
	NEED(38); r->world = r16(p); r->lobby = r16(p + 2); r->room = r16(p + 4); r->flags = r32(p + 6); r->max = r16(p + 10); r->pwmask = r64(p + 12);
	memcpy(r->key, p + 20, 16); memcpy(r->handle, p + 36, 2); p += 38;
	NEED(1); r->sigopt_len = *p++; if (r->sigopt_len > sizeof r->sigopt) return -1; NEED(r->sigopt_len); memcpy(r->sigopt, p, r->sigopt_len); p += r->sigopt_len;
	NEED(33); r->have_ints = *p++; for (int i = 0; i < 8; i++, p += 4) r->ints[i] = r32(p);
	NEED(21); r->addr_len = *p++ <= 16 ? p[-1] : 16; memcpy(r->addr, p, 16); p += 16; r->registered = *p++; r->authority = *p++; r->gen = *p++;
	const int k = *p++;
	for (int j = 0; j < k; j++) {
		NEED(59); const int sl = *p; if (sl >= MAXSLOT) return -1;
		Member *m = &r->m[sl]; m->used = 1; m->sock = -1; m->id = r16(p + 1); memcpy(m->npid, p + 3, 24); memcpy(m->key, p + 27, 16);
		m->flags = r32(p + 43); m->team = p[47]; m->nat = p[48]; m->joined = r64(p + 49); m->bin59_len = r16(p + 57); p += 59;
		if (m->bin59_len > sizeof m->bin59) return -1;
		NEED(m->bin59_len + 9); memcpy(m->bin59, p, m->bin59_len); p += m->bin59_len; m->mapped_len = *p++ <= 8 ? p[-1] : 8; memcpy(m->mapped, p, 8); p += 8;
	}
#undef NEED
	return 0;
}
static void replicate(Room *r) {   /* (lock held) the room to every member's proxy */
	static uint8_t b[SNAP_MAX]; size_t n = 0;
	for (int sl = 0; sl < MAXSLOT; sl++) if (r->m[sl].used && r->m[sl].sock >= 0 && is_proxied(r->m[sl].sock)) {
		if (!n) n = snapshot(r, b);
		const uint64_t t0 = mono_ms(); m2_send(r->m[sl].sock, b, n);
		if (m2_log() && mono_ms() - t0 > 300) { char name[17]; online_name(r->m[sl].npid, name); fprintf(stderr, "    m2: room %u/%u: the snapshot to %s's proxy took %llu ms to send\n", r->world, r->room, name, (unsigned long long)(mono_ms() - t0)); }
	}
}

/* the copies this copy keeps of rooms its player is in (another copy serves them) */
static struct { int used; Room r; } replicas[MAXROOM];
static int replica_of(uint16_t world, uint16_t room) { for (int i = 0; i < MAXROOM; i++) if (replicas[i].used && replicas[i].r.world == world && replicas[i].r.room == room) return i; return -1; }
void m2_replica_update(const uint8_t *frame, size_t n) {
	static Room r; pthread_mutex_lock(&lock);
	if (!snapshot_parse(frame, n, &r) && !own_room(r.world, r.room)) {
		int i = replica_of(r.world, r.room); if (i < 0) for (i = 0; i < MAXROOM && replicas[i].used; i++) {}
		if (i < MAXROOM) { if (!replicas[i].used && m2_log()) fprintf(stderr, "    m2: keeping a copy of room %u/%u\n", r.world, r.room); replicas[i].used = 1; replicas[i].r = r; }
	}
	pthread_mutex_unlock(&lock);
}
/* the successors of the room's serving copy after it, in the order every copy tries them: the owner (when the authority's
 * player isn't it), then the others by slot. n of them in out[] (slots); the authority's own slot never. */
static int successors(const Room *r, int out[MAXSLOT]) {
	int n = 0, owner = -1;
	for (int sl = 0; sl < MAXSLOT; sl++) if (r->m[sl].used && (r->m[sl].flags & M_OWNER)) owner = sl;
	if (owner >= 0 && owner != r->authority) out[n++] = owner;
	for (int sl = 0; sl < MAXSLOT; sl++) if (r->m[sl].used && sl != r->authority && sl != owner) out[n++] = sl;
	return n;
}
int m2_room_successor(uint16_t world, uint16_t room, int nth, char host[64], int *port) {
	pthread_mutex_lock(&lock);
	const int i = replica_of(world, room); int r = -1;
	if (i >= 0) {
		int c[MAXSLOT]; const int n = successors(&replicas[i].r, c);
		if (nth < n) {
			const Member *m = &replicas[i].r.m[c[nth]];
			if (is_me(m)) r = 1;
			else { char who[17]; memcpy(who, m->npid, 16); who[16] = 0; uint16_t p; if (!disc_room_server(who, host, &p)) { *port = p; r = 0; } else r = 2; }
		}
	}
	pthread_mutex_unlock(&lock);
	return r;   /* 1: this copy, 0: host:port, 2: that member's copy isn't heard of (try the next), -1: no more */
}
void net_host_ipv4(char *ip, size_t n);   /* (../net.h: the address the game is told is its own) */
int m2_room_promote(uint16_t world, uint16_t room) {
	char ip[32]; unsigned q[4]; net_host_ipv4(ip, sizeof ip);
	const int have_ip = sscanf(ip, "%u.%u.%u.%u", &q[0], &q[1], &q[2], &q[3]) == 4;
	pthread_mutex_lock(&lock);
	if (own_room(world, room)) { pthread_mutex_unlock(&lock); return 0; }   /* (already) */
	const int i = replica_of(world, room); int k; for (k = 0; k < MAXROOM && rooms[k].used; k++) {}
	if (i < 0 || k == MAXROOM) { pthread_mutex_unlock(&lock); return -1; }
	Room *r = &rooms[k]; *r = replicas[i].r; replicas[i].used = 0;
	r->used = 1; r->gone = r->m[r->authority]; r->m[r->authority].used = 0; r->promoted = mono_ms(); r->gen++;
	int me = -1; for (int sl = 0; sl < MAXSLOT; sl++) if (r->m[sl].used) { r->m[sl].sock = -1; r->m[sl].told = 0; if (is_me(&r->m[sl])) me = sl; }
	if (me < 0) { memset(r, 0, sizeof *r); pthread_mutex_unlock(&lock); return -1; }
	r->authority = (uint8_t)me;
	if (r->gone.flags & M_OWNER) r->m[me].flags |= M_OWNER;   /* (the owner went: the copy that serves the room now is its player's) */
	/* the owner's game addresses a joining game contacts first (tag 0x6c, as the old owner's game registered them: its mapped
	 * IPv4, 0, its local IPv4) are this copy's game's now: a late joiner across machines was kicked for "a loss of
	 * connectivity" when they still named the old owner's */
	if (have_ip && r->addr_len == 12) for (int k = 0; k < 2; k++) { uint8_t *a = r->addr + 8 * k; a[0] = (uint8_t)q[0]; a[1] = (uint8_t)q[1]; a[2] = (uint8_t)q[2]; a[3] = (uint8_t)q[3]; }
	update_full(r);
	char name[17]; online_name(r->gone.npid, name);
	fprintf(stderr, "    m2: room %u/%u taken over: %s's copy went, this copy serves it now (%d members)\n", world, room, name, members(r));
	pthread_mutex_unlock(&lock);
	disc_poke();
	return 0;
}
size_t m2_room_resume_frame(uint16_t world, uint16_t room, uint8_t out[64]) {   /* "NR": world, room, member id, NpId, proof */
	size_t n = 0; pthread_mutex_lock(&lock);
	const int i = replica_of(world, room); const Room *r = i >= 0 ? &replicas[i].r : own_room(world, room);
	if (r) for (int sl = 0; sl < MAXSLOT; sl++) if (r->m[sl].used && is_me(&r->m[sl])) {
		uint8_t *p = out; *p++ = 'N'; *p++ = 'R'; p += 2; p = w16(p, world); p = w16(p, room); p = w16(p, r->m[sl].id); memcpy(p, r->m[sl].npid, 24); p += 24;
		aes128_encrypt_block(r->key, r->m[sl].npid, p); p += 16; n = (size_t)(p - out); w16(out + 2, (uint16_t)n); break;
	}
	pthread_mutex_unlock(&lock);
	return n;
}
static void tell_gone(Room *r, int sl) {   /* (lock held) a resumed member hears the authority's player go, and the new owner */
	const Member *g = &r->gone; const uint8_t cause = CAUSE_GONE;
	uint8_t ev[64], *e = ev; e = w16(e, 0x4016); e = w16(e, 2); e = w16(e, g->id); e = tlv(e, 0x000b, g->npid, 24); e = tlv(e, 0x3062, &cause, 1);
	uint8_t b[256]; M2Out o = { b, 0, sizeof b, 0 }; m2_begin(&o, 4, 0x320f, 0, 0); memcpy(o.p + o.n, ev, (size_t)(e - ev)); o.n += (size_t)(e - ev); send_event(r->m[sl].sock, 0x320f, 0, &o);
	if (g->flags & M_OWNER) for (int no = 0; no < MAXSLOT; no++) if (r->m[no].used && (r->m[no].flags & M_OWNER)) {
		uint8_t ow[64], *q = ow; q = w16(q, 0x4016); q = w16(q, 2); q = w16(q, r->m[no].id); q = tlv(q, 0x000b, r->m[no].npid, 24); q = tlv(q, 0x3062, &cause, 1);
		M2Out p = { b, 0, sizeof b, 0 }; m2_begin(&p, 4, 0x3212, 0, 0); memcpy(p.p + p.n, ow, (size_t)(q - ow)); p.n += (size_t)(q - ow); send_event(r->m[sl].sock, 0x3212, 0, &p);
	}
	r->m[sl].told = 1;
}
/* an "NP" hello or "NR" resume, the first frame of a room connection from a proxy: 0 accepted (-1: the connection ends) */
int m2_room_link(int s, const uint8_t *f, size_t n) {
	if (n >= 4 && f[1] == 'P') { pthread_mutex_lock(&lock); proxied_add(s); pthread_mutex_unlock(&lock); return 0; }
	if (n < 4 + 6 + 24 + 16 || f[1] != 'R') return -1;
	const uint16_t world = r16(f + 4), room = r16(f + 6), id = r16(f + 8); const int sl = (id >> 4) - 1;
	pthread_mutex_lock(&lock);
	Room *r = own_room(world, room); uint8_t proof[16];
	if (r && sl >= 0 && sl < MAXSLOT && r->m[sl].used && r->m[sl].id == id && !memcmp(r->m[sl].npid, f + 10, 24)) aes128_encrypt_block(r->key, f + 10, proof);
	if (!r || sl < 0 || sl >= MAXSLOT || !r->m[sl].used || r->m[sl].id != id || memcmp(r->m[sl].npid, f + 10, 24) || memcmp(proof, f + 34, 16)) {
		pthread_mutex_unlock(&lock); if (m2_log()) fprintf(stderr, "    m2: a resume for room %u/%u member %#x: not one of ours (yet)\n", world, room, id); return -1;
	}
	r->m[sl].sock = s; proxied_add(s);
	const uint8_t ack[4] = { 'N', 'A', 0, 4 }; m2_send(s, ack, 4);
	if (r->gone.used && !r->m[sl].told) tell_gone(r, sl);
	char name[17]; online_name(r->m[sl].npid, name);
	fprintf(stderr, "    m2: %s is back in room %u/%u (its proxy reconnected)\n", name, world, room);
	replicate(r);
	pthread_mutex_unlock(&lock);
	return 0;
}
static const uint8_t *owner_npid(const Room *r) { for (int sl = 0; sl < MAXSLOT; sl++) if (r->m[sl].used && (r->m[sl].flags & M_OWNER)) return r->m[sl].npid; return 0; }
/* (lock held) a room this copy serves that another copy serves too: taken over while this one was away (frozen, its network
 * gone a while). The later generation keeps it (on a tie, two takeovers at once, the owner whose NpId sorts first); the
 * other lets go: its members' connections end -- its own player's game hears what a player cut off from PSN heard -- and
 * the room goes. A dead copy's last advert (kept 5 s) is an earlier generation: it never displaces its successor. */
static int superseded(Room *r, const RemoteRoom *rr, int k) {
	const uint8_t *mine = owner_npid(r);
	for (int j = 0; j < k; j++) {
		if (rr[j].ad.world != r->world || rr[j].ad.room != r->room) continue;
		if (rr[j].ad.gen > r->gen || (rr[j].ad.gen == r->gen && mine && memcmp(rr[j].ad.owner, mine, 24) < 0)) return 1;
	}
	return 0;
}
static void *heartbeat_main(void *arg) {   /* every second: each room to its members' proxies; a taken-over room's stragglers dropped */
	(void)arg;
	for (;;) {
		plat_sleep_ns(1000000000ull);
		const uint64_t t0 = mono_ms();
		RemoteRoom rr[32]; const int k = disc_rooms(rr, 32);   /* (before the lock: discovery takes it the other way round) */
		pthread_mutex_lock(&lock);
		const uint64_t now = mono_ms();
		if (m2_log() && now - t0 > 500) fprintf(stderr, "    m2: heartbeat: %llu ms to the room lock\n", (unsigned long long)(now - t0));
		for (int i = 0; i < MAXROOM; i++) {
			Room *r = &rooms[i]; if (!r->used) continue;
			if (superseded(r, rr, k)) {
				fprintf(stderr, "    m2: room %u/%u: another copy serves it now (taken over while this one was away): this copy lets it go\n", r->world, r->room);
				for (int sl = 0; sl < MAXSLOT; sl++) if (r->m[sl].used && r->m[sl].sock >= 0) shutdown(r->m[sl].sock, 2);   /* (2: both ways, on every platform) */
				memset(r, 0, sizeof *r); disc_poke(); continue;
			}
			if (r->gone.used && r->promoted && now - r->promoted > RESUME_TIMEOUT_MS) {
				for (int sl = 0; sl < MAXSLOT && r->used; sl++) if (r->m[sl].used && r->m[sl].sock < 0 && sl != r->authority) {
					char name[17]; online_name(r->m[sl].npid, name); fprintf(stderr, "    m2: %s didn't come back to room %u/%u\n", name, r->world, r->room);
					remove_member(r, sl, CAUSE_GONE);
				}
				r->promoted = 0;
			}
			if (r->used) replicate(r);
		}
		if (m2_log() && mono_ms() - now > 500) fprintf(stderr, "    m2: heartbeat: the round took %llu ms\n", (unsigned long long)(mono_ms() - now));
		pthread_mutex_unlock(&lock);
	}
	return 0;
}
void m2_rooms_start(void) { pthread_t t; if (!pthread_create(&t, 0, heartbeat_main, 0)) pthread_detach(t); }

