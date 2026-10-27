/* Vita system calls on the host: process time, memory blocks, the real-time clock (SceRtc) and the C library's time. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "hle.h"

/* ---- kernel: time, memory blocks (threads and locks: kernel.c) */
/* the game's clock: display.c's guest_time_us (the host's monotonic clock, or the fixed test clock) */
static void h_time_wide(Cpu *c) { uint64_t t = guest_time_us(); c->r[0] = (uint32_t)t; c->r[1] = (uint32_t)(t >> 32); }
static void h_time_low(Cpu *c) { RET(guest_time_us()); }
static void h_time(Cpu *c) { uint64_t t = guest_time_us(); if (A0) WR64(A0, t); RET(0); }
/* uid -> base, size; size 0 is a free slot (never used, or freed: the UID/slot is up for reuse). This used to be a fixed
 * blocks[256] with nblocks++ never checked against it: a long session making more than 256 (freed or not) allocations over
 * its lifetime silently overflowed into whatever static data followed it. */
#define MAX_BLOCKS 4096
static uint32_t blocks[MAX_BLOCKS][2];
static uint32_t block_type[MAX_BLOCKS];   /* the SceKernelMemBlockType it was allocated with (GetMemBlockInfoByAddr reports it) */
static int nblocks;
static pthread_mutex_t block_lock = PTHREAD_MUTEX_INITIALIZER;   /* blocks[] and holes[]: games allocate on any thread */
/* a freed block's guest memory, for the next block that fits (best fit, split; neighbours merged). guest_alloc is a bump
 * allocator with no free, and it used to be the only source: Killzone takes ~200 MB of new blocks at every level load and
 * movie (its render-target pools, VM heaps) and frees the old ones, so a long session walked the 1.75 GB heap towards
 * "guest heap exhausted" -- its level 1 -> 2 transition on Linux had reached 0x3b5..., five or six loads from the end. A
 * console's kernel reuses the memory the same way. Bases and sizes are page-aligned (sizes rounded up: the next block
 * starts at a page boundary past the end anyway). */
#define MAX_HOLES 1024
static uint32_t holes[MAX_HOLES][2];
static int nholes;
static uint32_t hole_take(uint32_t size) {
	int best = -1;
	for (int i = 0; i < nholes; i++) if (holes[i][1] >= size && (best < 0 || holes[i][1] < holes[best][1])) best = i;
	if (best < 0) return 0;
	const uint32_t base = holes[best][0];
	if (holes[best][1] == size) holes[best][0] = holes[--nholes][0], holes[best][1] = holes[nholes][1];
	else holes[best][0] += size, holes[best][1] -= size;
	return base;
}
static void hole_put(uint32_t base, uint32_t size) {
	for (int i = 0; i < nholes; i++) {   /* merged with a neighbour on either side (then the merged hole is put back) */
		if (holes[i][0] + holes[i][1] == base || base + size == holes[i][0]) {
			if (holes[i][0] < base) base = holes[i][0];
			size += holes[i][1];
			holes[i][0] = holes[--nholes][0]; holes[i][1] = holes[nholes][1];
			hole_put(base, size); return;
		}
	}
	if (nholes < MAX_HOLES) { holes[nholes][0] = base; holes[nholes][1] = size; nholes++; }   /* (else that memory stays unused) */
}
static uint32_t page_up(uint32_t n) { return (n + 4095u) & ~4095u; }
static void h_alloc_block(Cpu *c) {   /* (name, type, size, opt) */
	if (!A2) { RET(0x80020001u); return; }   /* an empty block fails, as on the console (illegal size) */
	pthread_mutex_lock(&block_lock);
	int slot = -1;
	for (int i = 0; i < nblocks; i++) if (!blocks[i][1]) { slot = i; break; }   /* a freed slot first */
	if (slot < 0) { if (nblocks >= MAX_BLOCKS) { pthread_mutex_unlock(&block_lock); RET(0x80020001u); return; } slot = nblocks++; }
	uint32_t size = A2, base = hole_take(page_up(size));
	const int reused = base != 0;
	if (!base) base = guest_alloc(size, 4096);
	blocks[slot][0] = base; blocks[slot][1] = size; block_type[slot] = A1;
	pthread_mutex_unlock(&block_lock);
	memset(G(base), 0, size);
	fprintf(stderr, "    memblock '%s' %#x bytes at %#x%s\n", (char *)G(A0), size, base, reused ? " (reused)" : "");
	RET(0x10000 + slot);
}
static void h_free_block(Cpu *c) {   /* (uid) */
	uint32_t i = A0 - 0x10000;
	pthread_mutex_lock(&block_lock);
	if (i >= (uint32_t)nblocks || !blocks[i][1]) { pthread_mutex_unlock(&block_lock); RET(0x80020001u); return; }
	hole_put(blocks[i][0], page_up(blocks[i][1]));
	blocks[i][1] = 0;
	pthread_mutex_unlock(&block_lock);
	RET(0);
}
/* sceKernelFindMemBlockByAddr(address, size): the uid of the block holding [address, address+size)
 * (Killzone asks for the block behind its render-data pool before it gives it to GXM). The address
 * itself must lie inside: with size 0 an address just past one block's end is the start of the next
 * -- Killzone's level load asks for its volatile render-target pool, which sits right after the
 * fixed one, and was given (and freed) the fixed pool, leaving its targets at offsets from 0 */
