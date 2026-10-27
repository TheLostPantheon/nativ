/*
 * The SceSystemGesture test scenario, run unchanged against the firmware library on the console and
 * against runtime/input/gesture.c (gesture_test.c), producing comparable traces (trace.h). VitaSDK has no header for
 * this module; types and call semantics come from a study of the firmware module (see runtime/input/gesture.c).
 *
 * Two independent layers, both behind one static mutex in the library:
 *  - "Primitive": a single global per-process recognizer (no handle argument). Feed it the front
 *    and/or back panel's current SceTouchData (as sceTouchPeek would hand it, but synthesised
 *    here from a fixed seed so runs repeat exactly) and it tracks raw per-finger press/move/lift
 *    primitives, each a packed (panel << 8 | touch id) "primitive ID".
 *  - "TouchRecognizer": a caller-allocated opaque object (>= 0x969 bytes; GESTURE_REC_SIZE here)
 *    that turns the primitives the Primitive layer already cached into one higher-level gesture
 *    per instance, chosen at Create time by `type` (1, 2, 4, 8 or 0x10 -- the exact gesture each
 *    number names, e.g. tap/hold/drag/flick/rotate, isn't confirmed, only that they take distinct
 *    code paths with distinct default tuning parameters; 2 gets no defaults applied at all).
 *    sceSystemGestureUpdateTouchRecognizer(recognizer) must be called after
 *    UpdatePrimitiveTouchRecognizer each "frame" to pick up what was just cached.
 *
 * SceTouchData matches VitaSDK's psp2/touch.h (timeStamp, status, reportNum, report[8]); defined
 * locally since this scenario is compiled standalone (no psp2 headers) on both sides.
 */
#ifndef SYSTEMGESTURE_TEST_SCENARIO_H
#define SYSTEMGESTURE_TEST_SCENARIO_H

#include <stdint.h>
#include <string.h>
#include "trace.h"

typedef struct {
	uint8_t id, force;
	int16_t x, y;
	uint8_t reserved[8];
	uint16_t info;
} GestureTouchReport;   /* 0x10 bytes */

typedef struct {
	uint64_t timeStamp;
	uint32_t status;
	uint32_t reportNum;
	GestureTouchReport report[8];
} GestureTouchData;   /* 0x90 bytes: SceTouchData */

typedef struct {
	int (*CreateTouchRecognizer)(void *recognizer, uint32_t type, uint32_t port, int16_t *rect, uint32_t *param);
	int (*ResetTouchRecognizer)(void *recognizer);
	int (*UpdateTouchRecognizerRectangle)(void *recognizer, int16_t *rect);
	int (*UpdateTouchRecognizer)(void *recognizer);
	int (*GetTouchRecognizerInformation)(void *recognizer, void *info);
	int (*GetTouchEvents)(void *recognizer, void *events, uint32_t numEvents, uint32_t *eventNum);
	int (*GetTouchEventsCount)(void *recognizer);
	int (*GetTouchEventByIndex)(void *recognizer, uint32_t index, void *event);
	int (*GetTouchEventByEventID)(void *recognizer, int eventId, void *event);
	int (*InitializePrimitiveTouchRecognizer)(void);
	int (*FinalizePrimitiveTouchRecognizer)(void);
	int (*ResetPrimitiveTouchRecognizer)(void);
	int (*UpdatePrimitiveTouchRecognizer)(const void *front, const void *back);
	int (*GetPrimitiveTouchEvents)(void *events, uint32_t numEvents, int *eventNum);
	uint32_t (*GetPrimitiveTouchEventsCount)(void);
	int (*GetPrimitiveTouchEventByIndex)(uint32_t index, void *event);
	int (*GetPrimitiveTouchEventByPrimitiveID)(int16_t primitiveId, void *event);
} GestureApi;

enum { N_CREATE, N_RESET, N_UPDATE_RECT, N_UPDATE, N_INFO, N_EVENTS, N_EVENTS_COUNT, N_EVENT_BY_INDEX,
       N_EVENT_BY_ID, N_PRIM_INIT, N_PRIM_FINALIZE, N_PRIM_RESET, N_PRIM_UPDATE, N_PRIM_EVENTS,
       N_PRIM_EVENTS_COUNT, N_PRIM_EVENT_BY_INDEX, N_PRIM_EVENT_BY_ID, N_NOTE };
