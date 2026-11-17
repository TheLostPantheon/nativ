/* Test programs for dev/tests/recomp_exec_test.py: compiled for the Vita's CPU (Thumb-2, Cortex-A9), recompiled, and
 * run on random inputs against Unicorn running the original code. Freestanding: no library, no division instruction
 * (the A9 has none: the compiler calls __aeabi_uidiv/idiv, defined below). Every exported function is t_<name>(a, b, c, d)
 * returning an int; the harness calls each one. They cover what the recompiler turns into control flow and memory
 * traffic: calls and returns, recursion, indirect calls through a table, switch tables (tbb/tbh), conditional execution
 * (IT blocks), 64-bit arithmetic (carry chains), loads and stores of every width, multiple-register pushes and loads
 * (struct copies), flag-setting loops, VFP floating point. */
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;

u32 __aeabi_uidiv(u32 n, u32 d) { u32 q = 0, r = 0; if (!d) return 0; for (int i = 31; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1u << i; } } return q; }
s32 __aeabi_idiv(s32 n, s32 d) { int neg = (n < 0) != (d < 0); u32 q = __aeabi_uidiv(n < 0 ? -(u32)n : (u32)n, d < 0 ? -(u32)d : (u32)d); return neg ? -(s32)q : (s32)q; }

static u32 table[64];
static unsigned char bytes[256];
struct S { u32 a, b, c, d, e, f; };
static struct S structs[8];

static __attribute__((noinline)) u32 mix(u32 x, u32 y) { x ^= y + 0x9e3779b9u + (x << 6) + (x >> 2); return x; }
static __attribute__((noinline)) u32 sq(u32 x) { return x * x; }
static __attribute__((noinline)) u32 cube(u32 x) { return x * x * x; }
static __attribute__((noinline)) u32 neg(u32 x) { return -x; }
static u32 (*const ops[4])(u32) = { sq, cube, neg, sq };

s32 t_fib(s32 n, s32 b, s32 c, s32 d) { n &= 15; return n < 2 ? n : t_fib(n - 1, b, c, d) + t_fib(n - 2, b, c, d); }
u32 t_loop(u32 a, u32 b, u32 c, u32 d) { u32 s = d; for (u32 i = 0; i < (a & 63); i++) s = mix(s, b + i * c); return s; }
u32 t_indirect(u32 a, u32 b, u32 c, u32 d) { u32 s = 0; for (int i = 0; i < 8; i++) s += ops[(a >> (2 * i)) & 3](b + (u32)i) ^ c; return s + d; }
u32 t_switch(u32 a, u32 b, u32 c, u32 d) {
	u32 s = 0;
	for (int i = 0; i < 6; i++) switch ((a >> (3 * i)) & 15) {
	case 0: s += b; break; case 1: s ^= c; break; case 2: s -= d; break; case 3: s = s * 3 + 1; break;
	case 4: s >>= 1; break; case 5: s <<= 2; break; case 6: s |= b & c; break; case 7: s &= ~d; break;
	case 8: s += 100; break; case 9: s = mix(s, a); break; case 10: s -= b ^ d; break; default: s += (u32)i; }
	return s;
}
s32 t_cond(s32 a, s32 b, s32 c, s32 d) {   /* (IT blocks: min, max, abs, selects) */
	s32 m = a < b ? a : b, x = c > d ? c : d, ab = a < 0 ? -a : a;
	return (m ^ x) + ab + (b == c ? 7 : -3) + ((u32)a > (u32)d ? 1 : 2);
}
u32 t_wide(u32 a, u32 b, u32 c, u32 d) {   /* (64-bit: umull, adds/adcs, shifts across words) */
	u64 x = (u64)a * b + ((u64)c << 32 | d), y = x >> (a & 31);
	y += (u64)(s32)c * (s32)d;
	return (u32)y ^ (u32)(y >> 32) ^ (u32)(x >> 7) * 7u;
}
u32 t_memory(u32 a, u32 b, u32 c, u32 d) {   /* (every width, both signs) */
	for (int i = 0; i < 64; i++) table[i] = mix(a, (u32)i * b);
	for (int i = 0; i < 256; i++) bytes[i] = (unsigned char)(table[i & 63] >> (i & 24));
	u32 s = c;
	for (int i = 0; i < 128; i++) { short h = ((short *)bytes)[i]; s += (u32)h ^ (u32)(signed char)bytes[255 - i] ^ table[(i * 5) & 63]; }
	return s ^ d;
}
u32 t_structs(u32 a, u32 b, u32 c, u32 d) {   /* (ldm/stm: struct copies) */
	for (int i = 0; i < 8; i++) { struct S v = { a + (u32)i, b, c ^ (u32)i, d, a * b, (u32)i }; structs[i] = v; }
	struct S t = structs[a & 7]; structs[b & 7] = structs[c & 7]; structs[d & 7] = t;
	u32 s = 0; for (int i = 0; i < 8; i++) s += structs[i].a ^ structs[i].c ^ structs[i].e ^ structs[i].f;
	return s;
}
u32 t_bits(u32 a, u32 b, u32 c, u32 d) {   /* (clz, rotates, bit fields, saturation-free arithmetic shifts) */
	u32 r = (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31));
	return (u32)__builtin_clz(c | 1) + r + ((d >> 3) & 0x1f) * 13 + (u32)((s32)c >> (d & 31)) + __builtin_popcount(a ^ d);
}
u32 t_divide(u32 a, u32 b, u32 c, u32 d) { return a / (b | 1) + (u32)((s32)c / (s32)(d | 1)) + a % 13; }
s32 t_float(s32 a, s32 b, s32 c, s32 d) {   /* (VFP scalar: convert, add, multiply, divide, compare) */
	float x = (float)a * 0.001f, y = (float)b * 0.25f + 1.0f, z = x / (y == 0.0f ? 1.0f : y);
	double w = (double)c * 1e-3 + (double)z;
	return (s32)(z * 1000.0f) + (w > 0.0 ? 1 : -1) * (s32)(w * 10.0) + (x < y ? d : -d);
}
