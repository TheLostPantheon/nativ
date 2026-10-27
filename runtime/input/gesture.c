/*
 * SceSystemGesture (vs0:sys/external/libsystemgesture.suprx): touch gesture recognition -- tap,
 * drag, tap-and-hold, pinch, rotation -- over the raw SceTouchData the game reads itself.
 *
 * Why it matters: Killzone: Mercenary drives its whole UI through this library: one DRAG recognizer
 * over the whole front panel, updated once per touch sample, whose BEGIN/END events its menus
 * hit-test against their buttons. With every call stubbed to "zero events" its first screen, the
 * "Connection preferences" dialog, ignored every tap; now a tap on NEVER CONNECT leads on to the
 * Valor card and a tap on CONTINUE to the main menu.
 *
 * Source of the semantics: VitaSDK has no header for this library (vita-headers has only the NID
 * list, db/360/SceSystemGesture.yml), so every layout, default and rule below comes from a
 * console-verified study of the firmware module (a decompilation kept out of this repository, which
 * matches the firmware on the console over all 684 trace records). SceTouchData and
 * SceTouchReport are VitaSDK's public include/psp2/touch.h. Names of the state and type values are
 * ours: the firmware only has the numbers. This is a re-implementation of what the firmware does,
 * written from that reading, not a copy of its code.
 *
 * Two layers, one lock (the firmware's one lightweight mutex):
 *
 * PRIMITIVE recognizer (process-wide, no handle). UpdatePrimitiveTouchRecognizer(front, back)
 * takes each panel's current SceTouchData (either may be NULL) and tracks every touch report by
 * (panel << 8 | report id), its "primitive ID", in a pool of 48:
 *   BEGIN (1)  a report id that wasn't being tracked appears;
 *   ACTIVE (2) it persists (current position, delta since the last update, delta time, elapsed time
 *              since the press, a sticky "moved" flag once it is more than 3 mm from where it was
 *              pressed -- the panel's pixel density turns panel units into mm);
 *   END (3)    it is no longer reported (or its panel's data wasn't passed this time);
 *   CANCEL (4) the report carries SCE_TOUCH_REPORT_INFO_HIDE_UPPER_LAYER, the SceTouchData status
 *              is 0x10000, or more than 200 ms passed since the panel's previous sample; a
 *              cancelled id is not tracked again until it has been lifted.
 * Each lasts one update (BEGIN becomes ACTIVE, END/CANCEL go back to the pool). The primitive
 * event (0x60 bytes): u32 state, u16 primitiveID, s16 pressed x,y, u16 pressed force, s16 current
 * x,y, u16 force, s16 delta x,y, s16 delta force, u64 delta time, u64 elapsed time; listed BEGIN,
 * ACTIVE, END, CANCEL.
 *
 * TOUCH recognizers: SceSystemGestureTouchRecognizer is the game's own memory (>= 0x969 bytes) and
 * all of a recognizer's state lives in it, in the firmware's layout, so any number of them work and
 * Create/Reset clear exactly what the firmware clears:
 *   +0x000 u32 port, +0x004 u32 type, +0x008 u64 time of the panel sample of the last update,
 *   +0x010 u32 event count, +0x018 16 event slots of 0x90 bytes (SceSystemGestureTouchEvent),
 *   +0x918 s16 rect x, y, width, height (panel units; NULL at Create: the panel's active area),
 *   +0x920 the parameter block (0x40 bytes), +0x960 u64 time of the first update, +0x968 u8 created.
 * An event (0x90): u32 eventID, u32 eventState, u32 gestureType, pad, u64 updatedTime, then the
 * property union at +0x18 (offsets below are from the event's start):
 *   TAP (1)          +0x18 u16 primitiveID, +0x1a s16 x,y (where the first press landed),
 *                    +0x1e u8 tappedCount
 *   DRAG (2)         +0x18 u16 primitiveID, +0x1a s16 delta x,y, +0x1e current x,y, +0x22 pressed x,y
 *   TAP_AND_HOLD (4) +0x18 u16 primitiveID, +0x1a s16 pressed x,y
 *   PINCH_OUT_IN (8) +0x18 float scale (finger distance now / at the start), then per finger k
 *   ROTATION (0x10)  +0x18 float angle (radians, signed, unwrapped past +-pi), then per finger k
 *                    (k = 0, 1) at +0x1c + 14k: u16 primitiveID, s16 current x,y, delta x,y,
 *                    start x,y; eventID packs both: (id1 + 1) | (id0 + 1) << 16
 * A single-finger event's eventID is its primitiveID + 1. The rest of a slot is the recognizer's
 * own bookkeeping (a tap's release point and timer, the fingers a pinch has claimed); the firmware
 * hands it out with the event, so the host does too.
 *
 * Events are a recognizer's slots in a non-zero state: GetTouchEvents COPIES them (it does not drain
 * them; the firmware doesn't); each UpdateTouchRecognizer retires what the previous one reported
 * (a TAP after one update, an END/CANCEL after one update), so a game that updates then reads once
 * a frame sees each event once. States: BEGIN 1, ACTIVE 2, END 3, CANCEL 4 (TAP only ever reports
 * 2: a completed tap).
 *
 * Gesture rules (thresholds are the firmware's defaults; the parameter block overrides a few):
 *   TAP: a touch that BEGINs inside the rect claims a slot; released inside it, unmoved (< 3 mm)
 *     and within 400 ms of the press, it counts a tap; the event (tappedCount) is reported once
 *     the count reaches the parameter's tap count (param byte 0; default 1), or 200 ms after the
 *     last release with no further press within 4 mm. A moved, long or cancelled press drops it.
 *   DRAG: no parameters. A touch pressed inside the rect, still inside it, and pressed after the
 *     recognizer's first update BEGINs on its first ACTIVE sample, is ACTIVE while it persists,
 *     ENDs when lifted or when it leaves the rect, CANCELs when cancelled. (No movement threshold:
 *     the firmware starts a drag on any held touch.)
 *   TAP_AND_HOLD: param u64 hold time in ms (default 1000). A touch inside the rect held unmoved
 *     longer than that BEGINs, then ACTIVE while unmoved, END when it moves or lifts.
 *   PINCH_OUT_IN / ROTATION: two touches that BEGIN inside the rect, one after the other, start the
 *     gesture (BEGIN, then ACTIVE); scale / angle are relative to the fingers' positions at the
 *     start; END when either lifts or leaves the rect. One such gesture per recognizer at a time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include "hle.h"

enum { ERR_ARG = 0x80890001u, ERR_TYPE = 0x80890002u, ERR_NOT_INIT = 0x80890003u, ERR_RANGE = 0x80890004u, ERR_NOT_FOUND = 0x80890005u };
enum { T_TAP = 1, T_DRAG = 2, T_HOLD = 4, T_PINCH = 8, T_ROTATION = 0x10 };
enum { S_NONE, S_BEGIN, S_ACTIVE, S_END, S_CANCEL };
static const char *type_name(uint32_t t) {
	return t == T_TAP ? "TAP" : t == T_DRAG ? "DRAG" : t == T_HOLD ? "TAP_AND_HOLD" : t == T_PINCH ? "PINCH_OUT_IN" : t == T_ROTATION ? "ROTATION" : "?";
}
static const char *state_name(uint32_t s) { static const char *n[] = {"NONE", "BEGIN", "ACTIVE", "END", "CANCEL"}; return s < 5 ? n[s] : "?"; }

/* RECOMP_LOG_GESTURE=1: recognizers created/re-aimed, every event an update produces, every
 * GetTouchEvents that hands events to the game; =2 adds the primitive layer's touch tracking */