static void h_find_block_by_addr(Cpu *c) {
	for (int i = 0; i < nblocks; i++) {
		uint32_t off = A0 - blocks[i][0];
		if (blocks[i][1] && A0 >= blocks[i][0] && off < blocks[i][1] && A1 <= blocks[i][1] - off) { RET(0x10000 + i); return; }
	}
	RET(0x8002000Bu);   /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
}
static void h_block_base(Cpu *c) {
	uint32_t i = A0 - 0x10000;
	if (i >= (uint32_t)nblocks || !blocks[i][1]) { RET(0x80020001); return; }
	WR32(A1, blocks[i][0]); RET(0);
}
/* (SceKernelFreeMemorySizeInfo *info): no real budget is tracked (guest_alloc just grows the
 * arena), so a generous fixed figure -- enough that a game's own low-memory logic doesn't trip */
static void h_free_mem_size(Cpu *c) {
	uint32_t sz = RD32(A0) ? RD32(A0) : 0x10, free_bytes = 200u << 20;
	memset(G(A0), 0, sz); WR32(A0, sz);
	if (sz >= 8) WR32(A0 + 4, free_bytes);
	if (sz >= 12) WR32(A0 + 8, free_bytes);
	if (sz >= 16) WR32(A0 + 12, free_bytes);
	RET(0);
}

/* ---- SceRtc. A tick is microseconds since 0001-01-01 00:00:00 UTC (proleptic Gregorian); a
 * SceDateTime is u16 year, month, day, hour, minute, second, then u32 microsecond (16 bytes). The
 * host has one time zone, UTC with no summer time (SystemParamGetInt reports zone 0), so every
 * "local" form equals UTC -- one clock, however a game asks for it. Every reading of the current
 * time goes through rtc_now: on the fixed clock (RECOMP_CLOCK=fixed) it is FIXED_EPOCH_S plus the
 * game's deterministic guest time, so a test run sees the same dates every time. */