static const char *const GESTURE_NAMES[] = {
	"CREATE", "RESET", "UPDATE_RECT", "UPDATE", "INFO", "EVENTS", "EVENTS_COUNT", "EVENT_BY_INDEX",
	"EVENT_BY_ID", "PRIM_INIT", "PRIM_FINALIZE", "PRIM_RESET", "PRIM_UPDATE", "PRIM_EVENTS",
	"PRIM_EVENTS_COUNT", "PRIM_EVENT_BY_INDEX", "PRIM_EVENT_BY_ID", "NOTE",
};

#define GESTURE_REC_SIZE 0x980      /* a TouchRecognizer instance: >= 0x969 bytes used */
#define GESTURE_NREC 5              /* one per type: 1, 2, 4, 8, 0x10 */
#define GESTURE_EVENTS_CAP 8        /* events requested per poll (recognizer events are 0x90 each) */
#define GESTURE_PRIM_EVENTS_CAP 8   /* primitive events requested per poll (0x60 each) */
#define GESTURE_MEM (GESTURE_NREC * GESTURE_REC_SIZE + GESTURE_EVENTS_CAP * 0x90 + \
                     GESTURE_PRIM_EVENTS_CAP * 0x60 + 0x40 + 2 * sizeof(GestureTouchData))

typedef struct {
	Trace *t;
	const GestureApi *api;
	uint8_t *mem;        /* GESTURE_MEM bytes, carved up below */
	uint8_t *rec[GESTURE_NREC];
	uint8_t *events;      /* GESTURE_EVENTS_CAP * 0x90 */
	uint8_t *primEvents;  /* GESTURE_PRIM_EVENTS_CAP * 0x60 */
	uint8_t *info;        /* 0x40 (32 used) */
	GestureTouchData *front, *back;
	uint64_t now;          /* running timestamp (us), advanced by frame() */
} GestureHarness;

static const uint32_t GESTURE_TYPES[GESTURE_NREC] = { 1, 2, 4, 8, 0x10 };

static GestureHarness *gh;
static void rec(uint16_t name, int r, const void *d, uint32_t n) { trace(gh->t, name, r, d, n); }
static void rec1(uint16_t name, int r) { rec(name, r, 0, 0); }

/* An event's word 6 (byte 0x18) is the primitive this slot is bound to, +1 (0 if none): a plain
 * 32-bit store for a single-finger gesture, but packed as two 16-bit halves -- (finger0 + 1) |
 * (finger1 + 1) << 16 -- for a type that tracks two fingers per slot (see the firmware's
 * FUN_81001872/FUN_810012d6/FUN_81001ea2). Console testing (golden/ vs an emulator dual-run) found
 * this word still carrying a stale primitive-pool slot's generation from a *previous* match this
 * same recognizer slot was bound to, read back before anything this run writes it again -- not
 * cleared by ResetTouchRecognizer (which only clears a slot's first 0x40 bytes) and not part of
 * the event's public meaning for a single-finger gesture (a real app has no way to tell this
 * apart from the slot's uninitialised memory). Zeroed here so the trace reflects only what every
 * caller can rely on. */
static void gesture_mask_events(void *events, int n) {
	for (int i = 0; i < n; i++) {
		uint8_t *e = (uint8_t *)events + i * 0x90;
		memset(e + 0x18, 0, 4);
		memset(e + 0x3c, 0, 4);   /* likewise: a type-specific slot field this type never sets */
	}
}

/* poll every way to read a recognizer's events, tracing each result (count, the array, by index
 * and by the event ID GetTouchEventByIndex just reported for index 0) */
static void gesture_poll(void *r) {
	const GestureApi *a = gh->api;
	int cnt = a->GetTouchEventsCount(r);
	rec1(N_EVENTS_COUNT, cnt);
	memset(gh->events, 0xcc, GESTURE_EVENTS_CAP * 0x90);
	uint32_t n = 0xdddddddd;
	int ret = a->GetTouchEvents(r, gh->events, GESTURE_EVENTS_CAP, &n);
	gesture_mask_events(gh->events, GESTURE_EVENTS_CAP);
	uint8_t buf[4 + GESTURE_EVENTS_CAP * 0x90];
	memcpy(buf, &n, 4); memcpy(buf + 4, gh->events, GESTURE_EVENTS_CAP * 0x90);
	rec(N_EVENTS, ret, buf, sizeof(buf));
	uint8_t ev[0x90]; int32_t eventId = -1;
	memset(ev, 0xcc, sizeof(ev));
	ret = a->GetTouchEventByIndex(r, 0, ev);
	gesture_mask_events(ev, 1);
	rec(N_EVENT_BY_INDEX, ret, ev, sizeof(ev));
	if (ret == 0) memcpy(&eventId, ev, 4);
	memset(ev, 0xcc, sizeof(ev));
	ret = a->GetTouchEventByEventID(r, eventId, ev);
	gesture_mask_events(ev, 1);
	rec(N_EVENT_BY_ID, ret, ev, sizeof(ev));
}

