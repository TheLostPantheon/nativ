/* Vita system calls on the host: the C runtime: guest malloc, strings and memory, the printf family, libm, C++ static-init guards, exit/abort. */
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "hle.h"

/* ---- guest malloc: a size header in front of each block; free keeps nothing (a proof run) */
static uint32_t g_malloc(uint32_t n) {
	uint32_t a = guest_alloc(n + 16, 16);
	WR32(a, n);
	return a + 16;
}
static void h_malloc(Cpu *c) { RET(g_malloc(A0)); }
static void h_free(Cpu *c) { (void)c; }
static void h_calloc(Cpu *c) { uint32_t n = A0 * A1, a = g_malloc(n); memset(G(a), 0, n); RET(a); }
static void h_realloc(Cpu *c) {
	uint32_t old = A0, n = A1, a = g_malloc(n);
	if (old) { uint32_t on = RD32(old - 16); memcpy(G(a), G(old), on < n ? on : n); }
	RET(a);
}

/* ---- strings and memory */
static void h_memset(Cpu *c) { memset(G(A0), A1 & 0xff, A2); RET(A0); }
static void h_memcpy(Cpu *c) { memmove(G(A0), G(A1), A2); RET(A0); }
static void h_memcmp(Cpu *c) { RET(memcmp(G(A0), G(A1), A2)); }
static void h_memchr(Cpu *c) { uint8_t *p = memchr(G(A0), A1 & 0xff, A2); RET(p ? (uint32_t)(p - recomp_mem) : 0); }
static void h_strlen(Cpu *c) { RET(strlen((char *)G(A0))); }
static void h_strnlen(Cpu *c) { RET(strnlen((char *)G(A0), A1)); }
static void h_strcmp(Cpu *c) { RET(strcmp((char *)G(A0), (char *)G(A1))); }
static void h_strncmp(Cpu *c) { RET(strncmp((char *)G(A0), (char *)G(A1), A2)); }
static void h_strncasecmp(Cpu *c) { const unsigned char *a = G(A0), *b = G(A1); uint32_t n = A2; int r = 0; for (uint32_t i = 0; i < n && !r; i++) { r = tolower(a[i]) - tolower(b[i]); if (!a[i]) break; } RET((uint32_t)r); }   /* sceClibStrncasecmp: stubbed it returned 0, "equal", for any two strings */
static void h_strcpy(Cpu *c) { strcpy((char *)G(A0), (char *)G(A1)); RET(A0); }
static void h_strncpy(Cpu *c) { strncpy((char *)G(A0), (char *)G(A1), A2); RET(A0); }
static void h_strcat(Cpu *c) { strcat((char *)G(A0), (char *)G(A1)); RET(A0); }
static void h_strncat(Cpu *c) { strncat((char *)G(A0), (char *)G(A1), A2); RET(A0); }
/* sceClibStrlcat(dst, src, size): BSD strlcat -- the length it tried to make (np_matching2 builds its STUN server's name
 * with it: unimplemented, "us" "." "np" "." "stun.playstation.net" came out "usstun.playstation.net") */
static void h_strlcat(Cpu *c) {
	char *d = (char *)G(A0); const char *src = (const char *)G(A1); const size_t n = A2, dl = strnlen(d, n), sl = strlen(src);
	if (dl < n) { const size_t k = sl < n - dl - 1 ? sl : n - dl - 1; memcpy(d + dl, src, k); d[dl + k] = 0; }
	RET((uint32_t)(dl + sl));
}
static uint32_t gptr(char *p) { return p ? (uint32_t)((uint8_t *)p - recomp_mem) : 0; }
static void h_strstr(Cpu *c) { RET(gptr(strstr((char *)G(A0), (char *)G(A1)))); }
static void h_strrchr(Cpu *c) { RET(gptr(strrchr((char *)G(A0), (int)A1))); }
/* the guest's strtol/strtoul family: the Vita's long is 32 bits, so a string out of its range saturates there
 * (LONG_MAX/LONG_MIN, ULONG_MAX) -- parsed at 64 bits and clamped, the same on every host. Forwarded to the
 * host's own, a 64-bit long (macOS, Linux) parsed it whole and RET kept the low half, Windows' 32-bit one
 * saturated: three builds, two answers, and neither 64-bit one the Vita's. strtoul negates a '-' number
 * modulo 2^32, as a 32-bit one does ("-5" is 0xfffffffb), after checking the magnitude fits. */
static int32_t guest_strtol(const char *s, char **end, int base) {
	long long v = strtoll(s, end, base);
	return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}
static uint32_t guest_strtoul(const char *s, char **end, int base) {
	const char *p = s; while (isspace((unsigned char)*p)) p++;
	int neg = *p == '-'; if (neg || *p == '+') p++;
	if (isspace((unsigned char)*p) || *p == '+' || *p == '-') { *end = (char *)s; return 0; }   /* (no number: strtoull would take a second sign) */
	unsigned long long m = strtoull(p, end, base);
	if (*end == p) { *end = (char *)s; return 0; }
	if (m > UINT32_MAX) return UINT32_MAX;
	return neg ? 0u - (uint32_t)m : (uint32_t)m;
}
static void h_strtol(Cpu *c) {
	char *end; int32_t v = guest_strtol((char *)G(A0), &end, (int)A2);
	if (A1) WR32(A1, gptr(end));
	RET(v);
}
static void h_clib_strtoll(Cpu *c) {   /* sceClibStrtoll(str, endptr, base): 64 bits on the Vita too, the result in r0:r1 (np_matching2 reads its list's signature with it) */
	char *end; const long long v = strtoll((char *)G(A0), &end, (int)A2);
	if (A1) WR32(A1, gptr(end));
	c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)((unsigned long long)v >> 32);
}
static void h_abs(Cpu *c) { RET(abs((int32_t)A0)); }