#define FIXED_EPOCH_S 1791244800ull   /* 2026-10-06 00:00:00 UTC: the fixed clock's calendar date */
#define TICK_UNIX 62135596800000000ull       /* 1970-01-01 as a tick */
#define TICK_FILETIME 50491123200000000ull   /* 1601-01-01, Win32 FILETIME's epoch, as a tick */
#define US_DAY 86400000000ull
#define RTC_EINVAL 0x80251000u   /* SCE_RTC_ERROR_INVALID_VALUE */
#define RTC_EPTR 0x80251001u     /* SCE_RTC_ERROR_INVALID_POINTER */
#define RTC_EPARSE 0x80251080u   /* SCE_RTC_ERROR_BAD_PARSE; YEAR..MICROSECOND follow it (0x81..0x87) */
static uint64_t rtc_now(void) {
	if (guest_clock_fixed()) return FIXED_EPOCH_S * 1000000 + guest_time_us() + TICK_UNIX;
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 + TICK_UNIX;
}
/* days since 1970-01-01 of a Gregorian date, and back (H. Hinnant's era arithmetic: exact for any
 * year, no tables); month/day outside their range just carry, as mktime's normalisation needs */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
	y += (m - 1) >= 0 ? (m - 1) / 12 : -((12 - m) / 12); m = ((m - 1) % 12 + 12) % 12 + 1;
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
	int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = yoe + era * 400 + (*m <= 2);
}
static int is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in_month(int64_t y, int m) { static const int n[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; return m == 2 && is_leap(y) ? 29 : n[m - 1]; }
/* SceDateTime <-> tick: the conversions themselves don't validate (as on the console: garbage in,
 * garbage tick out); sceRtcCheckValid is the check */
static uint64_t dt_to_tick(uint32_t dt) {
	int64_t days = days_from_civil(RD16(dt), RD16(dt + 2), RD16(dt + 4)) + 719162;
	return (uint64_t)days * US_DAY + ((uint64_t)RD16(dt + 6) * 3600 + (uint64_t)RD16(dt + 8) * 60 + RD16(dt + 10)) * 1000000 + RD32(dt + 12);
}
static void tick_to_dt(uint32_t dt, uint64_t tick) {
	int64_t y; int m, d; uint64_t us = tick % US_DAY;
	civil_from_days((int64_t)(tick / US_DAY) - 719162, &y, &m, &d);
	WR16(dt, (uint16_t)y); WR16(dt + 2, (uint16_t)m); WR16(dt + 4, (uint16_t)d);
	WR16(dt + 6, (uint16_t)(us / 3600000000ull)); WR16(dt + 8, (uint16_t)(us / 60000000 % 60)); WR16(dt + 10, (uint16_t)(us / 1000000 % 60));
	WR32(dt + 12, (uint32_t)(us % 1000000));
}
static uint32_t dt_check(uint32_t dt) {   /* sceRtcCheckValid's order: year, month, day, then the clock */
	uint32_t y = RD16(dt), m = RD16(dt + 2), d = RD16(dt + 4);
	if (y < 1 || y > 9999) return 0x80251081u;
	if (m < 1 || m > 12) return 0x80251082u;
	if (d < 1 || (int)d > days_in_month(y, (int)m)) return 0x80251083u;
	if (RD16(dt + 6) > 23) return 0x80251084u;
	if (RD16(dt + 8) > 59) return 0x80251085u;
	if (RD16(dt + 10) > 59) return 0x80251086u;
	if (RD32(dt + 12) > 999999) return 0x80251087u;
	return 0;
}
static void h_rtc_tick(Cpu *c) { if (!A0) { RET(RTC_EPTR); return; } WR64(A0, rtc_now()); RET(0); }   /* (SceRtcTick *) */
/* the network clock: the console keeps it adjusted from the network; the host's clock is the one clock */
static void h_rtc_network_tick(Cpu *c) { h_rtc_tick(c); }
static void h_rtc_resolution(Cpu *c) { RET(1000000); }   /* ticks per second */
static void h_rtc_clock(Cpu *c) {   /* (SceDateTime *, time zone in minutes): now, in that zone */
	if (!A0) { RET(RTC_EPTR); return; }
	tick_to_dt(A0, rtc_now() + (uint64_t)((int64_t)(int32_t)A1 * 60000000)); RET(0);
}
static void h_rtc_clock_local(Cpu *c) { if (!A0) { RET(RTC_EPTR); return; } tick_to_dt(A0, rtc_now()); RET(0); }
static void h_rtc_utc_local(Cpu *c) { if (!A0 || !A1) { RET(RTC_EPTR); return; } WR64(A1, RD64(A0)); RET(0); }   /* (from, to): UTC = local */
static void h_rtc_check(Cpu *c) { RET(A0 ? dt_check(A0) : RTC_EPTR); }
static void h_rtc_leap(Cpu *c) { RET((int32_t)A0 < 0 ? 0x80251081u : (uint32_t)is_leap((int32_t)A0)); }
static void h_rtc_days_in_month(Cpu *c) { RET((int32_t)A0 < 0 ? 0x80251081u : A1 < 1 || A1 > 12 ? 0x80251082u : (uint32_t)days_in_month((int32_t)A0, (int)A1)); }
static void h_rtc_day_of_week(Cpu *c) {   /* (year, month, day) -> 0 Sunday .. 6 Saturday */
	int32_t y = (int32_t)A0, m = (int32_t)A1, d = (int32_t)A2;
	if (y < 1 || y > 9999) { RET(0x80251081u); return; }
	if (m < 1 || m > 12) { RET(0x80251082u); return; }
	if (d < 1 || d > days_in_month(y, m)) { RET(0x80251083u); return; }
	RET((uint32_t)(((days_from_civil(y, m, d) + 4) % 7 + 7) % 7));   /* 1970-01-01 was a Thursday */
}
static void h_rtc_get_tick(Cpu *c) { if (!A0 || !A1) { RET(RTC_EPTR); return; } WR64(A1, dt_to_tick(A0)); RET(0); }   /* (SceDateTime *, SceRtcTick *) */
static void h_rtc_set_tick(Cpu *c) { if (!A0 || !A1) { RET(RTC_EPTR); return; } tick_to_dt(A0, RD64(A1)); RET(0); }   /* (SceDateTime *, const SceRtcTick *) */
static void h_rtc_compare(Cpu *c) { uint64_t a = RD64(A0), b = RD64(A1); RET(a < b ? -1 : a > b); }
/* time_t is 32 bits on the Vita (SetTime_t's second argument is one register); read unsigned, 1970-2106 */
static void h_rtc_set_time_t(Cpu *c) { if (!A0) { RET(RTC_EPTR); return; } tick_to_dt(A0, (uint64_t)A1 * 1000000 + TICK_UNIX); RET(0); }
static void h_rtc_get_time_t(Cpu *c) { if (!A0 || !A1) { RET(RTC_EPTR); return; } WR32(A1, (uint32_t)((int64_t)(dt_to_tick(A0) - TICK_UNIX) / 1000000)); RET(0); }
/* (SceDateTime *, SceUInt64): the 64-bit argument takes the even register pair r2:r3 (AAPCS), r1 is padding */
static void h_rtc_set_time64(Cpu *c) { if (!A0) { RET(RTC_EPTR); return; } tick_to_dt(A0, ((uint64_t)A3 << 32 | A2) * 1000000 + TICK_UNIX); RET(0); }
/* sceRtcGetTime64_t(SceDateTime *, SceUInt64 *): seconds since the Unix epoch; libfios2's dearchiver
 * stamps archived files with it */
static void h_rtc_time64(Cpu *c) { if (!A0 || !A1) { RET(RTC_EPTR); return; } WR64(A1, (uint64_t)((int64_t)(dt_to_tick(A0) - TICK_UNIX) / 1000000)); RET(0); }
static void h_rtc_get_win32(Cpu *c) {   /* (SceDateTime *, SceUInt64 *): 100 ns units since 1601 */
	if (!A0 || !A1) { RET(RTC_EPTR); return; }
	uint64_t t = dt_to_tick(A0);
	if (t < TICK_FILETIME) { WR64(A1, 0); RET(RTC_EINVAL); return; }
	WR64(A1, (t - TICK_FILETIME) * 10); RET(0);
}
/* sceRtcTickAdd*(SceRtcTick *out, const SceRtcTick *in, n): a 64-bit n is r2:r3, a 32-bit one r2 */
#define RTC_ADD(name, unit, wide) static void name(Cpu *c) { \
	if (!A0 || !A1) { RET(RTC_EPTR); return; } \
	int64_t n = wide ? (int64_t)((uint64_t)A3 << 32 | A2) : (int64_t)(int32_t)A2; \
	WR64(A0, RD64(A1) + (uint64_t)(n * (int64_t)(unit))); RET(0); }