static void gesture_poll_primitive(void) {
	const GestureApi *a = gh->api;
	uint32_t cnt = a->GetPrimitiveTouchEventsCount();
	rec1(N_PRIM_EVENTS_COUNT, (int)cnt);
	memset(gh->primEvents, 0xcc, GESTURE_PRIM_EVENTS_CAP * 0x60);
	int n = -1;
	int ret = a->GetPrimitiveTouchEvents(gh->primEvents, GESTURE_PRIM_EVENTS_CAP, &n);
	uint8_t buf[4 + GESTURE_PRIM_EVENTS_CAP * 0x60];
	memcpy(buf, &n, 4); memcpy(buf + 4, gh->primEvents, GESTURE_PRIM_EVENTS_CAP * 0x60);
	rec(N_PRIM_EVENTS, ret, buf, sizeof(buf));
	uint8_t ev[0x60]; int16_t primId = -1;
	memset(ev, 0xcc, sizeof(ev));
	ret = a->GetPrimitiveTouchEventByIndex(0, ev);
	rec(N_PRIM_EVENT_BY_INDEX, ret, ev, sizeof(ev));
	if (ret == 0) memcpy(&primId, ev, 2);
	memset(ev, 0xcc, sizeof(ev));
	ret = a->GetPrimitiveTouchEventByPrimitiveID(primId, ev);
	rec(N_PRIM_EVENT_BY_ID, ret, ev, sizeof(ev));
}

/* build one SceTouchData with n points starting at (x,y), spaced dx,dy apart, advance gh->now by
 * dt_us and push it through UpdatePrimitiveTouchRecognizer (back panel left out: NULL) */
static void gesture_touch(int n, int x, int y, int dx, int dy, uint32_t dt_us) {
	gh->now += dt_us;
	memset(gh->front, 0, sizeof(*gh->front));
	gh->front->timeStamp = gh->now;
	gh->front->reportNum = n;
	for (int i = 0; i < n && i < 8; i++) {
		gh->front->report[i].id = i;
		gh->front->report[i].force = 1;
		gh->front->report[i].x = x + i * dx;
		gh->front->report[i].y = y + i * dy;
	}
	int r = gh->api->UpdatePrimitiveTouchRecognizer(n ? gh->front : 0, 0);
	rec1(N_PRIM_UPDATE, r);
}

/* two-finger touch (for pinch/rotate-shaped recognizers): independent positions per finger */
static void gesture_touch2(int x0, int y0, int x1, int y1, uint32_t dt_us) {
	gh->now += dt_us;
	memset(gh->front, 0, sizeof(*gh->front));
	gh->front->timeStamp = gh->now;
	gh->front->reportNum = 2;
	gh->front->report[0] = (GestureTouchReport){ .id = 0, .force = 1, .x = x0, .y = y0 };
	gh->front->report[1] = (GestureTouchReport){ .id = 1, .force = 1, .x = x1, .y = y1 };
	int r = gh->api->UpdatePrimitiveTouchRecognizer(gh->front, 0);
	rec1(N_PRIM_UPDATE, r);
}

/* a touch lifted (reportNum 0) */
static void gesture_release(uint32_t dt_us) {
	gh->now += dt_us;
	memset(gh->front, 0, sizeof(*gh->front));
	gh->front->timeStamp = gh->now;
	int r = gh->api->UpdatePrimitiveTouchRecognizer(gh->front, 0);
	rec1(N_PRIM_UPDATE, r);
}

/* drive every created recognizer's UpdateTouchRecognizer + poll, after a primitive update */
static void gesture_update_all(void) {
	for (int i = 0; i < GESTURE_NREC; i++) {
		int r = gh->api->UpdateTouchRecognizer(gh->rec[i]);
		rec1(N_UPDATE, r);
		gesture_poll(gh->rec[i]);
	}
	gesture_poll_primitive();
}

/* like gesture_update_all, but only the one recognizer a test segment is about: a real caller
 * only updates the recognizers it cares about for a given touch stream, and updating the other
 * four against touches never meant for them leaves them tracking irrelevant, unfinished
 * multi-touch "groups" (SceSystemGestureUpdateTouchRecognizer tracks up to two fingers per type)
 * -- internal bookkeeping with no public meaning, but it does make every byte of the internal
 * state history-dependent, which a single recognizer's own dedicated stream avoids */