/* ---- the C library's variables. The ctype tables are Dinkumware's: a variable holding a pointer
 * to 16-bit entries indexed by the character (entry -1 is EOF). _Ctype classes: */
enum { XD = 0x01, UP = 0x02, SP = 0x04, PU = 0x08, LO = 0x10, DI = 0x20, CN = 0x40, BB = 0x80 };
static uint32_t short_table(int (*f)(int)) {
	uint32_t t = guest_alloc(257 * 2, 4);
	for (int ch = -1; ch < 256; ch++) {
		int v = f(ch);
		uint8_t *p = G(t + 2 * (ch + 1)); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	}
	return t + 2;
}
static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static int upper(int ch) { return ch >= 'a' && ch <= 'z' ? ch - 32 : ch; }
static int ctype(int ch) {
	if (ch < 0 || ch > 127) return 0;
	int v = 0;
	if (ch >= '0' && ch <= '9') v |= DI | XD;
	if (ch >= 'a' && ch <= 'z') v |= LO | (ch <= 'f' ? XD : 0);
	if (ch >= 'A' && ch <= 'Z') v |= UP | (ch <= 'F' ? XD : 0);
	if (ch == ' ') v |= SP;
	if (ch >= '\t' && ch <= '\r') v |= CN;
	else if (ch < 32 || ch == 127) v |= BB;
	if (ch > ' ' && ch < 127 && !(v & (DI | LO | UP))) v |= PU;
	return v;
}
int libc_import_var(const char *name, uint32_t addr) {
	if (!strcmp(name, "_Ctype")) WR32(addr, short_table(ctype));
	else if (!strcmp(name, "_Tolotab")) WR32(addr, short_table(lower));
	else if (!strcmp(name, "_Touptab")) WR32(addr, short_table(upper));
	else if (!strcmp(name, "_Stdin") || !strcmp(name, "_Stdout") || !strcmp(name, "_Stderr")) return 1;
	/* Dinkumware's float constants (a union read as float or double) and std::_BADOFF (streamoff -1) */
	else if (!strcmp(name, "_FInf")) WR32(addr, 0x7f800000u);
	else if (!strcmp(name, "_FNan")) WR32(addr, 0x7fc00000u);
	else if (!strcmp(name, "_ZSt7_BADOFF")) WR64(addr, ~0ull);
	else if (!strcmp(name, "_ZSt4_Fpz")) return 1;   /* fpos 0: zero */   /* FILE objects: the slot itself; the printf family ignores the stream */
	else return 0;
	return 1;
}

/* ---- the C++ runtime's variables (Itanium ABI): a type_info is {vtable + 8, name}; a vtable is
 * {offset to top, type_info, virtual functions}. The game's own dynamic_cast reads a type_info's
 * class (__si_class_type_info etc.) through vtable[-1] of it, so the ABI classes' vtables need
 * their type_infos; type_infos the module doesn't import are made here */
static uint32_t guest_str(const char *s) { uint32_t a = guest_alloc((uint32_t)strlen(s) + 1, 4); strcpy((char *)G(a), s); return a; }
static uint32_t typeinfo_for(const char *mangled) {   /* the type_info object of a mangled type */
	static struct { char name[64]; uint32_t addr; } made[32]; static int n;
	char ti[72]; snprintf(ti, sizeof ti, "_ZTI%s", mangled);
	uint32_t a = hle_var_addr(ti);
	if (a) return a;
	for (int i = 0; i < n; i++) if (!strcmp(made[i].name, mangled)) return made[i].addr;
	a = guest_alloc(8, 4);
	uint32_t vt = hle_var_addr("_ZTVN10__cxxabiv117__class_type_infoE");
	WR32(a, vt ? vt + 8 : 0); WR32(a + 4, guest_str(mangled));
	if (n < 32) { strncpy(made[n].name, mangled, 63); made[n++].addr = a; }
	return a;
}
int cxx_import_var(const char *name, uint32_t addr) {
	if (!strncmp(name, "_ZTI", 4)) {   /* a type_info: those imported (the ABI's classes, std exceptions) have one base */
		uint32_t vt = hle_var_addr("_ZTVN10__cxxabiv120__si_class_type_infoE");
		WR32(addr, vt ? vt + 8 : 0); WR32(addr + 4, guest_str(name + 4));
		return 1;
	}
	if (!strncmp(name, "_ZTV", 4)) { WR32(addr, 0); WR32(addr + 4, typeinfo_for(name + 4)); return 1; }   /* virtual functions stay 0 */
	return !strcmp(name, "_ZSt7nothrow");   /* an empty object */
}

/* ---- printf family: a formatter over guest arguments. Arguments come from next(): the
 * registers/stack for printf(fmt, ...), or a guest va_list (a pointer into the arg area). */