RTC_ADD(h_rtc_add_ticks, 1, 1)
RTC_ADD(h_rtc_add_us, 1, 1)
RTC_ADD(h_rtc_add_seconds, 1000000, 1)
RTC_ADD(h_rtc_add_minutes, 60000000, 1)
RTC_ADD(h_rtc_add_hours, 3600000000ll, 0)
RTC_ADD(h_rtc_add_days, 86400000000ll, 0)
/* RFC 2822 ("Tue, 06 Oct 2026 00:00:00 +0000") and RFC 3339 ("2026-10-06T00:00:00.00Z", hundredths
 * of a second, as the PSP's formatter writes it) of a UTC tick shown in a zone (minutes east); a
 * null tick is now. The buffer is the game's (the SDK documents 32 bytes for either) */
static void rtc_format(uint32_t out, uint32_t utcp, int32_t tz, int rfc3339) {
	static const char *wd[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *mn[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	uint64_t t = (utcp ? RD64(utcp) : rtc_now()) + (uint64_t)((int64_t)tz * 60000000);
	int64_t y; int m, d; uint64_t us = t % US_DAY, days = t / US_DAY;
	civil_from_days((int64_t)days - 719162, &y, &m, &d);
	int h = (int)(us / 3600000000ull), mi = (int)(us / 60000000 % 60), s = (int)(us / 1000000 % 60);
	int az = tz < 0 ? -tz : tz;
	char buf[64];
	if (rfc3339) {
		int n = snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%02d", (int)y, m, d, h, mi, s, (int)(us % 1000000 / 10000));
		if (tz) snprintf(buf + n, sizeof buf - n, "%c%02d:%02d", tz < 0 ? '-' : '+', az / 60, az % 60); else snprintf(buf + n, sizeof buf - n, "Z");
	} else
		snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d %c%02d%02d", wd[(days + 1) % 7], d, mn[m - 1], (int)y, h, mi, s, tz < 0 ? '-' : '+', az / 60, az % 60);
	memcpy(G(out), buf, strlen(buf) + 1);
}
static void h_rtc_rfc2822(Cpu *c) { if (!A0) { RET(RTC_EPTR); return; } rtc_format(A0, A1, (int32_t)A2, 0); RET(0); }   /* (char *, const SceRtcTick *, zone) */
static void h_rtc_rfc2822_local(Cpu *c) { if (!A0) { RET(RTC_EPTR); return; } rtc_format(A0, A1, 0, 0); RET(0); }
static void h_rtc_rfc3339(Cpu *c) { if (!A0) { RET(RTC_EPTR); return; } rtc_format(A0, A1, (int32_t)A2, 1); RET(0); }
static void h_rtc_rfc3339_local(Cpu *c) { if (!A0) { RET(RTC_EPTR); return; } rtc_format(A0, A1, 0, 1); RET(0); }
/* sceRtcParseRFC3339(SceRtcTick *utc, const char *): "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)"
 * (a space or a lower-case t/z accepted, as RFC 3339 allows), converted to UTC */
static int digits(const char **p, int n, int *v) {
	*v = 0;
	for (int i = 0; i < n; i++, (*p)++) { if (**p < '0' || **p > '9') return 0; *v = *v * 10 + (**p - '0'); }
	return 1;
}
static void h_rtc_parse3339(Cpu *c) {
	if (!A0 || !A1) { RET(RTC_EPTR); return; }
	const char *p = (const char *)G(A1);
	int y, mo, d, h, mi, s, us = 0, tz = 0;
	if (!digits(&p, 4, &y) || *p++ != '-' || !digits(&p, 2, &mo) || *p++ != '-' || !digits(&p, 2, &d)) { RET(RTC_EPARSE); return; }
	if (*p != 'T' && *p != 't' && *p != ' ') { RET(RTC_EPARSE); return; }
	p++;
	if (!digits(&p, 2, &h) || *p++ != ':' || !digits(&p, 2, &mi) || *p++ != ':' || !digits(&p, 2, &s)) { RET(RTC_EPARSE); return; }
	if (*p == '.') {   /* any number of fraction digits; microseconds kept */
		int scale = 100000; p++;
		if (*p < '0' || *p > '9') { RET(RTC_EPARSE); return; }
		for (; *p >= '0' && *p <= '9'; p++) { us += (*p - '0') * scale; scale /= 10; }
	}
	if (*p == 'Z' || *p == 'z') p++;
	else if (*p == '+' || *p == '-') {
		int sign = *p++ == '-' ? -1 : 1, th, tm;
		if (!digits(&p, 2, &th) || *p++ != ':' || !digits(&p, 2, &tm)) { RET(RTC_EPARSE); return; }
		tz = sign * (th * 60 + tm);
	} else { RET(RTC_EPARSE); return; }
	if (y < 1 || mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo) || h > 23 || mi > 59 || s > 59) { RET(RTC_EPARSE); return; }
	int64_t t = (days_from_civil(y, mo, d) + 719162) * (int64_t)US_DAY + ((int64_t)h * 3600 + mi * 60 + s - (int64_t)tz * 60) * 1000000 + us;
	WR64(A0, (uint64_t)t); RET(0);
}
/* the guest addresses (4-aligned) of every live memory block word equal to `value` -- the objects
 * whose vtable pointer is `value`, for verify.c's RECOMP_DATAWATCH_NEW */
int sysmem_scan_u32(uint32_t value, uint32_t *out, int max) {
	int n = 0;
	for (int i = 0; i < nblocks && n < max; i++) if (blocks[i][1]) {
		const uint32_t *w = (const uint32_t *)G(blocks[i][0]);
		for (uint32_t k = 0; k < blocks[i][1] / 4 && n < max; k++) if (w[k] == value) out[n++] = blocks[i][0] + 4 * k;
	}
	return n;
}
/* RECOMP_DUMP_MEM=frame[,frame...]:path (display.c calls this once per presented frame): at each
 * of those frames, every live memory block goes to path.<frame> as {guest address, size} headers
 * each followed by its bytes -- what a level actually loaded (a navigation map, a resource table),
 * and what changes between two frames, searchable offline */
static struct { uint64_t frame; char base[512]; } later[16]; static int nlater;
void sysmem_dump_now(const char *base, uint64_t frame);
void sysmem_dump_later(const char *base, uint64_t frame);
void sysmem_dump_maybe(uint64_t frame) {
	for (int i = 0; i < nlater; i++) if (later[i].frame == frame) sysmem_dump_now(later[i].base, frame);
	static int init; static long at[64]; static int nat; static char base[480];
	if (!init) {
		init = 1;
		const char *e = getenv("RECOMP_DUMP_MEM"), *colon = e ? strchr(e, ':') : 0;
		if (colon) {
			snprintf(base, sizeof base, "%s", colon + 1);
			for (const char *p = e; p < colon && nat < 64; ) { at[nat++] = strtol(p, (char **)&p, 0); if (*p == ',') p++; else break; }
		}
	}
	int hit = 0;
	for (int i = 0; i < nat; i++) if ((uint64_t)at[i] == frame) hit = 1;
	if (!hit) return;
	sysmem_dump_now(base, frame);
}
/* RECOMP_DUMP_MEM_AT_LUA=<line>:path (verify.c's Lua tracer calls this): when a Lua function defined
 * at that line of the mission script is called, dump now and 30 and 300 frames later -- the state
 * at a script event however many frames the session took to get there */
void sysmem_dump_at_lua(int line, uint64_t frame) {
	static int init, want; static char base[480];
	if (!init) {
		init = 1;
		const char *e = getenv("RECOMP_DUMP_MEM_AT_LUA"), *colon = e ? strchr(e, ':') : 0;
		if (colon) { want = atoi(e); snprintf(base, sizeof base, "%s", colon + 1); }
	}
	if (!want || line != want) return;
	char b[512]; snprintf(b, sizeof b, "%s.lua%d", base, line);
	sysmem_dump_now(b, frame);
	sysmem_dump_later(b, frame + 30); sysmem_dump_later(b, frame + 300);
}
void sysmem_dump_later(const char *base, uint64_t frame) {
	if (nlater < 16) { later[nlater].frame = frame; snprintf(later[nlater].base, 512, "%s", base); nlater++; }
}
void sysmem_dump_now(const char *base, uint64_t frame) {
	char path[560]; snprintf(path, sizeof path, "%s.%llu", base, (unsigned long long)frame);
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "RECOMP_DUMP_MEM: cannot write %s\n", path); return; }
	uint64_t total = 0;
	/* RECOMP_DUMP_MEM_RANGES=start-end,...: more ranges first (a module's data and bss) */
	const char *r = getenv("RECOMP_DUMP_MEM_RANGES");
	while (r && *r) {
		unsigned long a = strtoul(r, (char **)&r, 0), b = *r == '-' ? strtoul(r + 1, (char **)&r, 0) : a;
		if (b > a) { uint32_t hdr[2] = { (uint32_t)a, (uint32_t)(b - a) }; fwrite(hdr, 4, 2, f); fwrite(G(a), 1, b - a, f); total += b - a; }
		if (*r != ',') break;
		r++;
	}
	for (int i = 0; i < nblocks; i++) if (blocks[i][1]) {
		uint32_t hdr[2] = { blocks[i][0], blocks[i][1] };
		fwrite(hdr, 4, 2, f); fwrite(G(blocks[i][0]), 1, blocks[i][1], f); total += blocks[i][1];
	}
	fclose(f);
	fprintf(stderr, "RECOMP_DUMP_MEM: frame %llu, %llu bytes of memory blocks to %s\n", (unsigned long long)frame, (unsigned long long)total, path);
}
/* (address, SceKernelMemBlockInfo*: size, mapped base, mapped size, memory type, access, type).
 * The type is the SceKernelMemBlockType the block was allocated with, and the memory type and
 * access are its fields (bits 8-15: 0xd0 cached, 0x80 uncached; bits 4-7: R 4 | W 2 | X 1). Killzone
 * keeps a byte budget per heap (main, CDRAM, phycont) and credits a free to the heap the type names:
 * reported as 0, no heap matched, its counters only ever grew, and 20 s into the campaign start its
 * allocations failed "heap overflow" -- UID 0 blocks, failed render targets, a std::out_of_range throw. */
