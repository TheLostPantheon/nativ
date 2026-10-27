/* Sony's NP Matching2 wire format (m2.h): framing and fields, read from np_matching2's own writers and readers
 * (FUN_810247ec the prefix, FUN_81024856 the header, FUN_81024ab2 / FUN_81024a32 the fields; 3.65 Ghidra names). */
#include <string.h>
#include "m2.h"
#include "../sha1.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int m2_parse_head(const uint8_t *p, size_t n, M2Head *h) {
	if (n < M2_PREFIX + M2_HEAD || (p[0] & 0xf0) != 0x10) return -1;   /* (the module's own check: the high nibble 1) */
	h->ver = p[0]; h->cls = p[1]; h->len = be16(p + 2);
	h->op = be16(p + 4); h->b6 = p[6]; h->b7 = p[7]; h->id = be32(p + 8);
	memcpy(h->r, p + 12, 4); memcpy(h->mac, p + 16, 16);
	return 0;
}

int m2_next_field(const uint8_t *body, size_t n, size_t *at, M2Field *f) {
	if (*at >= n) return 0;
	if (*at + 4 > n) return -1;
	const uint16_t w = be16(body + *at), len = be16(body + *at + 2);
	if (*at + 4 + len > n) return -1;
	f->tag = w & 0xfff; f->type = (uint8_t)(w >> 12); f->len = len; f->data = body + *at + 4;
	*at += 4 + (size_t)len;
	return 1;
}

void m2_dump(FILE *out, const char *dir, const uint8_t *p, size_t n) {
	M2Head h;
	if (m2_parse_head(p, n, &h)) {
		fprintf(out, "    m2 %s: %zu bytes, not a message:", dir, n);
		for (size_t i = 0; i < n && i < 32; i++) fprintf(out, " %02x", p[i]);
		fprintf(out, "\n"); return;
	}
	fprintf(out, "    m2 %s: op %#06x class %u len %u bytes6,7 %02x %02x id %#x r %02x%02x%02x%02x\n", dir, h.op, h.cls, h.len, h.b6, h.b7, h.id, h.r[0], h.r[1], h.r[2], h.r[3]);
	const uint8_t *body = p + M2_PREFIX + M2_HEAD; const size_t bn = n - M2_PREFIX - M2_HEAD;
	size_t at = 0; M2Field f; int r;
	while ((r = m2_next_field(body, bn, &at, &f)) == 1) {
		fprintf(out, "      tag %#05x type %x len %u:", f.tag, f.type, f.len);
		int text = f.len > 0; for (unsigned i = 0; i < f.len; i++) if (f.data[i] < 0x20 || f.data[i] > 0x7e) { text = text && i == f.len - 1u && !f.data[i]; }
		if (text) fprintf(out, " \"%.*s\"", (int)f.len, (const char *)f.data);
		const unsigned shown = m2_log() > 1 ? 1024 : 48;   /* (RECOMP_M2_LOG=2: whole fields) */
		if (!text) for (unsigned i = 0; i < f.len && i < shown; i++) fprintf(out, " %02x", f.data[i]);
		fprintf(out, f.len > shown && !text ? " ...\n" : "\n");
	}
	if (r < 0) fprintf(out, "      (a field runs past the body at %zu of %zu)\n", at, bn);
}

void m2_begin(M2Out *o, uint8_t cls, uint16_t op, uint8_t result, uint32_t id) {
	o->n = 0; o->bad = o->cap < M2_PREFIX + M2_HEAD;
	if (o->bad) return;
	memset(o->p, 0, M2_PREFIX + M2_HEAD);
	o->p[0] = 0x13; o->p[1] = cls;
	o->p[4] = (uint8_t)(op >> 8); o->p[5] = (uint8_t)op; o->p[6] = result;
	o->p[8] = (uint8_t)(id >> 24); o->p[9] = (uint8_t)(id >> 16); o->p[10] = (uint8_t)(id >> 8); o->p[11] = (uint8_t)id;
	o->n = M2_PREFIX + M2_HEAD;
}
void m2_put(M2Out *o, uint16_t tagword, const void *data, size_t len) {
	if (o->bad || len > 0xffff || o->n + 4 + len > o->cap) { o->bad = 1; return; }
	uint8_t *q = o->p + o->n;
	q[0] = (uint8_t)(tagword >> 8); q[1] = (uint8_t)tagword; q[2] = (uint8_t)(len >> 8); q[3] = (uint8_t)len;
	if (len) memcpy(q + 4, data, len);
	o->n += 4 + len;
}
void m2_put16(M2Out *o, uint16_t tagword, uint16_t v) { const uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; m2_put(o, tagword, b, 2); }
size_t m2_end(M2Out *o) {
	if (o->bad || o->n > 0xffff) return 0;
	o->p[2] = (uint8_t)(o->n >> 8); o->p[3] = (uint8_t)o->n;
	return o->n;
}
void m2_mac(const uint8_t *key, size_t kn, const uint8_t *msg, size_t n, uint8_t out[16]) {
	uint8_t tmp[0x10000], mac[20];
	if (n > sizeof tmp || n < M2_PREFIX + M2_HEAD) { memset(out, 0, 16); return; }
	memcpy(tmp, msg, n); memset(tmp + 16, 0, 16);
	hmac_sha1(key, kn, tmp, n, mac); memcpy(out, mac, 16);
}