typedef struct { Cpu *c; int reg; uint32_t stk; uint32_t va; } Args;
static uint32_t next32(Args *a) {
	if (a->va) { uint32_t v = RD32(a->va); a->va += 4; return v; }
	if (a->reg < 4) return a->c->r[a->reg++];
	uint32_t v = RD32(a->stk); a->stk += 4; return v;
}
static uint64_t next64(Args *a) {   /* doubles and long longs are 8-aligned (AAPCS) */
	if (a->va) { a->va = (a->va + 7) & ~7u; uint64_t v = RD64(a->va); a->va += 8; return v; }
	if (a->reg < 4) { a->reg = (a->reg + 1) & ~1; if (a->reg < 4) { uint64_t v = a->c->r[a->reg] | (uint64_t)a->c->r[a->reg + 1] << 32; a->reg += 2; return v; } }
	a->stk = (a->stk + 7) & ~7u; uint64_t v = RD64(a->stk); a->stk += 8; return v;
}
static int format(char *out, size_t cap, const char *f, Args *a) {
	size_t n = 0;
	char spec[32], tmp[512];
	while (*f) {
		if (*f != '%') { if (n + 1 < cap) out[n] = *f; n++; f++; continue; }
		const char *s = f++;
		while (*f && strchr("-+ #0123456789.", *f)) f++;
		int ll = 0, h = 0;
		while (*f && strchr("hlzjtL", *f)) { if (*f == 'l') ll++; if (*f == 'h') h++; f++; }
		char conv = *f ? *f++ : 0;
		size_t sl = (size_t)(f - s) < sizeof spec - 4 ? (size_t)(f - s) : sizeof spec - 4;
		memcpy(spec, s, sl); spec[sl] = 0;
		/* rebuild the spec without length modifiers, then add the host's */
		char base[32]; size_t bl = 0;
		for (size_t i = 0; i < sl - 1; i++) if (!strchr("hlzjtL", spec[i])) base[bl++] = spec[i];
		base[bl] = 0;
		int k;
		switch (conv) {
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
			char fmt[40]; snprintf(fmt, sizeof fmt, "%sll%c", base, conv);
			if (conv == 'c') { snprintf(fmt, sizeof fmt, "%sc", base); k = snprintf(tmp, sizeof tmp, fmt, (int)next32(a)); break; }
			long long v = ll >= 2 ? (long long)next64(a) : (conv == 'd' || conv == 'i') ? (long long)(int32_t)next32(a) : (long long)next32(a);
			if (h == 1 && conv != 'd' && conv != 'i') v &= 0xffff;
			k = snprintf(tmp, sizeof tmp, fmt, v); break;
		}
		case 'p': k = snprintf(tmp, sizeof tmp, "0x%08x", next32(a)); break;
		case 's': {
			char fmt[40]; snprintf(fmt, sizeof fmt, "%ss", base);
			uint32_t p = next32(a); k = snprintf(tmp, sizeof tmp, fmt, p ? (char *)G(p) : "(null)"); break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
			char fmt[40]; snprintf(fmt, sizeof fmt, "%s%c", base, conv);
			uint64_t b = next64(a); double d; memcpy(&d, &b, 8); k = snprintf(tmp, sizeof tmp, fmt, d); break;
		}
		case '%': k = snprintf(tmp, sizeof tmp, "%%"); break;
		case 'n': next32(a); k = 0; break;
		default: k = snprintf(tmp, sizeof tmp, "%s", spec); break;
		}
		for (int i = 0; i < k && i < (int)sizeof tmp - 1; i++) { if (n + 1 < cap) out[n] = tmp[i]; n++; }
	}
	if (cap) out[n < cap ? n : cap - 1] = 0;
	return (int)n;
}
/* the last string the game formatted: release builds format their assert messages and then print
 * them nowhere, so a stop shows this */
char hle_last_format[1024];
static void remember(const char *s) {
	strncpy(hle_last_format, s, sizeof hle_last_format - 1);
	static int all = -1; if (all < 0) all = getenv("RECOMP_LOG_FORMAT") != 0;   /* every string the game formats */
	if (all) fprintf(stderr, "    fmt: %s\n", s);
}
void hle_note_format(const char *s) { remember(s); }   /* a module's formatting (main.c observes it) */
static void guest_out(const char *s) { fputs("[game] ", stdout); fputs(s, stdout); if (!*s || s[strlen(s) - 1] != '\n') fputc('\n', stdout); fflush(stdout); }
static void h_printf(Cpu *c) { char b[4096]; Args a = {c, 1, c->r[13], 0}; RET(format(b, sizeof b, (char *)G(A0), &a)); guest_out(b); }
static void h_vprintf(Cpu *c) { char b[4096]; Args a = {c, 4, 0, A1}; RET(format(b, sizeof b, (char *)G(A0), &a)); guest_out(b); }
static void h_fprintf(Cpu *c) { char b[4096]; Args a = {c, 2, c->r[13], 0}; RET(format(b, sizeof b, (char *)G(A1), &a)); guest_out(b); }
static void h_vfprintf(Cpu *c) { char b[4096]; Args a = {c, 4, 0, A2}; RET(format(b, sizeof b, (char *)G(A1), &a)); guest_out(b); }
static void h_sprintf(Cpu *c) { char b[4096]; Args a = {c, 2, c->r[13], 0}; int n = format(b, sizeof b, (char *)G(A1), &a); strcpy((char *)G(A0), b); remember(b); RET(n); }
static void h_snprintf(Cpu *c) { uint32_t out = A0; Args a = {c, 3, c->r[13], 0}; RET(format((char *)G(out), A1, (char *)G(A2), &a)); remember((char *)G(out)); }
static void h_vsprintf(Cpu *c) { char b[4096]; Args a = {c, 4, 0, A2}; int n = format(b, sizeof b, (char *)G(A1), &a); strcpy((char *)G(A0), b); remember(b); RET(n); }
static void h_vsnprintf(Cpu *c) { uint32_t out = A0; Args a = {c, 4, 0, A3}; RET(format((char *)G(out), A1, (char *)G(A2), &a)); remember((char *)G(out)); }