static void h_block_info_by_addr(Cpu *c) {
	for (int i = 0; i < nblocks; i++) if (blocks[i][1] && A0 >= blocks[i][0] && A0 < blocks[i][0] + blocks[i][1]) {
		uint32_t sz = RD32(A1) ? RD32(A1) : 0x18, t = block_type[i];
		memset(G(A1), 0, sz); WR32(A1, sz); WR32(A1 + 4, blocks[i][0]); WR32(A1 + 8, blocks[i][1]);
		if (sz >= 0x10) WR32(A1 + 0xc, (t >> 8) & 0xff);
		if (sz >= 0x14) WR32(A1 + 0x10, (t >> 4) & 0xf);
		if (sz >= 0x18) WR32(A1 + 0x14, t);
		RET(0); return;
	}
	RET(0x80020001u);
}
/* the C library's time sources (SceProcessmgr): time(*t), gettimeofday(tv, tz), clock() in
 * microseconds -- time and gettimeofday on rtc_now's clock, so the fixed clock covers them too */
static void h_libc_time(Cpu *c) { uint32_t t = (uint32_t)((rtc_now() - TICK_UNIX) / 1000000); if (A0) WR32(A0, t); RET(t); }
static void h_libc_gettimeofday(Cpu *c) {
	uint64_t us = rtc_now() - TICK_UNIX;
	if (A0) { WR32(A0, (uint32_t)(us / 1000000)); WR32(A0 + 4, (uint32_t)(us % 1000000)); }
	if (A1) { WR32(A1, 0); WR32(A1 + 4, 0); }   /* struct timezone: UTC, no summer time */
	RET(0);
}
/* clock() (sceKernelLibcClock): the process's time in microseconds -- on the console, where the
 * game is the only process on its cores, that is elapsed time since start, the same clock as
 * sceKernelGetProcessTime; and it is what the fixed test clock covers. It used to return the
 * host's process CPU time, which with a dozen host threads advances many times faster than the
 * game's clock: Unit 13's Kynapse pathfinder meters its per-frame time slice with clock() (six
 * reads a frame on KynapseThread), saw its budget gone before it had searched anything, and no
 * enemy ever found a path -- they turned to face the player and never walked. */