static int log_level(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_LOG_GESTURE"); v = e ? atoi(e) : 0; } return v; }

/* The panels' pixel densities in panel units per mm, as SceTouch's sceTouchGetPixelDensity2 reports
 * them (dump/decrypted/kd/bootimage/SceTouch.skprx.elf, the constants it stores at boot:
 * 0x418c51ec = 17.54 for front x/y and back x, 0x41c1d70a = 24.23 for back y unless the back
 * panel's id is 0x800a; on hardware codes 0x30/0x31 it stores 22.0 for all four). 1920 front units
 * over 109.5 mm: the OLED model's panel width.
 * They turn distances into mm for the 3 mm "moved" rule, the tap radius and pinch/rotation. */
static float dens_x[2] = {17.54f, 17.54f}, dens_y[2] = {17.54f, 24.23f};   /* (not const: runtime/tests/gesture_test.c sets the console's) */

/* ---- the primitive recognizer (the firmware's module globals) */
typedef struct {
	uint32_t state; uint16_t id;
	int16_t px, py; uint16_t pforce;   /* where and how hard it was pressed */
	int16_t x, y; uint16_t force;
	int16_t dx, dy, dforce;            /* since the previous update */
	uint64_t dt, elapsed;              /* us since the previous update / since the press */
	uint8_t moved;                     /* ever more than 3 mm from the press */
	int next;
} Prim;
#define NPRIM 48
static Prim pool[NPRIM];
static int l_free = -1, l_new = -1, l_active = -1, l_end = -1, l_cancel = -1;   /* singly linked through .next, in order */
static uint32_t nprim;          /* events in the four lists */
static int prim_on;             /* Initialize'd */
static uint8_t cur[2][0x90], prev[2][0x90];   /* each panel's latest and previous SceTouchData */
static uint64_t seen[4], seen_prev[4], held_off[4];   /* report ids by (panel << 1 | id >= 64): present now / last time / cancelled until lifted */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t panel_time(uint32_t port) { uint64_t t = 0; if (port < 2) memcpy(&t, cur[port], 8); return t; }
static void list_append(int *head, int l) {   /* append list l (possibly one element) to *head */
	if (l < 0) return;
	if (*head < 0) { *head = l; return; }
	int t = *head; while (pool[t].next >= 0) t = pool[t].next;
	pool[t].next = l;
}
static void push(int *head, int e) { pool[e].next = -1; list_append(head, e); }
static Prim *prim_find(uint16_t id) {   /* GetPrimitiveTouchEventByPrimitiveID's search */
	int lists[4] = {l_new, l_active, l_end, l_cancel};
	for (int k = 0; k < 4; k++) for (int e = lists[k]; e >= 0; e = pool[e].next) if (pool[e].id == id) return &pool[e];
	return 0;
}
static void put_prim(uint32_t a, const Prim *p) {
	WR32(a, p->state); WR16(a + 4, p->id);
	WR16(a + 6, p->px); WR16(a + 8, p->py); WR16(a + 10, p->pforce);
	WR16(a + 12, p->x); WR16(a + 14, p->y); WR16(a + 16, p->force);
	WR16(a + 18, p->dx); WR16(a + 20, p->dy); WR16(a + 22, p->dforce);
	WR64(a + 24, p->dt); WR64(a + 32, p->elapsed);
}

static void prim_reset(void) {
	l_free = l_new = l_active = l_end = l_cancel = -1;
	nprim = 0;
	held_off[0] = held_off[1] = 0;   /* the firmware clears only the front panel's cancelled ids here */
	for (int i = 0; i < NPRIM; i++) { memset(&pool[i], 0, sizeof pool[i]); push(&l_free, i); }
}
static void prim_wipe(void) {   /* Initialize/Finalize: the pool, the lists and both panels' samples */
	memset(pool, 0, sizeof pool); memset(cur, 0, sizeof cur); memset(prev, 0, sizeof prev);
	l_free = l_new = l_active = l_end = l_cancel = -1; nprim = 0; prim_on = 0;
}

static void prim_update(uint32_t front, uint32_t back) {
	/* last update's END and CANCEL go back to the pool, its BEGINs are ACTIVE now */
	for (int e = l_cancel; e >= 0; e = pool[e].next) { pool[e].elapsed = 0; pool[e].dt = 0; pool[e].state = S_NONE; }
	list_append(&l_free, l_cancel); l_cancel = -1;
	for (int e = l_end; e >= 0; e = pool[e].next) { pool[e].elapsed = 0; pool[e].dt = 0; pool[e].state = S_NONE; }
	list_append(&l_free, l_end); l_end = -1;
	for (int e = l_new; e >= 0; e = pool[e].next) pool[e].state = S_ACTIVE;
	list_append(&l_active, l_new); l_new = -1;
	memcpy(prev, cur, sizeof cur);
	nprim = 0;
	for (int i = 0; i < 4; i++) { seen_prev[i] = seen[i]; seen[i] = 0; }
	int lnew = -1, lact = -1, lcan = -1;
	for (uint32_t panel = 0; panel < 2; panel++) {
		uint32_t src = panel ? back : front;
		if (!src) continue;
		memcpy(cur[panel], G(src), 0x90);
		uint64_t ts, pts; uint32_t status, n;
		memcpy(&ts, cur[panel], 8); memcpy(&pts, prev[panel], 8); memcpy(&status, cur[panel] + 8, 4); memcpy(&n, cur[panel] + 12, 4);
		int gap = status == 0x10000 || (pts && (uint32_t)(ts - pts) > 200000);
		if (n > 8) n = 8;
		for (uint32_t r = 0; r < n; r++) {
			const uint8_t *rep = cur[panel] + 16 + 16 * r;   /* SceTouchReport: u8 id, u8 force, s16 x, s16 y, u8 reserved[8], u16 info */
			uint8_t rid = rep[0], force = rep[1]; int16_t x, y; uint16_t info;
			memcpy(&x, rep + 2, 2); memcpy(&y, rep + 4, 2); memcpy(&info, rep + 14, 2);
			int half = rid >= 64; uint32_t sh = rid - 64u * half;
			uint64_t bit = sh < 64 ? 1ull << sh : 0;
			int idx = half | panel << 1;
			int cancel = gap || info == 1;   /* SCE_TOUCH_REPORT_INFO_HIDE_UPPER_LAYER: the system has it */
			seen[idx] |= bit;
			if (cancel) held_off[idx] |= bit;
			uint16_t id = (uint16_t)(panel << 8 | rid);
			int e, pe = -1;
			for (e = l_active; e >= 0 && pool[e].id != id; e = pool[e].next) pe = e;
			if (e >= 0) {   /* still down */
				Prim *p = &pool[e];
				if (pe < 0) l_active = p->next; else pool[pe].next = p->next;
				p->dx = x - p->x; p->dy = y - p->y; p->dforce = force - p->force;
				p->x = x; p->y = y; p->force = force;
				p->dt = (uint32_t)(ts - pts); p->elapsed += p->dt;
				float fy = (float)(y - p->py) / dens_y[panel], fx = (float)(x - p->px) / dens_x[panel];
				if (fy * fy + fx * fx > 9.0f) p->moved = 1;
				p->state = cancel ? S_CANCEL : S_ACTIVE;
				push(cancel ? &lcan : &lact, e);
				nprim++;
			} else if (!(held_off[idx] & bit) && l_free >= 0) {   /* a new touch */
				e = l_free; l_free = pool[e].next;
				Prim *p = &pool[e];
				memset(p, 0, sizeof *p);
				p->state = S_BEGIN; p->id = id;
				p->x = p->px = x; p->y = p->py = y; p->force = p->pforce = force;
				push(&lnew, e);
				nprim++;
			}
		}
	}
	for (int e = l_active; e >= 0; e = pool[e].next) { pool[e].state = S_END; nprim++; }   /* not reported any more */
	l_end = l_active;
	l_new = lnew; l_active = lact; l_cancel = lcan;
	for (int i = 0; i < 4; i++) held_off[i] &= ~(seen_prev[i] & ~seen[i]);   /* lifted: trackable again */
	if (log_level() >= 2) {
		int lists[4] = {l_new, l_active, l_end, l_cancel};
		for (int k = 0; k < 4; k++) for (int e = lists[k]; e >= 0; e = pool[e].next) {
			Prim *p = &pool[e];
			fprintf(stderr, "gesture: primitive %#x %s at %d,%d (pressed %d,%d, delta %d,%d, %llu us%s)\n", p->id, state_name(p->state),
				p->x, p->y, p->px, p->py, p->dx, p->dy, (unsigned long long)p->elapsed, p->moved ? ", moved" : "");
		}
	}
}

static void h_prim_init(Cpu *c) {
	pthread_mutex_lock(&mu);
	prim_wipe(); prim_on = 1; prim_reset();
	pthread_mutex_unlock(&mu);
	RET(0);
}
static void h_prim_finalize(Cpu *c) { pthread_mutex_lock(&mu); prim_wipe(); pthread_mutex_unlock(&mu); RET(0); }
static void h_prim_reset(Cpu *c) {
	pthread_mutex_lock(&mu);
	if (prim_on) prim_reset();
	pthread_mutex_unlock(&mu);
	RET(prim_on ? 0 : ERR_NOT_INIT);
}
static void h_prim_update(Cpu *c) {   /* (const SceTouchData *front, const SceTouchData *back) */
	if (!prim_on) { RET(ERR_NOT_INIT); return; }
	if (!A0 && !A1) { RET(ERR_ARG); return; }
	pthread_mutex_lock(&mu);
	prim_update(A0, A1);
	pthread_mutex_unlock(&mu);
	RET(0);
}
static void h_prim_events(Cpu *c) {   /* (event *buffer, u32 capacity, s32 *count) */
	if (!prim_on) { RET(ERR_NOT_INIT); return; }
	if (!A0 || !A1 || !A2) { RET(ERR_ARG); return; }
	pthread_mutex_lock(&mu);
	memset(G(A0), 0, (size_t)A1 * 0x60);
	uint32_t n = 0, cap = A1 < nprim ? A1 : nprim;
	int lists[4] = {l_new, l_active, l_end, l_cancel};
	for (int k = 0; k < 4; k++) for (int e = lists[k]; e >= 0 && n < cap; e = pool[e].next) put_prim(A0 + 0x60 * n++, &pool[e]);
	pthread_mutex_unlock(&mu);
	WR32(A2, n);
	RET(0);
}
static void h_prim_count(Cpu *c) { RET(prim_on ? nprim : ERR_NOT_INIT); }
static void h_prim_by_index(Cpu *c) {   /* (u32 index, event *) */
	if (!prim_on) { RET(ERR_NOT_INIT); return; }
	if (!A1) { RET(ERR_ARG); return; }
	pthread_mutex_lock(&mu);
	uint32_t r = ERR_RANGE, i = 0;
	int lists[4] = {l_new, l_active, l_end, l_cancel};
	if (A0 < nprim)
		for (int k = 0; k < 4 && r; k++) for (int e = lists[k]; e >= 0; e = pool[e].next, i++) if (i == A0) { put_prim(A1, &pool[e]); r = 0; break; }
	pthread_mutex_unlock(&mu);
	RET(r);
}
static void h_prim_by_id(Cpu *c) {   /* (s16 primitiveID, event *) */
	if (!prim_on) { RET(ERR_NOT_INIT); return; }
	if (!A1) { RET(ERR_ARG); return; }
	pthread_mutex_lock(&mu);
	Prim *p = prim_find((uint16_t)A0);
	if (p) put_prim(A1, p);
	pthread_mutex_unlock(&mu);
	RET(p ? 0 : ERR_NOT_FOUND);
}

/* ---- touch recognizers (state in the game's SceSystemGestureTouchRecognizer) */
enum { R_PORT = 0, R_TYPE = 4, R_TIME = 8, R_COUNT = 0x10, R_SLOTS = 0x18, R_RECT = 0x918, R_PARAM = 0x920, R_START = 0x960, R_CREATED = 0x968 };
enum { E_ID = 0, E_STATE = 4, E_TYPE = 8, E_TIME = 0x10, E_SIZE = 0x90, NSLOT = 16 };
#define SLOT(r, i) ((r) + R_SLOTS + (uint32_t)(i) * E_SIZE)
static int16_t rs16(uint32_t a) { return (int16_t)RD16(a); }
static float rdf(uint32_t a) { float f; memcpy(&f, G(a), 4); return f; }
static void wrf(uint32_t a, float f) { memcpy(G(a), &f, 4); }
static int in_rect(uint32_t r, int x, int y) {
	int rx = rs16(r + R_RECT), ry = rs16(r + R_RECT + 2), w = rs16(r + R_RECT + 4), h = rs16(r + R_RECT + 6);
	return rx <= x && ry <= y && x <= rx + w && y <= ry + h;
}
static uint32_t slot_with_id(uint32_t r, uint32_t id) { for (int i = 0; i < NSLOT; i++) if (RD32(SLOT(r, i)) == id) return SLOT(r, i); return 0; }
static void clear_slots(uint32_t r) {   /* Create/Reset: id, state, type and the property area; the rest of a slot stays */
	for (int i = 0; i < NSLOT; i++) {
		uint32_t s = SLOT(r, i);
		WR32(s + E_ID, 0); WR32(s + E_STATE, 0); WR32(s + E_TYPE, RD32(r + R_TYPE));
		memset(G(s + 0x18), 0, 0x40);
	}
}
static void rec_reset(uint32_t r) {
	WR64(r + R_TIME, 0); WR64(r + R_START, 0); WR32(r + R_COUNT, 0);
	clear_slots(r);
}
/* the closing count of every type: events in a non-zero state, stamped with this update's time */
static void count_events(uint32_t r, uint64_t now, int stamp) {
	uint32_t n = 0;
	for (int i = 0; i < NSLOT; i++) {
		uint32_t s = SLOT(r, i);
		if (RD32(s + E_STATE)) { if (stamp) WR64(s + E_TIME, now); n++; }
	}
	WR32(r + R_COUNT, n);
}

/* TAP slot bookkeeping (event offsets): +0x1f u8 released-and-waiting, +0x20 u64 us since the
 * release, +0x28 s16 x,y where it was released */
static void rec_tap(uint32_t r) {
	uint32_t port = RD32(r + R_PORT); uint64_t now = panel_time(port);
	uint32_t taps = RD32(r + R_PARAM), max_press_ms = RD32(r + R_PARAM + 8), wait_ms = RD32(r + R_PARAM + 16);
	float radius = (float)RD32(r + R_PARAM + 12);
	for (int i = 0; i < NSLOT; i++) {
		uint32_t s = SLOT(r, i);
		if (RD32(s + E_STATE) == S_ACTIVE) { WR32(s + E_ID, 0); WR32(s + E_STATE, 0); WR8(s + 0x1f, 0); }   /* reported last update */
		else if (!RD32(s + E_STATE) && RD32(s + E_ID) && RD8(s + 0x1f)) {   /* waiting for another tap */
			uint64_t acc = RD64(s + 0x20) + (uint32_t)(now - RD64(s + E_TIME));
			WR64(s + 0x20, acc); WR64(s + E_TIME, now);
			if ((acc >> 32) || wait_ms * 1000u < (uint32_t)acc) WR32(s + E_STATE, S_ACTIVE);   /* none came: report the count */
		}
	}
	for (int e = l_new; e >= 0; e = pool[e].next) {   /* presses */
		Prim *p = &pool[e];
		if (p->id >> 8 != port || !in_rect(r, p->x, p->y)) continue;
		int panel = p->id >> 8, done = 0;
		for (int i = 0; i < NSLOT && !done; i++) {   /* the next tap of a multi-tap: near the last release */
			uint32_t s = SLOT(r, i);
			if (RD32(s + E_STATE) || !RD32(s + E_ID) || !RD8(s + 0x1f)) continue;
			float fy = (float)(p->y - rs16(s + 0x2a)) / dens_y[panel], fx = (float)(p->x - rs16(s + 0x28)) / dens_x[panel];
			if (fy * fy + fx * fx < radius * radius) { WR32(s + E_ID, p->id + 1u); WR8(s + 0x1f, 0); WR16(s + 0x18, p->id); done = 1; }
		}
		for (int i = 0; i < NSLOT && !done; i++) {
			uint32_t s = SLOT(r, i);
			if (RD32(s + E_ID)) continue;
			WR32(s + E_ID, p->id + 1u); WR32(s + E_STATE, 0); WR16(s + 0x18, p->id);
			WR8(s + 0x1e, 0); WR8(s + 0x1f, 0); WR64(s + 0x20, 0);
			WR16(s + 0x1a, p->px); WR16(s + 0x1c, p->py); WR16(s + 0x28, 0xffff); WR16(s + 0x2a, 0xffff);
			done = 1;
		}
	}
	for (int e = l_end; e >= 0; e = pool[e].next) {   /* releases */
		Prim *p = &pool[e];
		if (p->id >> 8 != port) continue;
		uint32_t s = slot_with_id(r, p->id + 1u);
		if (!s) continue;
		if (!p->moved && !(p->elapsed >> 32) && (uint32_t)p->elapsed < max_press_ms * 1000u && in_rect(r, p->x, p->y)) {
			uint8_t n = RD8(s + 0x1e) + 1;
			WR64(s + E_TIME, now); WR8(s + 0x1e, n); WR8(s + 0x1f, 1);
			WR16(s + 0x28, p->x); WR16(s + 0x2a, p->y); WR64(s + 0x20, 0);
			if (n == taps) WR32(s + E_STATE, S_ACTIVE);
		} else WR32(s + E_ID, 0);   /* moved, held too long or released outside: not a tap */
	}
	for (int e = l_cancel; e >= 0; e = pool[e].next) {
		Prim *p = &pool[e];
		uint32_t s = p->id >> 8 == port ? slot_with_id(r, p->id + 1u) : 0;
		if (s) WR32(s + E_ID, 0);
	}
	count_events(r, now, 1);
}

/* ends (lifted) and cancels of the touches a slot follows: DRAG and TAP_AND_HOLD alike */
static void end_and_cancel(uint32_t r, uint32_t port, int only_live) {
	for (int k = 0; k < 2; k++)
		for (int e = k ? l_cancel : l_end; e >= 0; e = pool[e].next) {
			Prim *p = &pool[e];
			if (p->id >> 8 != port) continue;
			for (int i = 0; i < NSLOT; i++) {
				uint32_t s = SLOT(r, i), st = RD32(s + E_STATE);
				if (RD32(s + E_ID) == p->id + 1u && (!only_live || st == S_BEGIN || st == S_ACTIVE)) { WR32(s + E_STATE, k ? S_CANCEL : S_END); break; }
			}
		}
}

static void rec_drag(uint32_t r) {
	uint32_t port = RD32(r + R_PORT); uint64_t now = panel_time(port);
	uint32_t age = (uint32_t)(RD64(r + R_TIME) - RD64(r + R_START));   /* since the recognizer's first update */
	for (int i = 0; i < NSLOT; i++) { uint32_t s = SLOT(r, i), st = RD32(s + E_STATE); if (st == S_END || st == S_CANCEL) WR32(s + E_STATE, 0); }
	for (int e = l_active; e >= 0; e = pool[e].next) {
		Prim *p = &pool[e];
		if (p->id >> 8 != port) continue;
		uint32_t id = p->id + 1u;
		if (in_rect(r, p->px, p->py) && in_rect(r, p->x, p->y) && !(p->elapsed >> 32) && (uint32_t)p->elapsed <= age) {
			int i, followed = 0;
			for (i = 0; i < NSLOT; i++) {
				uint32_t s = SLOT(r, i), st = RD32(s + E_STATE);
				if (RD32(s + E_ID) != id || (st != S_BEGIN && st != S_ACTIVE)) continue;
				WR32(s + E_STATE, S_ACTIVE); WR64(s + E_TIME, now);
				WR16(s + 0x1e, p->x); WR16(s + 0x20, p->y); WR16(s + 0x1a, p->dx); WR16(s + 0x1c, p->dy);
				break;
			}
			if (i < NSLOT) continue;
			/* a drag that already ended (it left the rect) doesn't restart while the same touch is
			 * down; hardware numbers each new contact afresh, so a new touch always can */
			int last_free = -1;
			for (i = 0; i < NSLOT; i++) {
				uint32_t s = SLOT(r, i);
				if (RD32(s + E_STATE)) continue;
				if (RD32(s + E_ID) == id) followed = 1;
				last_free = i;
			}
			if (followed || last_free < 0) continue;
			uint32_t s = SLOT(r, last_free);
			WR32(s + E_ID, id); WR32(s + E_STATE, S_BEGIN); WR16(s + 0x18, p->id);
			WR16(s + 0x22, p->px); WR16(s + 0x24, p->py); WR16(s + 0x1e, p->x); WR16(s + 0x20, p->y);
			WR16(s + 0x1a, p->dx); WR16(s + 0x1c, p->dy);
		} else
			for (int i = 0; i < NSLOT; i++) {
				uint32_t s = SLOT(r, i);
				if (RD32(s + E_ID) == id && RD32(s + E_STATE)) { WR32(s + E_STATE, S_END); break; }
			}
	}
	end_and_cancel(r, port, 1);
	count_events(r, now, 1);
}

static void rec_hold(uint32_t r) {
	uint32_t port = RD32(r + R_PORT); uint64_t now = panel_time(port);
	uint32_t age = (uint32_t)(RD64(r + R_TIME) - RD64(r + R_START));
	uint64_t hold_us = RD64(r + R_PARAM) * 1000u;
	for (int i = 0; i < NSLOT; i++) { uint32_t s = SLOT(r, i), st = RD32(s + E_STATE); if (st == S_END || st == S_CANCEL) { WR32(s + E_ID, 0); WR32(s + E_STATE, 0); } }
	for (int e = l_active; e >= 0; e = pool[e].next) {
		Prim *p = &pool[e];
		if (p->id >> 8 != port || !in_rect(r, p->x, p->y) || (p->elapsed >> 32) || (uint32_t)p->elapsed > age) continue;
		uint32_t s = slot_with_id(r, p->id + 1u);
		if (s) { WR32(s + E_STATE, p->moved ? S_END : S_ACTIVE); continue; }
		if (p->moved || !(hold_us < p->elapsed)) continue;
		int k = -1;
		for (int i = 0; i < NSLOT; i++) if (!RD32(SLOT(r, i) + E_ID)) k = i;
		if (k < 0) continue;
		s = SLOT(r, k);
		WR32(s + E_ID, p->id + 1u); WR32(s + E_STATE, S_BEGIN); WR16(s + 0x18, p->id);
		WR16(s + 0x1a, p->px); WR16(s + 0x1c, p->py);
	}
	end_and_cancel(r, port, 0);
	count_events(r, now, 1);
}

/* PINCH_OUT_IN and ROTATION: a slot claims two touches (u8 flags at +0x38/+0x39), the recognizer's
 * +0x924 (inside the parameter block, as in the firmware) says a gesture is under way; ROTATION
 * keeps its turn count at +0x3a (s8) and last raw angle at +0x3c (float) to unwrap past +-pi */
static void rec_two(uint32_t r, int rot) {
	uint32_t port = RD32(r + R_PORT), busy = r + R_PARAM + 4; uint64_t now = panel_time(port);
	float dx = dens_x[port != 0], dy = dens_y[port != 0];   /* mm per panel unit, as the firmware picks them: front for port 0 */
	for (int i = 0; i < NSLOT; i++) {
		uint32_t s = SLOT(r, i), st = RD32(s + E_STATE);
		if (st == S_END || st == S_CANCEL) { WR32(s + E_ID, 0); WR32(s + E_STATE, 0); WR32(busy, 0); }
		else if (st == S_BEGIN) WR32(s + E_STATE, S_ACTIVE);
	}
	if (!RD32(busy)) {   /* waiting for two fingers to land in the rect, one per update or both at once */
		int k = -1;
		for (int i = 0; i < NSLOT && k < 0; i++) if (!RD8(SLOT(r, i) + 0x38) || !RD8(SLOT(r, i) + 0x39)) k = i;
		uint32_t s = k >= 0 ? SLOT(r, k) : 0;
		for (int e = l_new; s && e >= 0 && !RD32(busy); e = pool[e].next) {
			Prim *p = &pool[e];
			if (p->id >> 8 != port || !in_rect(r, p->x, p->y)) break;
			if (!RD8(s + 0x38)) { WR8(s + 0x38, 1); WR16(s + 0x1c, p->id); }
			else if (!RD8(s + 0x39)) { WR8(s + 0x39, 1); WR16(s + 0x2a, p->id); }
			if (!RD8(s + 0x38) || !RD8(s + 0x39)) continue;
			WR32(busy, 1); WR32(s + E_STATE, S_BEGIN);
			WR32(s + E_ID, (RD16(s + 0x2a) + 1u) | (RD16(s + 0x1c) + 1u) << 16);
			if (rot) { WR8(s + 0x3a, 0); wrf(s + 0x3c, 0); }
			int bad = 0;
			for (int f = 0; f < 2; f++) {
				Prim *q = prim_find(RD16(s + 0x1c + 14 * f));
				if (!q) { WR8(s + 0x38 + f, 0); bad = 1; } else { WR16(s + 0x26 + 14 * f, q->x); WR16(s + 0x28 + 14 * f, q->y); }
			}
			/* (on the impossible failure the firmware's PINCH leaves itself "busy" with no slot, and
			 * every later update then resets the recognizer: ROTATION's choice, clearing it, is used for both) */
			if (bad) { WR32(s + E_ID, 0); WR32(s + E_STATE, 0); WR32(busy, 0); }
			else { wrf(s + 0x18, rot ? 0.0f : 1.0f); WR64(s + E_TIME, now); }
		}
	} else {
		int i;
		for (i = 0; i < NSLOT && !RD32(SLOT(r, i) + E_ID); i++) ;
		if (i == NSLOT) rec_reset(r);   /* under way with nothing claimed: start over */
		else {
			uint32_t s = SLOT(r, i);
			Prim *q[2];
			for (int f = 0; f < 2; f++) {
				q[f] = prim_find(RD16(s + 0x1c + 14 * f));
				if (!q[f]) WR8(s + 0x38 + f, 0);
				else { WR16(s + 0x1e + 14 * f, q[f]->x); WR16(s + 0x20 + 14 * f, q[f]->y); WR16(s + 0x22 + 14 * f, q[f]->dx); WR16(s + 0x24 + 14 * f, q[f]->dy); }
			}
			if (!q[0] || !q[1]) WR32(s + E_STATE, S_END);
			else if (q[0]->state != S_END && q[0]->state != S_CANCEL && in_rect(r, q[0]->x, q[0]->y) &&
			         q[1]->state != S_END && q[1]->state != S_CANCEL && in_rect(r, q[1]->x, q[1]->y)) {
				/* start and current finger-to-finger vectors, in mm */
				float sx = (float)(rs16(s + 0x26) - rs16(s + 0x34)) / dx, sy = (float)(rs16(s + 0x28) - rs16(s + 0x36)) / dy;
				float cx = (float)(rs16(s + 0x1e) - rs16(s + 0x2c)) / dx, cy = (float)(rs16(s + 0x20) - rs16(s + 0x2e)) / dy;
				float s2 = sx * sx + sy * sy;
				if (s2 > 0) {
					if (!rot) wrf(s + 0x18, sqrtf((cx * cx + cy * cy) / s2));
					else {
						float mag = sqrtf(cx * cx + cy * cy) * sqrtf(s2), angle = 0;
						if (mag != 0) {
							float cosv = (sx * cx + sy * cy) / mag;
							if (cosv > 1) cosv = 1;
							if (cosv < -1) cosv = -1;
							float a = acosf(cosv), signed_a = a * (sx * cy - sy * cx > 0 ? 1.0f : -1.0f), last = rdf(s + 0x3c);
							int8_t turns = (int8_t)RD8(s + 0x3a);
							if (a > 1.5707964f) {   /* near +-pi: crossing it adds or removes a turn */
								if (last > 0 && signed_a < 0) turns++;
								else if (last < 0 && signed_a > 0) turns--;
								WR8(s + 0x3a, (uint8_t)turns);
							}
							wrf(s + 0x3c, signed_a);
							angle = (float)((double)signed_a + turns * 6.283185307);
						}
						wrf(s + 0x18, angle);
					}
					WR64(s + E_TIME, now);
				}
			} else {
				int cancel = 0;
				for (int f = 0; f < 2; f++) {
					cancel = q[f]->state == S_CANCEL;
					if (q[f]->state == S_END || cancel) WR8(s + 0x38 + f, 0);
				}
				if (RD32(s + E_STATE) == S_ACTIVE) WR32(s + E_STATE, cancel ? S_CANCEL : S_END);
			}
		}
	}
	for (int k = 0; k < 2; k++)   /* lifted or cancelled fingers stop being claimed (any panel, as the firmware) */
		for (int e = k ? l_cancel : l_end; e >= 0; e = pool[e].next)
			for (int i = 0; i < NSLOT; i++)
				for (int f = 0; f < 2; f++) {
					uint32_t s = SLOT(r, i);
					if (RD8(s + 0x38 + f) == 1 && RD16(s + 0x1c + 14 * f) == pool[e].id) WR8(s + 0x38 + f, 0);
				}
	count_events(r, now, 0);
}

static void log_event(uint32_t r, uint32_t s) {
	uint32_t type = RD32(s + E_TYPE);
	fprintf(stderr, "gesture: recognizer %#x %s event %#x %s", r, type_name(type), RD32(s + E_ID), state_name(RD32(s + E_STATE)));
	if (type == T_TAP) fprintf(stderr, " at %d,%d taps %u", rs16(s + 0x1a), rs16(s + 0x1c), RD8(s + 0x1e));
	else if (type == T_DRAG) fprintf(stderr, " at %d,%d delta %d,%d pressed %d,%d", rs16(s + 0x1e), rs16(s + 0x20), rs16(s + 0x1a), rs16(s + 0x1c), rs16(s + 0x22), rs16(s + 0x24));
	else if (type == T_HOLD) fprintf(stderr, " at %d,%d", rs16(s + 0x1a), rs16(s + 0x1c));
	else fprintf(stderr, " %s %f, fingers %d,%d and %d,%d", type == T_PINCH ? "scale" : "angle", rdf(s + 0x18), rs16(s + 0x1e), rs16(s + 0x20), rs16(s + 0x2c), rs16(s + 0x2e));
	fprintf(stderr, " (t=%llu)\n", (unsigned long long)RD64(s + E_TIME));
}

static void h_create(Cpu *c) {   /* (recognizer, u32 type, u32 port, const s16 rect[4] or NULL, const u32 *param or NULL) */
	uint32_t r = A0, type = A1, port = A2 & 0xff, rect = A3, param = STK(0);
	if (!r) { RET(ERR_ARG); return; }
	if (type != T_TAP && type != T_DRAG && type != T_HOLD && type != T_PINCH && type != T_ROTATION) { RET(ERR_TYPE); return; }
	WR32(r + R_PORT, port); WR32(r + R_TYPE, type);
	WR64(r + R_TIME, 0); WR64(r + R_START, 0);
	if (rect) for (int k = 0; k < 4; k++) WR16(r + R_RECT + 2 * k, RD16(rect + 2 * k));
	else {   /* the panel's active area (sceTouchGetPanelInfo's minAa/maxAa, display.c's numbers) */
		if (port > 1) { RET(0x80350001u); return; }   /* SCE_TOUCH_ERROR_INVALID_ARG */
		WR16(r + R_RECT, 0); WR16(r + R_RECT + 2, 0); WR16(r + R_RECT + 4, 1919); WR16(r + R_RECT + 6, port ? 889 : 1087);
	}
	memset(G(r + R_PARAM), 0, 0x40);
	if (type == T_TAP) {   /* tap count (param byte 0, 0 means 1), 3 (unread), max press 400 ms, multi-tap radius 4 mm, multi-tap wait 200 ms */
		uint32_t n = param ? RD8(param) : 1;
		WR32(r + R_PARAM, n ? n : 1); WR32(r + R_PARAM + 4, 3); WR32(r + R_PARAM + 8, 400); WR32(r + R_PARAM + 12, 4); WR32(r + R_PARAM + 16, 200);
	} else if (type == T_HOLD) {   /* hold time in ms, a u64 (default 1000) */
		WR32(r + R_PARAM, param ? RD32(param) : 1000); WR32(r + R_PARAM + 4, param ? RD32(param + 4) : 0);
	} else if (type == T_PINCH || type == T_ROTATION) WR32(r + R_PARAM, 1);   /* (DRAG takes no parameters) */
	WR8(r + R_CREATED, 1);
	WR32(r + R_COUNT, 0);
	clear_slots(r);
	if (log_level())
		fprintf(stderr, "gesture: recognizer %#x created: %s on port %u, rect %d,%d %dx%d%s, param %#x (%u %u %u %u %u)\n", r, type_name(type), port,
			rs16(r + R_RECT), rs16(r + R_RECT + 2), rs16(r + R_RECT + 4), rs16(r + R_RECT + 6), rect ? "" : " (panel)", param,
			RD32(r + R_PARAM), RD32(r + R_PARAM + 4), RD32(r + R_PARAM + 8), RD32(r + R_PARAM + 12), RD32(r + R_PARAM + 16));
	RET(0);
}
static int check(Cpu *c, uint32_t r) {
	if (!r) { RET(ERR_ARG); return 0; }
	if (!RD8(r + R_CREATED)) { RET(ERR_NOT_INIT); return 0; }
	return 1;
}
static void h_reset(Cpu *c) {
	if (!check(c, A0)) return;
	rec_reset(A0);
	if (log_level()) fprintf(stderr, "gesture: recognizer %#x reset\n", A0);
	RET(0);
}
static void h_update_rect(Cpu *c) {   /* (recognizer, const s16 rect[4]) */
	if (!A0 || !A1) { RET(ERR_ARG); return; }
	if (!check(c, A0)) return;
	int changed = 0;
	for (int k = 0; k < 4; k++) { changed |= RD16(A0 + R_RECT + 2 * k) != RD16(A1 + 2 * k); WR16(A0 + R_RECT + 2 * k, RD16(A1 + 2 * k)); }
	if (changed && log_level()) fprintf(stderr, "gesture: recognizer %#x rect %d,%d %dx%d\n", A0, rs16(A1), rs16(A1 + 2), rs16(A1 + 4), rs16(A1 + 6));
	RET(0);
}
static void h_info(Cpu *c) {   /* (recognizer, info *): type, port, rect, u64 time of the last update */
	if (!A0 || !A1) { RET(ERR_ARG); return; }
	if (!check(c, A0)) return;
	WR32(A1, RD32(A0 + R_TYPE)); WR32(A1 + 4, RD32(A0 + R_PORT));
	for (int k = 0; k < 4; k++) WR16(A1 + 8 + 2 * k, RD16(A0 + R_RECT + 2 * k));
	WR64(A1 + 16, RD64(A0 + R_TIME));
	RET(0);
}
static void h_update(Cpu *c) {   /* (recognizer): after UpdatePrimitiveTouchRecognizer, once per sample */
	uint32_t r = A0;
	if (!check(c, r)) return;
	pthread_mutex_lock(&mu);
	uint64_t t = panel_time(RD32(r + R_PORT));
	WR64(r + R_TIME, t);
	if (!RD64(r + R_START)) WR64(r + R_START, t);
	switch (RD32(r + R_TYPE)) {
	case T_TAP: rec_tap(r); break;
	case T_DRAG: rec_drag(r); break;
	case T_HOLD: rec_hold(r); break;
	case T_PINCH: rec_two(r, 0); break;
	case T_ROTATION: rec_two(r, 1); break;
	}
	if (log_level())
		for (int i = 0; i < NSLOT; i++) if (RD32(SLOT(r, i) + E_STATE)) log_event(r, SLOT(r, i));
	pthread_mutex_unlock(&mu);
	RET(0);
}
static void h_events(Cpu *c) {   /* (recognizer, event *buffer, u32 capacity, u32 *count): copies, doesn't drain */
	uint32_t r = A0, buf = A1, cap = A2, out = A3;
	if (!r || !buf || !cap || !out) { RET(ERR_ARG); return; }
	if (!check(c, r)) return;
	memset(G(buf), 0, cap);   /* (sic: the firmware clears capacity BYTES, not events) */
	uint32_t want = RD32(r + R_COUNT) < cap ? RD32(r + R_COUNT) : cap, n = 0;
	for (int i = 0; i < NSLOT && n < want; i++)
		if (RD32(SLOT(r, i) + E_STATE)) memcpy(G(buf + E_SIZE * n++), G(SLOT(r, i)), E_SIZE);
	WR32(out, n);
	if (n && log_level()) fprintf(stderr, "gesture: recognizer %#x: GetTouchEvents -> %u event(s)\n", r, n);
	RET(0);
}
static void h_events_count(Cpu *c) { if (!check(c, A0)) return; RET(RD32(A0 + R_COUNT)); }
static void h_event_by_index(Cpu *c) {   /* (recognizer, u32 index, event *) */
	if (!A0 || !A2) { RET(ERR_ARG); return; }
	if (!check(c, A0)) return;
	if (A1 >= RD32(A0 + R_COUNT)) { RET(ERR_RANGE); return; }
	for (uint32_t i = 0, n = 0; i < NSLOT; i++)
		if (RD32(SLOT(A0, i) + E_STATE) && n++ == A1) { memcpy(G(A2), G(SLOT(A0, i)), E_SIZE); break; }
	RET(0);
}
static void h_event_by_id(Cpu *c) {   /* (recognizer, u32 eventID, event *) */
	if (!A0 || !A2) { RET(ERR_ARG); return; }
	if (!check(c, A0)) return;
	for (int i = 0; i < NSLOT; i++) {
		uint32_t s = SLOT(A0, i);
		if (RD32(s + E_STATE) && RD32(s + E_ID) == A1) { memcpy(G(A2), G(s), E_SIZE); RET(0); return; }
	}
	RET(ERR_NOT_FOUND);
}

const HleEntry hle_gesture[] = {
	{"sceSystemGestureInitializePrimitiveTouchRecognizer", h_prim_init}, {"sceSystemGestureFinalizePrimitiveTouchRecognizer", h_prim_finalize},
	{"sceSystemGestureResetPrimitiveTouchRecognizer", h_prim_reset}, {"sceSystemGestureUpdatePrimitiveTouchRecognizer", h_prim_update},
	{"sceSystemGestureGetPrimitiveTouchEvents", h_prim_events}, {"sceSystemGestureGetPrimitiveTouchEventsCount", h_prim_count},
	{"sceSystemGestureGetPrimitiveTouchEventByIndex", h_prim_by_index}, {"sceSystemGestureGetPrimitiveTouchEventByPrimitiveID", h_prim_by_id},
	{"sceSystemGestureCreateTouchRecognizer", h_create}, {"sceSystemGestureResetTouchRecognizer", h_reset},
	{"sceSystemGestureUpdateTouchRecognizerRectangle", h_update_rect}, {"sceSystemGestureUpdateTouchRecognizer", h_update},
	{"sceSystemGestureGetTouchRecognizerInformation", h_info}, {"sceSystemGestureGetTouchEvents", h_events},
	{"sceSystemGestureGetTouchEventsCount", h_events_count}, {"sceSystemGestureGetTouchEventByIndex", h_event_by_index},
	{"sceSystemGestureGetTouchEventByEventID", h_event_by_id},
	{0, 0},
};