/* ---- scanf family: each conversion runs through the host's sscanf on its own (with %n to learn
 * how far it read), and the value is stored through the guest pointer at its guest size */
static int scan(const char *in, const char *f, Args *a) {
	const char *p = in; int got = 0;
	while (*f) {
		if (*f == ' ' || (*f >= '\t' && *f <= '\r')) { while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++; f++; continue; }
		if (*f != '%') { if (*p != *f) return got; p++; f++; continue; }
		if (f[1] == '%') { if (*p != '%') return got; p++; f += 2; continue; }
		const char *s = f++;
		int skip = *f == '*'; if (skip) f++;
		while (*f >= '0' && *f <= '9') f++;
		int h = 0, l = 0; while (*f && strchr("hlLjzt", *f)) { if (*f == 'h') h++; if (*f == 'l' || *f == 'L' || *f == 'j') l++; f++; }
		char conv = *f;
		const char *e = f + 1;
		if (conv == '[') { e = f + 1; if (*e == '^') e++; if (*e == ']') e++; while (*e && *e != ']') e++; if (*e) e++; }
		/* the host spec: width and conversion, our own length modifier, then %n */
		char spec[160], wid[16] = ""; size_t wl = 0;
		for (const char *q = s + 1 + skip; *q >= '0' && *q <= '9' && wl < sizeof wid - 1; q++) wid[wl++] = *q;
		wid[wl] = 0;
		int n = -1;
		if (conv == 'n') { if (!skip) WR32(next32(a), (uint32_t)(p - in)); f = e; continue; }
		if (!*p && conv != 'c') return got ? got : -1;
		if (strchr("dioux", conv) || conv == 'X') {
			long long v; snprintf(spec, sizeof spec, "%%%sll%c%%n", wid, conv == 'X' ? 'x' : conv);
			if (sscanf(p, spec, &v, &n) < 1 || n < 0) return got;
			if (!skip) { uint32_t d = next32(a); if (h == 2) WR8(d, (uint8_t)v); else if (h) WR16(d, (uint16_t)v); else if (l >= 2) WR64(d, (uint64_t)v); else WR32(d, (uint32_t)v); got++; }
		} else if (strchr("fFeEgGaA", conv)) {
			double v; snprintf(spec, sizeof spec, "%%%slf%%n", wid);
			if (sscanf(p, spec, &v, &n) < 1 || n < 0) return got;
			if (!skip) { uint32_t d = next32(a); if (l) { uint64_t b; memcpy(&b, &v, 8); WR64(d, b); } else { float fv = (float)v; uint32_t b; memcpy(&b, &fv, 4); WR32(d, b); } got++; }
		} else if (conv == 's' || conv == 'c' || conv == '[') {
			static char tmp[4096];
			snprintf(spec, sizeof spec, "%%%s%.*s%%n", wid[0] ? wid : (conv == 's' || conv == '[' ? "4095" : ""), (int)(e - f), f);
			if (sscanf(p, spec, tmp, &n) < 1 || n < 0) return got;
			if (!skip) { uint32_t d = next32(a); size_t len = conv == 'c' ? (size_t)(wid[0] ? atoi(wid) : 1) : strlen(tmp) + 1; memcpy(G(d), tmp, len); got++; }
		} else if (conv == 'p') {
			unsigned v; if (sscanf(p, "%x%n", &v, &n) < 1) return got;
			if (!skip) { WR32(next32(a), v); got++; }
		} else return got;
		p += n; f = e;
	}
	return got;
}
static void h_sscanf(Cpu *c) { Args a = {c, 2, c->r[13], 0}; RET(scan((char *)G(A0), (char *)G(A1), &a)); }
static void h_vsscanf(Cpu *c) { Args a = {c, 4, 0, A2}; RET(scan((char *)G(A0), (char *)G(A1), &a)); }

/* ---- libm, hard-float ABI: float arguments in s0, s1, results in s0; doubles in d0, d1 / d0 */
#define F0 c->vfp.f[0]
#define F1 c->vfp.f[1]
#define D0 c->vfp.fd[0]
#define D1 c->vfp.fd[1]
static void h_fmodf(Cpu *c) { F0 = fmodf(F0, F1); }
static void h_fmod(Cpu *c) { D0 = fmod(D0, D1); }
static void h_powf(Cpu *c) { F0 = powf(F0, F1); }
static void h_pow(Cpu *c) { D0 = pow(D0, D1); }
static void h_floorf(Cpu *c) { F0 = floorf(F0); }
static void h_floor(Cpu *c) { D0 = floor(D0); }
static void h_rint(Cpu *c) { D0 = rint(D0); }
static void h_fabsf(Cpu *c) { F0 = fabsf(F0); }
static void h_sqrtf(Cpu *c) { F0 = sqrtf(F0); }
static void h_tanf(Cpu *c) { F0 = tanf(F0); }
static void h_atan2f(Cpu *c) { F0 = atan2f(F0, F1); }
static void h_expf(Cpu *c) { F0 = expf(F0); }
static void h_ceilf(Cpu *c) { F0 = ceilf(F0); }
static void h_atan2(Cpu *c) { D0 = atan2(D0, D1); }
static void h_exp2f(Cpu *c) { F0 = exp2f(F0); }
static void h_fmaxf(Cpu *c) { F0 = fmaxf(F0, F1); }
static void h_fminf(Cpu *c) { F0 = fminf(F0, F1); }
static void h_ldexp(Cpu *c) { D0 = ldexp(D0, (int32_t)A0); }
static void h_ldexpf(Cpu *c) { F0 = ldexpf(F0, (int32_t)A0); }
/* Dinkumware internals: _Sinx(x, quadrant offset) is sin for an even offset, cos for an odd one
 * (sin(x + qoff * pi/2)); _Log(x, base flag) is ln, log10, log2 for 0, 1, 2 */