static void h_libc_clock(Cpu *c) { RET((uint32_t)guest_time_us()); }
/* the kernel half of SceLibc's mktime / localtime_r / gmtime_r. Its struct tm is nine ints -- sec,
 * min, hour, mday, mon (0-11), year, wday, yday, isdst -- with the year in FULL (SceLibc adds 1900
 * on the way in and subtracts it on the way out); time_t is 32 bits. Local time is UTC (the host's
 * one zone), so localtime_r and gmtime_r are the same conversion. */
static void tm_from_secs(uint32_t tm, int64_t t) {
	int64_t days = t >= 0 ? t / 86400 : -((86399 - t) / 86400), sec = t - days * 86400, y; int m, d;
	civil_from_days(days, &y, &m, &d);
	WR32(tm, (uint32_t)(sec % 60)); WR32(tm + 4, (uint32_t)(sec / 60 % 60)); WR32(tm + 8, (uint32_t)(sec / 3600));
	WR32(tm + 12, (uint32_t)d); WR32(tm + 16, (uint32_t)(m - 1)); WR32(tm + 20, (uint32_t)y);
	WR32(tm + 24, (uint32_t)(((days + 4) % 7 + 7) % 7)); WR32(tm + 28, (uint32_t)(days - days_from_civil(y, 1, 1))); WR32(tm + 32, 0);
}
static void h_libc_gmtime(Cpu *c) {   /* (const time_t *, struct tm *) */
	if (!A0 || !A1) { RET(0x80020006u); return; }   /* ILLEGAL_ADDR */
	tm_from_secs(A1, (int64_t)RD32(A0)); RET(0);
}
/* (struct tm *, time_t *, SceUInt64 *): the fields may be out of range (mktime's normalisation:
 * 32 Jan is 1 Feb); the structure comes back normalised with wday/yday filled, isdst 0. The third
 * pointer looks like the 64-bit time (SceLibc gives it eight bytes and ignores it); <0 = unrepresentable */