static void gesture_update_one(int i) {
	int r = gh->api->UpdateTouchRecognizer(gh->rec[i]);
	rec1(N_UPDATE, r);
	gesture_poll(gh->rec[i]);
	gesture_poll_primitive();
}

__attribute__((noinline, used)) static void gesture_run(const GestureApi *a, GestureHarness *h) {
	gh = h; h->api = a;
	memset(h->mem, 0, GESTURE_MEM);
	uint8_t *p = h->mem;
	for (int i = 0; i < GESTURE_NREC; i++) { h->rec[i] = p; p += GESTURE_REC_SIZE; }
	h->events = p; p += GESTURE_EVENTS_CAP * 0x90;
	h->primEvents = p; p += GESTURE_PRIM_EVENTS_CAP * 0x60;
	h->info = p; p += 0x40;
	h->front = (GestureTouchData *)p; p += sizeof(GestureTouchData);
	h->back = (GestureTouchData *)p; p += sizeof(GestureTouchData);
	h->now = 1000000000ull;   /* an arbitrary epoch, like sceKernelGetProcessTimeWide */

	/* argument checks and "not initialized" errors, before anything is created */
	rec1(N_CREATE, a->CreateTouchRecognizer(0, 1, 0, 0, 0));                     /* NULL recognizer */
	rec1(N_CREATE, a->CreateTouchRecognizer(h->rec[0], 0, 0, 0, 0));             /* type 0 */
	rec1(N_CREATE, a->CreateTouchRecognizer(h->rec[0], 3, 0, 0, 0));             /* type 3 */
	rec1(N_CREATE, a->CreateTouchRecognizer(h->rec[0], 0x20, 0, 0, 0));          /* type 0x20 */
	rec1(N_RESET, a->ResetTouchRecognizer(0));
	rec1(N_RESET, a->ResetTouchRecognizer(h->rec[0]));                          /* never created */
	rec1(N_UPDATE, a->UpdateTouchRecognizer(h->rec[0]));
	rec1(N_UPDATE, a->UpdateTouchRecognizer(0));
	rec1(N_UPDATE_RECT, a->UpdateTouchRecognizerRectangle(h->rec[0], 0));
	memset(h->info, 0xcc, 0x40);
	rec(N_INFO, a->GetTouchRecognizerInformation(h->rec[0], h->info), h->info, 0x18);
	rec1(N_INFO, a->GetTouchRecognizerInformation(0, h->info));
	rec1(N_INFO, a->GetTouchRecognizerInformation(h->rec[0], 0));
	rec1(N_EVENTS_COUNT, a->GetTouchEventsCount(h->rec[0]));
	rec1(N_EVENTS, a->GetTouchEvents(h->rec[0], h->events, GESTURE_EVENTS_CAP, (uint32_t[]){ 0 }));
	rec1(N_PRIM_RESET, a->ResetPrimitiveTouchRecognizer());
	rec1(N_PRIM_EVENTS_COUNT, (int)a->GetPrimitiveTouchEventsCount());
	rec1(N_PRIM_UPDATE, a->UpdatePrimitiveTouchRecognizer(0, 0));               /* both NULL */
	rec1(N_PRIM_FINALIZE, a->FinalizePrimitiveTouchRecognizer());               /* never initialized */

	/* bring the primitive layer up */
	rec1(N_PRIM_INIT, a->InitializePrimitiveTouchRecognizer());
	rec1(N_PRIM_INIT, a->InitializePrimitiveTouchRecognizer());                 /* twice */

	/* create one recognizer per type: explicit rectangles (front panel's real extent, from
	 * sceTouchGetPanelInfo on PCH-2000: -2/1919 x, -2/1087 y) except rec[0], which asks the
	 * library to look it up itself (rect == NULL) -- exercises both paths */
	int16_t rect[4] = { -2, -2, 1921, 1089 };
	uint32_t tapParam = 2;                 /* type 1: up to 2 simultaneous touches */
	uint32_t dragParam[2] = { 500, 7 };    /* type 4: two tuning words */
	for (int i = 0; i < GESTURE_NREC; i++) {
		uint32_t *param = i == 0 ? &tapParam : i == 2 ? dragParam : 0;
		int r = a->CreateTouchRecognizer(h->rec[i], GESTURE_TYPES[i], 0, i == 0 ? 0 : rect, param);
		rec1(N_CREATE, r);
	}
	rec1(N_CREATE, a->CreateTouchRecognizer(h->rec[0], GESTURE_TYPES[0], 0, rect, 0));  /* twice */
	memset(h->info, 0xcc, 0x40);
	rec(N_INFO, a->GetTouchRecognizerInformation(h->rec[2], h->info), h->info, 0x18);
	rec1(N_UPDATE_RECT, a->UpdateTouchRecognizerRectangle(h->rec[1], rect));
	rec1(N_UPDATE_RECT, a->UpdateTouchRecognizerRectangle(h->rec[1], 0));

	/* a short tap: down, released two frames later (rec[0], type 1) */
	gesture_touch(1, 400, 300, 0, 0, 16000);
	gesture_update_one(0);
	gesture_touch(1, 400, 300, 0, 0, 16000);
	gesture_update_one(0);
	gesture_release(16000);
	gesture_update_one(0);

	/* a hold: down, stationary for many frames (~1.5 s) (rec[1], type 2) */
	gesture_touch(1, 600, 500, 0, 0, 16000);
	for (int i = 0; i < 90; i++) {
		gesture_touch(1, 600, 500, 0, 0, 16000);
		if (i % 15 == 0) gesture_update_one(1);
	}
	gesture_release(16000);
	gesture_update_one(1);

	/* a drag: down, moves steadily, released (rec[2], type 4) */
	gesture_touch(1, 100, 900, 0, 0, 16000);
	gesture_update_one(2);
	for (int i = 1; i <= 20; i++) {
		gesture_touch(1, 100 + i * 15, 900 - i * 5, 0, 0, 16000);
		gesture_update_one(2);
	}
	gesture_release(16000);
	gesture_update_one(2);

	/* a flick: down, a few fast moves, released quickly (rec[3], type 8) */
	gesture_touch(1, 1500, 200, 0, 0, 16000);
	for (int i = 1; i <= 4; i++) {
		gesture_touch(1, 1500 - i * 60, 200 + i * 10, 0, 0, 8000);
	}
	gesture_release(8000);
	gesture_update_one(3);

	/* two fingers: pinch apart, then rotate around the midpoint, then released together
	 * (rec[4], type 0x10) */
	gesture_touch2(900, 500, 1000, 520, 16000);
	gesture_update_one(4);
	for (int i = 1; i <= 15; i++) {
		gesture_touch2(900 - i * 20, 500 - i * 5, 1000 + i * 20, 520 + i * 5, 16000);
		if (i % 5 == 0) gesture_update_one(4);
	}
	for (int i = 1; i <= 15; i++) {
		int a0 = i * 4, dx = (i * 3), dy = (i * 2);
		gesture_touch2(650 - dx, 450 + dy + a0 / 4, 1300 + dx, 600 - dy - a0 / 4, 16000);
		if (i % 5 == 0) gesture_update_one(4);
	}
	gesture_release(16000);
	gesture_update_one(4);

	/* now that each recognizer has its own settled history, a few frames of every recognizer
	 * against the same stream, for cross-recognizer coverage */
	gesture_touch(1, 700, 700, 0, 0, 16000);
	gesture_update_all();
	gesture_touch(1, 715, 690, 0, 0, 16000);
	gesture_update_all();
	gesture_release(16000);
	gesture_update_all();

	/* a point on the back panel too (rec[0]'s port is front/0, so this shouldn't affect it) */
	gh->now += 16000;
	memset(h->back, 0, sizeof(*h->back));
	h->back->timeStamp = h->now;
	h->back->reportNum = 1;
	h->back->report[0] = (GestureTouchReport){ .id = 0, .force = 1, .x = 200, .y = 200 };
	memset(h->front, 0, sizeof(*h->front));
	h->front->timeStamp = h->now;
	int r = a->UpdatePrimitiveTouchRecognizer(h->front, h->back);
	rec1(N_PRIM_UPDATE, r);
	gesture_update_all();

	/* reset mid-flight and tear down */
	rec1(N_RESET, a->ResetTouchRecognizer(h->rec[3]));
	rec1(N_RESET, a->ResetTouchRecognizer(h->rec[3]));              /* twice */
	rec1(N_PRIM_RESET, a->ResetPrimitiveTouchRecognizer());
	gesture_poll_primitive();
	rec1(N_PRIM_FINALIZE, a->FinalizePrimitiveTouchRecognizer());
	rec1(N_PRIM_EVENTS_COUNT, (int)a->GetPrimitiveTouchEventsCount());   /* finalized: not initialized */
	rec1(N_UPDATE, a->UpdateTouchRecognizer(h->rec[0]));                 /* primitive layer is down */

	rec(N_NOTE, 0, "end", 4);
}

#endif