static void h_sinx(Cpu *c) { D0 = (A0 & 1) ? ((A0 & 2) ? -cos(D0) : cos(D0)) : ((A0 & 2) ? -sin(D0) : sin(D0)); }
static void h_fsinx(Cpu *c) { F0 = (A0 & 1) ? ((A0 & 2) ? -cosf(F0) : cosf(F0)) : ((A0 & 2) ? -sinf(F0) : sinf(F0)); }
static void h_log(Cpu *c) { D0 = A0 == 1 ? log10(D0) : A0 == 2 ? log2(D0) : log(D0); }
static void h_flog(Cpu *c) { F0 = A0 == 1 ? log10f(F0) : A0 == 2 ? log2f(F0) : logf(F0); }

static void h_errno_loc(Cpu *c) { static uint32_t e; if (!e) e = guest_alloc(4, 4); RET(e); }
/* C++ function-local statics: acquire returns 1 when the caller must run the initialiser */
static void h_guard_acquire(Cpu *c) { RET(!(RD8(A0) & 1)); }
static void h_guard_release(Cpu *c) { WR8(A0, RD8(A0) | 1); }
static void h_abort(Cpu *c) { (void)c; fprintf(stderr, "game called abort()\n"); exit(4); }
void gfx_gpu_builds_stop(void);
static void h_exit(Cpu *c) { fprintf(stderr, "game called exit(%d)\n", (int)A0); gfx_gpu_builds_stop(); exit(0); }   /* (the shader workers first: gfx.h) */
static void h_stack_chk_fail(Cpu *c) { (void)c; fprintf(stderr, "stack check failed (stack smashing detected)\n"); exit(4); }

/* ---- more of the C runtime */
static void h_strchr(Cpu *c) { RET(gptr(strchr((char *)G(A0), (int)(A1 & 0xff)))); }
/* ldiv_t (8 bytes) comes back through memory: r0 is the result's address, the arguments follow */
static void h_ldiv(Cpu *c) {
	int32_t n = (int32_t)A1, d = (int32_t)A2;
	WR32(A0, d ? (uint32_t)(n / d) : 0); WR32(A0 + 4, d ? (uint32_t)(n % d) : 0);
}
/* qsort(base, count, size, compare): a merge sort of indices with the guest comparator, then the
 * elements are permuted in place */
static void msort(Cpu *c, uint32_t *ix, uint32_t *tmp, uint32_t n, uint32_t base, uint32_t size, uint32_t cmp) {
	if (n < 2) return;
	uint32_t m = n / 2;
	msort(c, ix, tmp, m, base, size, cmp); msort(c, ix + m, tmp, n - m, base, size, cmp);
	uint32_t i = 0, j = m, k = 0;
	while (i < m && j < n) tmp[k++] = (int32_t)guest_call(c, cmp, base + ix[j] * size, base + ix[i] * size) < 0 ? ix[j++] : ix[i++];
	while (i < m) tmp[k++] = ix[i++];
	while (j < n) tmp[k++] = ix[j++];
	memcpy(ix, tmp, n * 4);
}
static void h_qsort(Cpu *c) {
	uint32_t base = A0, n = A1, size = A2, cmp = A3;
	if (n < 2 || !size) return;
	uint32_t *ix = malloc(n * 4), *tmp = malloc(n * 4);
	for (uint32_t i = 0; i < n; i++) ix[i] = i;
	msort(c, ix, tmp, n, base, size, cmp);
	uint8_t *copy = malloc((size_t)n * size);
	for (uint32_t i = 0; i < n; i++) memcpy(copy + (size_t)i * size, G(base + ix[i] * size), size);
	memcpy(G(base), copy, (size_t)n * size);
	free(ix); free(tmp); free(copy);
}
/* C++ iostreams (Dinkumware, in the system's libc.suprx): ios_base::_Init as the module does it:
 * state +8 and exceptions +0xc clear, format flags +0x10 skipws|dec, precision +0x14 6, width
 * +0x18, word arrays +0x1c/+0x20 empty. std::_Mutex: +0 a flag, locked through one host
 * recursive lock (stream locks are short) */
static void h_ios_init(Cpu *c) {
	WR32(A0 + 8, 0); WR32(A0 + 0xc, 0); WR32(A0 + 0x10, 0x201); WR32(A0 + 0x14, 6);
	WR32(A0 + 0x18, 0); WR32(A0 + 0x1c, 0); WR32(A0 + 0x20, 0);
}
static pthread_mutex_t cxx_lock;
static void cxx_lock_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&cxx_lock, &a); }
static pthread_once_t cxx_once = PTHREAD_ONCE_INIT;
static void h_cxx_mutex(Cpu *c) { WR32(A0, 1); pthread_once(&cxx_once, cxx_lock_init); RET(A0); }
/* the one host lock held across guest code: under the scheduler (RECOMP_SCHED=vita) a thread that
 * must wait for it gives its slot up first -- the holder may be waiting for a slot, and a waiter
 * keeping its slot on one core would deadlock with it */
