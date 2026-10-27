/*
 * Call traces: a scenario appends one record per API call, and two traces (firmware vs rebuild)
 * are compared record by record. Record: uint16 step, uint16 name index, int32 ret, uint32 len,
 * then len bytes. Pointers never go in a trace, only whether they were NULL.
 */
#ifndef REBUILD_TRACE_H
#define REBUILD_TRACE_H

#include <stdint.h>
#include <string.h>

typedef struct {
	uint8_t *buf;
	uint32_t len, cap;
	uint16_t step;
	/* optional: called after every record with the bytes added, so a crash leaves a usable trace */
	void (*flush)(const void *p, uint32_t n);
} Trace;

static inline void trace_raw(Trace *t, const void *p, uint32_t n) {
	if (t->len + n > t->cap) n = t->len < t->cap ? t->cap - t->len : 0;
	if (n && p) memcpy(t->buf + t->len, p, n);
	t->len += n;
}

static inline void trace(Trace *t, uint16_t name, int32_t ret, const void *data, uint32_t n) {
	uint32_t start = t->len;
	uint16_t h[2] = { t->step++, name };
	trace_raw(t, h, 4);
	trace_raw(t, &ret, 4);
	trace_raw(t, &n, 4);
	trace_raw(t, data, n);
	if (t->flush) t->flush(t->buf + start, t->len - start);
}

static inline int32_t trace_f2i(float f) { int32_t i; memcpy(&i, &f, 4); return i; }

#endif