static void h_libc_mktime(Cpu *c) {
	if (!A0) { RET(0x80020006u); return; }
	int32_t f[6]; for (int i = 0; i < 6; i++) f[i] = (int32_t)RD32(A0 + 4 * i);
	int64_t t = days_from_civil(f[5], (int64_t)f[4] + 1, 1) * 86400 + ((int64_t)f[3] - 1) * 86400 + (int64_t)f[2] * 3600 + (int64_t)f[1] * 60 + f[0];
	if (t < INT32_MIN || t > (int64_t)UINT32_MAX) { RET(0x80020005u); return; }   /* INVALID_ARGUMENT */
	tm_from_secs(A0, t);
	if (A1) WR32(A1, (uint32_t)t);
	if (A2) WR64(A2, (uint64_t)t);
	RET(0);
}
const HleEntry hle_sysmem[] = {
	{"sceKernelGetProcessTimeWide", h_time_wide},
	{"sceKernelGetProcessTimeLow", h_time_low},
	{"sceKernelGetProcessTime", h_time},
	{"sceKernelAllocMemBlock", h_alloc_block}, {"sceKernelFreeMemBlock", h_free_block}, {"sceKernelGetFreeMemorySize", h_free_mem_size},
	{"sceKernelLibcTime", h_libc_time}, {"sceKernelLibcGettimeofday", h_libc_gettimeofday}, {"sceKernelLibcClock", h_libc_clock},
	{"sceKernelLibcMktime", h_libc_mktime}, {"sceKernelLibcLocaltime_r", h_libc_gmtime}, {"sceKernelLibcGmtime_r", h_libc_gmtime},
	{"sceKernelGetMemBlockInfoByAddr", h_block_info_by_addr},
	{"sceRtcGetCurrentTick", h_rtc_tick}, {"sceRtcGetCurrentNetworkTick", h_rtc_network_tick}, {"sceRtcGetTickResolution", h_rtc_resolution},
	{"sceRtcGetCurrentClock", h_rtc_clock}, {"sceRtcGetCurrentClockLocalTime", h_rtc_clock_local},
	{"sceRtcConvertUtcToLocalTime", h_rtc_utc_local}, {"sceRtcConvertLocalTimeToUtc", h_rtc_utc_local},
	{"sceRtcCheckValid", h_rtc_check}, {"sceRtcIsLeapYear", h_rtc_leap}, {"sceRtcGetDaysInMonth", h_rtc_days_in_month}, {"sceRtcGetDayOfWeek", h_rtc_day_of_week},
	{"sceRtcGetTick", h_rtc_get_tick}, {"sceRtcSetTick", h_rtc_set_tick}, {"sceRtcCompareTick", h_rtc_compare},
	{"sceRtcSetTime_t", h_rtc_set_time_t}, {"sceRtcGetTime_t", h_rtc_get_time_t}, {"sceRtcSetTime64_t", h_rtc_set_time64}, {"sceRtcGetTime64_t", h_rtc_time64},
	{"sceRtcGetWin32FileTime", h_rtc_get_win32},
	{"sceRtcTickAddTicks", h_rtc_add_ticks}, {"sceRtcTickAddMicroseconds", h_rtc_add_us}, {"sceRtcTickAddSeconds", h_rtc_add_seconds},
	{"sceRtcTickAddMinutes", h_rtc_add_minutes}, {"sceRtcTickAddHours", h_rtc_add_hours}, {"sceRtcTickAddDays", h_rtc_add_days},
	{"sceRtcFormatRFC2822", h_rtc_rfc2822}, {"sceRtcFormatRFC2822LocalTime", h_rtc_rfc2822_local},
	{"sceRtcFormatRFC3339", h_rtc_rfc3339}, {"sceRtcFormatRFC3339LocalTime", h_rtc_rfc3339_local}, {"sceRtcParseRFC3339", h_rtc_parse3339},
	{"sceKernelGetMemBlockBase", h_block_base}, {"sceKernelFindMemBlockByAddr", h_find_block_by_addr},
	{0, 0},
};