static void h_cxx_mutex_lock(Cpu *c) {
	(void)c; pthread_once(&cxx_once, cxx_lock_init);
	if (!recomp_sched_on) { pthread_mutex_lock(&cxx_lock); return; }
	if (!pthread_mutex_trylock(&cxx_lock)) return;
	int b = sched_block_begin(); pthread_mutex_lock(&cxx_lock); if (b) sched_block_end();   /* every waiter for it is slotless, so holding it while waiting for a slot is safe */
}
static void h_cxx_mutex_unlock(Cpu *c) { (void)c; pthread_mutex_unlock(&cxx_lock); }
static void h_nop(Cpu *c) { (void)c; }
static void h_ios_clear(Cpu *c) { WR32(A0 + 8, A1 & 0x17); }   /* (this, state, reraise): exceptions aren't raised */
/* Dinkumware's number parsers: _Stolx/_Stoulx(string, *end, base, *error), _Stofx(string, *end, power of ten, *error) */
static void h_stolx(Cpu *c) { char *e; int32_t v = guest_strtol((char *)G(A0), &e, (int)A2); if (A1) WR32(A1, gptr(e)); if (A3) WR32(A3, 0); RET(v); }
static void h_stoulx(Cpu *c) { char *e; uint32_t v = guest_strtoul((char *)G(A0), &e, (int)A2); if (A1) WR32(A1, gptr(e)); if (A3) WR32(A3, 0); RET(v); }
static void h_stofx(Cpu *c) {
	char *e; float v = strtof((char *)G(A0), &e);
	if ((int32_t)A2) v *= powf(10.0f, (float)(int32_t)A2);
	if (A1) WR32(A1, gptr(e)); if (A3) WR32(A3, 0);
	c->vfp.f[0] = v;
}
static void h_strcasecmp(Cpu *c) { RET(strcasecmp((char *)G(A0), (char *)G(A1))); }
/* time: struct tm is nine ints, as on the host; gmtime/localtime return a static guest copy */
static uint32_t guest_tm(struct tm *t) {
	static uint32_t g; if (!g) g = guest_alloc(64, 4);
	int v[9] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
	for (int i = 0; i < 9; i++) WR32(g + 4 * i, (uint32_t)v[i]);
	return g;
}
static void h_gmtime(Cpu *c) { time_t t = (time_t)(int32_t)RD32(A0); struct tm r; gmtime_r(&t, &r); RET(guest_tm(&r)); }
static void h_localtime(Cpu *c) { time_t t = (time_t)(int32_t)RD32(A0); struct tm r; localtime_r(&t, &r); RET(guest_tm(&r)); }
static void h_strftime(Cpu *c) {   /* (buf, max, format, tm*) */
	struct tm t = {0};
	int *f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
	for (int i = 0; i < 9; i++) *f[i] = (int32_t)RD32(A3 + 4 * i);
	RET(strftime((char *)G(A0), A1, (char *)G(A2), &t));
}
/* C++ RTTI (Itanium ABI): a type_info is {vtable, name}; equal when the names are */
static void h_typeinfo_name(Cpu *c) { RET(RD32(A0 + 4)); }
static void h_typeinfo_eq(Cpu *c) {
	if (!A0 || !A1) { RET(A0 == A1); return; }
	uint32_t a = RD32(A0 + 4), b = RD32(A1 + 4);
	RET(a == b || (a && b && !strcmp((char *)G(a), (char *)G(b))));
}
static void h_stoul(Cpu *c) {   /* _Stoul(string, *end, base) */
	char *end; uint32_t v = guest_strtoul((char *)G(A0), &end, (int)A2);
	if (A1) WR32(A1, gptr(end));
	RET(v);
}
/* rand/srand: one sequence for the whole process, as the guest libc has -- the host's rand() isn't
 * safe to call from several threads at once (guest threads are host threads) */
static pthread_mutex_t rand_m = PTHREAD_MUTEX_INITIALIZER;
static void h_rand(Cpu *c) { pthread_mutex_lock(&rand_m); uint32_t v = (uint32_t)rand() & 0x7fffffff; pthread_mutex_unlock(&rand_m); RET(v); }
static void h_srand(Cpu *c) { pthread_mutex_lock(&rand_m); srand(A0); pthread_mutex_unlock(&rand_m); }
static void h_time(Cpu *c) { uint32_t t = guest_clock_fixed() ? (uint32_t)(1791244800ull + guest_time_us() / 1000000) : (uint32_t)time(0); if (A0) WR32(A0, t); RET(t); }   /* the fixed test clock's date: 2026-10-06 */
static void h_strtok(Cpu *c) {
	static uint32_t next;
	uint32_t s = A0 ? A0 : next;
	if (!s) { RET(0); return; }
	const char *d = (char *)G(A1);
	s += strspn((char *)G(s), d);
	if (!RD8(s)) { next = 0; RET(0); return; }
	uint32_t e = s + strcspn((char *)G(s), d);
	if (RD8(e)) { WR8(e, 0); next = e + 1; } else next = 0;
	RET(s);
}
static void h_strtoul(Cpu *c) { char *end; uint32_t v = guest_strtoul((char *)G(A0), &end, (int)A2); if (A1) WR32(A1, gptr(end)); RET(v); }
static void h_divzero(Cpu *c) { RET(0); }   /* __sce_aeabi_idiv0: integer division by zero yields 0 */
/* bsearch(key, base, count, size, compare): the comparator is guest code */
static void h_bsearch(Cpu *c) {
	uint32_t key = A0, base = A1, n = A2, size = A3, cmp = STK(0);
	uint32_t lo = 0, hi = n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, at = base + mid * size;
		int32_t r = (int32_t)guest_call(c, cmp, key, at);
		if (r == 0) { RET(at); return; }
		if (r < 0) hi = mid; else lo = mid + 1;
	}
	RET(0);
}
static void h_fmaf(Cpu *c) { c->vfp.f[0] = fmaf(c->vfp.f[0], c->vfp.f[1], c->vfp.f[2]); }
/* Dinkumware float sine/cosine: _FSin(x, quadrant offset) as _FSinx */
static void h_fsin(Cpu *c) { float x = c->vfp.f[0]; c->vfp.f[0] = (A0 & 1) ? ((A0 & 2) ? -cosf(x) : cosf(x)) : ((A0 & 2) ? -sinf(x) : sinf(x)); }
/* _Stod(string, *end, ?): strtod into d0 */
static void h_stod(Cpu *c) { char *e; c->vfp.fd[0] = strtod((char *)G(A0), &e); if (A1) WR32(A1, gptr(e)); }

/* sceClibMspace*: a heap in memory the caller gives (libgpu_es4 keeps its objects in one). The
 * region holds { size, first block } then blocks { size | used bit, payload }, 8-byte aligned,
 * first fit, merged with the following free block on free. Thread-safe, as the console's are: Uncharted's
 * movie player allocates from one heap on its demux and video-decode threads at once -- unlocked, the
 * two corrupted the block chain and both spun for ever on a zero-size header (2026-10-16) */
#define MS_USED 1u
static pthread_mutex_t ms_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ms_end(uint32_t ms) { return ms + RD32(ms); }
static void h_mspace_create(Cpu *c) {   /* (base, size) -> mspace (0 if too small) */
	uint32_t base = (A0 + 7) & ~7u, size = (A1 - (base - A0)) & ~7u;
	/* RECOMP_NO_MSPACE=1: the game's own heaps fail to create (the pre-heap behaviour Uncharted's test
	 * relies on until video playback exists); the firmware modules' heaps (callers at 0x88000000 and
	 * up: libgpu_es4 keeps its objects in one) are untouched */
	{ const char *e = getenv("RECOMP_NO_MSPACE"); if (e && *e == '1' && c->r[14] < 0x88000000u) { RET(0); return; } }
	if (!A0 || A1 < 64 + (base - A0)) { RET(0); return; }
	WR32(base, size); WR32(base + 8, size - 8);   /* one free block from +8 */
	RET(base);
}
static uint32_t ms_alloc(uint32_t ms, uint32_t size, uint32_t align) {
	size = (size + 7) & ~7u; if (!size) size = 8;
	for (uint32_t b = ms + 8; b < ms_end(ms); b += RD32(b) & ~MS_USED) {
		uint32_t h = RD32(b);
		if ((h & ~MS_USED) < 8 || b + (h & ~MS_USED) > ms_end(ms)) {   /* a broken chain (an overrun by its owner): stop rather than loop */
			static int said; if (!said++) fprintf(stderr, "mspace %#x: corrupt block header %#x at %#x -- allocation fails\n", ms, h, b);
			return 0;
		}
		if (h & MS_USED) continue;
		while (b + h < ms_end(ms) && !(RD32(b + h) & MS_USED)) h += RD32(b + h);   /* merge free neighbours */
		WR32(b, h);
		uint32_t p = (b + 8 + align - 1) & ~(align - 1), pad = p - 8 - b;   /* payload aligned; header 8 before it */
		if (pad && pad < 16) { p += align < 16 ? (16 + align - 1) / align * align : align; pad = p - 8 - b; }
		if (pad + 8 + size > h) continue;
		if (pad) { WR32(b, pad); b += pad; h -= pad; }   /* the gap before stays a free block */
		if (h - 8 - size >= 16) { WR32(b + 8 + size, h - 8 - size); h = 8 + size; }
		WR32(b, h | MS_USED);
		return b + 8;
	}
	return 0;
}
static uint32_t ms_alloc_locked(uint32_t ms, uint32_t size, uint32_t align) {
	if (!ms) return 0;
	pthread_mutex_lock(&ms_lock); uint32_t p = ms_alloc(ms, size, align); pthread_mutex_unlock(&ms_lock);
	return p;
}
static void h_mspace_malloc(Cpu *c) { RET(ms_alloc_locked(A0, A1, 8)); }   /* (mspace, size) */
static void h_mspace_memalign(Cpu *c) { RET(ms_alloc_locked(A0, A2, A1 < 8 ? 8 : A1)); }   /* (mspace, alignment, size) */
static void h_mspace_calloc(Cpu *c) {   /* (mspace, count, size) */
	uint32_t n = A1 * A2, p = ms_alloc_locked(A0, n, 8);
	if (p) memset(G(p), 0, n);
	RET(p);
}
static void h_mspace_free(Cpu *c) {   /* (mspace, pointer) */
	if (A0 && A1) { pthread_mutex_lock(&ms_lock); WR32(A1 - 8, RD32(A1 - 8) & ~MS_USED); pthread_mutex_unlock(&ms_lock); }
	RET(0);
}
static void h_mspace_realloc(Cpu *c) {   /* (mspace, pointer, size) */
	if (!A1) { RET(ms_alloc_locked(A0, A2, 8)); return; }
	pthread_mutex_lock(&ms_lock);
	uint32_t old = (RD32(A1 - 8) & ~MS_USED) - 8, p = ms_alloc(A0, A2, 8);
	if (p) { memcpy(G(p), G(A1), old < A2 ? old : A2); WR32(A1 - 8, RD32(A1 - 8) & ~MS_USED); }
	pthread_mutex_unlock(&ms_lock);
	RET(p);
}

const HleEntry hle_libc[] = {
	{"sceClibMspaceCreate", h_mspace_create}, {"sceClibMspaceDestroy", h_nop}, {"sceClibMspaceMalloc", h_mspace_malloc},
	{"sceClibMspaceCalloc", h_mspace_calloc}, {"sceClibMspaceFree", h_mspace_free}, {"sceClibMspaceRealloc", h_mspace_realloc},
	{"sceClibMspaceMemalign", h_mspace_memalign},
	{"_ZNSt8ios_base5_InitEv", h_ios_init}, {"_ZNSt6_MutexC1Ev", h_cxx_mutex}, {"_ZNSt6_MutexC2Ev", h_cxx_mutex},
	{"_ZNSt6_Mutex5_LockEv", h_cxx_mutex_lock}, {"_ZNSt6_Mutex7_UnlockEv", h_cxx_mutex_unlock},
	{"_ZNSt6_MutexD1Ev", h_nop}, {"_ZNSt6_MutexD2Ev", h_nop}, {"_ZNSt8ios_baseD2Ev", h_nop},
	{"_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb", h_ios_clear},
	{"_Stolx", h_stolx}, {"_Stoulx", h_stoulx}, {"_Stofx", h_stofx}, {"strcasecmp", h_strcasecmp},
	{"ceilf", h_ceilf}, {"atan2", h_atan2}, {"gmtime", h_gmtime}, {"localtime", h_localtime}, {"strftime", h_strftime},
	{"sscanf", h_sscanf}, {"vsscanf", h_vsscanf}, {"qsort", h_qsort}, {"expf", h_expf}, {"exp2f", h_exp2f},
	{"fmaxf", h_fmaxf}, {"fminf", h_fminf}, {"_ZNKSt9type_info4nameEv", h_typeinfo_name}, {"_ZNKSt9type_infoeqERKS_", h_typeinfo_eq},
	{"_Stoul", h_stoul}, {"strtoul", h_strtoul}, {"rand", h_rand}, {"srand", h_srand}, {"time", h_time}, {"strtok", h_strtok},
	{"__sce_aeabi_idiv0", h_divzero}, {"__aeabi_idiv0", h_divzero},
	{"sceClibMemcpy", h_memcpy}, {"sceClibMemcpy_safe", h_memcpy}, {"sceClibMemmove", h_memcpy}, {"sceClibMemcmp", h_memcmp}, {"sceClibMemchr", h_memchr},
	{"sceClibStrcmp", h_strcmp}, {"sceClibStrncmp", h_strncmp}, {"sceClibStrncasecmp", h_strncasecmp}, {"sceClibStrnlen", h_strnlen}, {"sceClibStrncpy", h_strncpy},
	{"sceClibStrncat", h_strncat}, {"sceClibStrlcat", h_strlcat}, {"sceClibStrrchr", h_strrchr}, {"sceClibStrchr", h_strchr}, {"sceClibStrstr", h_strstr},
	{"sceClibSnprintf", h_snprintf}, {"sceClibVsnprintf", h_vsnprintf}, {"sceClibStrtoll", h_clib_strtoll},
	{"strchr", h_strchr}, {"ldiv", h_ldiv}, {"bsearch", h_bsearch}, {"fmaf", h_fmaf}, {"_FSin", h_fsin}, {"_Stod", h_stod},
	{"malloc", h_malloc},
	{"free", h_free},
	{"calloc", h_calloc},
	{"realloc", h_realloc},
	{"memset", h_memset},
	{"sceClibMemset", h_memset},
	{"memcpy", h_memcpy},
	{"memmove", h_memcpy},
	{"memcmp", h_memcmp},
	{"memchr", h_memchr},
	{"strlen", h_strlen},
	{"strnlen_s", h_strnlen},
	{"strcmp", h_strcmp},
	{"strncmp", h_strncmp},
	{"strcpy", h_strcpy},
	{"strncpy", h_strncpy},
	{"strcat", h_strcat},
	{"strncat", h_strncat},
	{"strstr", h_strstr},
	{"strrchr", h_strrchr},
	{"strtol", h_strtol},
	{"abs", h_abs},
	{"printf", h_printf},
	{"sceClibPrintf", h_printf},
	{"vprintf", h_vprintf},
	{"fprintf", h_fprintf},
	{"vfprintf", h_vfprintf},
	{"sprintf", h_sprintf},
	{"snprintf", h_snprintf},
	{"vsprintf", h_vsprintf},
	{"vsnprintf", h_vsnprintf},
	{"fmodf", h_fmodf},
	{"fmod", h_fmod},
	{"powf", h_powf},
	{"pow", h_pow},
	{"floorf", h_floorf},
	{"floor", h_floor},
	{"rint", h_rint},
	{"fabsf", h_fabsf},
	{"sqrtf", h_sqrtf},
	{"tanf", h_tanf},
	{"atan2f", h_atan2f},
	{"ldexp", h_ldexp},
	{"ldexpf", h_ldexpf},
	{"_Sinx", h_sinx},
	{"_FSinx", h_fsinx},
	{"_Log", h_log},
	{"_FLog", h_flog},
	{"__cxa_guard_acquire", h_guard_acquire},
	{"__cxa_guard_release", h_guard_release},
	{"_sceLibcErrnoLoc", h_errno_loc},
	{"abort", h_abort},
	{"exit", h_exit},
	{"__stack_chk_fail", h_stack_chk_fail},
	{0, 0},
};
