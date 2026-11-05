/* input_rec.c -- `# nativ-input v2` encode/parse/validate; see input_rec.h. */
#include "input_rec.h"
#include <string.h>
#include <stdio.h>

static int row_same(const InputRecRow *a, const InputRecRow *b)
{
	return a->buttons == b->buttons && !memcmp(a->st, b->st, sizeof a->st);
}

int input_rec_header_check(const char *line, int *version)
{
	int v;
	if (!line || strncmp(line, "# nativ-input", 13)) return 0;
	if (sscanf(line + 13, " v%d", &v) != 1 || v < 1) return -1;  /* unreadable header */
	if (v > INPUT_REC_VERSION) return -1;      /* newer trace than this reader knows */
	if (version) *version = v;
	return 1;
}

int input_rec_header_write(FILE *f)
{
	if (!f) return -1;
	/* one line: version and origin together, so a loader's first read decides both */
	if (fprintf(f, "# nativ-input v%d origin frame=0 poll=0 ms=0\n", INPUT_REC_VERSION) < 0)
		return -1;
	return 0;
}

void input_rec_writer_init(InputRecWriter *w, FILE *f)
{
	memset(w, 0, sizeof *w);
	w->f = f;
}

int input_rec_observe(InputRecWriter *w, uint64_t frame, uint64_t now_us,
                      const uint32_t *buttons, const uint8_t st[4])
{
	InputRecRow r;
	if (!w || !w->f) return -1;
	if (!w->ms0_set) { w->ms0 = now_us; w->ms0_set = 1; }
	w->poll++;
	memset(&r, 0, sizeof r);
	r.frame   = frame;
	r.poll    = w->poll;
	r.ms      = (uint32_t)((now_us - w->ms0) / 1000);
	r.buttons = *buttons;
	memcpy(r.st, st, sizeof r.st);
	w->last_ms = r.ms;
	if (w->have && row_same(&w->prev, &r)) return 0;    /* held input: no row (RLE by span) */
	w->prev = r;
	w->have = 1;
	/* buttons hex as the v1 loader always read them; sticks decimal; poll/ms after */
	if (fprintf(w->f, "C %llu %x %u %u %u %u %u %u\n",
	            (unsigned long long)r.frame, r.buttons,
	            r.st[0], r.st[1], r.st[2], r.st[3], r.poll, r.ms) < 0)
		return -1;
	return 1;
}

int input_rec_close_row(InputRecWriter *w)
{
	if (!w || !w->f || !w->ms0_set) return 0;
	/* bound the last hold only when reads happened since the last row (an unchanged write would
	 * repeat poll and fail the loader's ordering rule) */
	if (w->poll <= w->prev.poll) return 0;
	if (fprintf(w->f, "C %llu %x %u %u %u %u %u %u\n",
	            (unsigned long long)w->prev.frame, w->prev.buttons,
	            w->prev.st[0], w->prev.st[1], w->prev.st[2], w->prev.st[3],
	            w->poll, w->last_ms) < 0)
		return -1;
	return 0;
}

void input_rec_loader_init(InputRecLoader *l, int version)
{
	memset(l, 0, sizeof *l);
	l->version = version;
	l->hash = 14695981039346656037ull;   /* FNV-1a 64 offset basis */
}

int input_rec_parse_row(const char *line, int version, InputRecRow *out,
                        char *err, size_t errsz)
{
	InputRecRow r;
	unsigned long long f = 0;
	unsigned long b = 0;
	unsigned v[6] = {0, 0, 0, 0, 0, 0};
	int n, extra;
	char tail[8];

	if (!line || line[0] != 'C' || (line[1] != ' ' && line[1] != '\t')) return 0;
	n = sscanf(line + 1, " %llu %lx %u %u %u %u %u %u %7s",
	           &f, &b, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], tail);
	/* %7s after the fields only assigns when trailing tokens exist (n == 9) */
	extra = (n == 9);
	if (n > 8) n = 8;               /* a trailing token was read, not a field */
	if (n < 6) { snprintf(err, errsz, "short row (%d of 6+ fields)", n < 0 ? 0 : n); return -1; }
	if (version >= 2 && n < 8) { snprintf(err, errsz, "v2 row missing poll/ms columns"); return -1; }
	if (version >= 2 && extra) { snprintf(err, errsz, "unsupported trailing columns"); return -1; }
	if (v[0] > 255 || v[1] > 255 || v[2] > 255 || v[3] > 255) {
		snprintf(err, errsz, "analog byte out of range"); return -1;
	}
	if (b > 0xfffffffful) { snprintf(err, errsz, "button mask exceeds 32 bits"); return -1; }
	memset(&r, 0, sizeof r);
	r.frame   = (uint64_t)f;
	r.buttons = (uint32_t)b;
	r.st[0] = (uint8_t)v[0]; r.st[1] = (uint8_t)v[1];
	r.st[2] = (uint8_t)v[2]; r.st[3] = (uint8_t)v[3];
	r.poll  = (uint32_t)v[4];
	r.ms    = (uint32_t)v[5];
	*out = r;
	return 1;
}

int input_rec_loader_add(InputRecLoader *l, const char *line, InputRecRow *out,
                         char *err, size_t errsz)
{
	InputRecRow r;
	int rc;

	if (!line) return 0;
	l->lines++;
	rc = input_rec_parse_row(line, l->version, &r, err, errsz);
	if (rc <= 0) return rc;
	if (l->have) {
		if (r.frame < l->prev.frame) {
			snprintf(err, errsz, "frame %llu goes backwards (%llu)",
			         (unsigned long long)r.frame, (unsigned long long)l->prev.frame);
			return -1;
		}
		if (l->version >= 2) {          /* ordering guarantees of the live recorder */
			if (r.poll <= l->prev.poll) {
				snprintf(err, errsz, "poll %u not increasing (%u)", r.poll, l->prev.poll);
				return -1;
			}
			if (r.ms < l->prev.ms) {
				snprintf(err, errsz, "ms %u goes backwards (%u)", r.ms, l->prev.ms);
				return -1;
			}
		}
	}
	l->prev = r;
	l->have = 1;
	l->rows++;
	l->hash = input_rec_hash_row(l->hash, &r);
	*out = r;
	return 1;
}

int input_rec_flags_ok(int rec, int play, char *err, size_t errsz)
{
	if (rec && play) {
		snprintf(err, errsz, "RECOMP_INPUT_RECORD and RECOMP_INPUT_REPLAY cannot both be set");
		return -1;
	}
	return 1;
}

int input_rec_latest_at_or_before(const InputRecRow *rows, int n, uint64_t frame)
{
	int lo = 0, hi = n - 1, best = -1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		if (rows[mid].frame <= frame) { best = mid; lo = mid + 1; }
		else hi = mid - 1;
	}
	return best;
}

/* --- replay checkpoint fingerprint ------------------------------------------------------- */

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n)
{
	while (n--) { h ^= *p++; h *= 1099511628211ull; }
	return h;
}
static uint64_t fnv_u64(uint64_t h, uint64_t v)
{
	uint8_t b[8];
	for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));   /* fixed-width little-endian */
	return fnv1a(h, b, 8);
}
static uint64_t fnv_u32(uint64_t h, uint32_t v)
{
	uint8_t b[4];
	for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
	return fnv1a(h, b, 4);
}

uint64_t input_rec_hash_row(uint64_t h, const InputRecRow *r)
{
	h = fnv_u64(h, r->frame);
	h = fnv_u32(h, r->buttons);
	h = fnv1a(h, r->st, 4);
	h = fnv_u32(h, r->poll);
	h = fnv_u32(h, r->ms);
	return h;
}

uint64_t input_rec_fp_hash(const InputRecFp *fp)
{
	uint64_t h = 14695981039346656037ull;
	h = fnv_u32(h, (uint32_t)fp->version);
	h = fnv_u64(h, fp->rows);
	h = fnv_u64(h, fp->trace_hash);
	return h;
}

int input_rec_fp_cmp(const InputRecFp *run, const InputRecFp *base, char *why, size_t why_sz)
{
	if (!run || !base || !why) return 0;
	if (run->state != INPUT_REC_FP_REACHED || base->state != INPUT_REC_FP_REACHED) {
		snprintf(why, why_sz, "checkpoint not reached (run %llu frames, base %llu)",
		         (unsigned long long)run->reached, (unsigned long long)base->reached);
		return 0;
	}
	if (run->version != base->version) {
		snprintf(why, why_sz, "trace version %d vs %d", run->version, base->version);
		return -1;
	}
	if (run->rows != base->rows) {
		snprintf(why, why_sz, "row count %llu vs %llu",
		         (unsigned long long)run->rows, (unsigned long long)base->rows);
		return -1;
	}
	if (run->trace_hash != base->trace_hash) {
		snprintf(why, why_sz, "trace hash %016llx vs %016llx",
		         (unsigned long long)run->trace_hash, (unsigned long long)base->trace_hash);
		return -1;
	}
	if (run->reached != base->reached) {   /* same input identity: presentation alone diverged --
	                                        * thread scheduling or a render divergence; input
	                                        * state alone cannot decide -- report unknown */
		snprintf(why, why_sz, "same input identity, presentation differs (%llu vs %llu frames) -- "
		         "nondeterminism or render divergence, not decidable from input state",
		         (unsigned long long)run->reached, (unsigned long long)base->reached);
		return 0;
	}
	why[0] = 0;
	return 1;
}

int input_rec_fp_print(const InputRecFp *fp)
{
	if (!fp) return -1;
	return fprintf(stderr, "  checkpoint: fp=%016llx version=%d rows=%llu trace=%016llx state=%s reached=%llu\n",
	               (unsigned long long)input_rec_fp_hash(fp), fp->version,
	               (unsigned long long)fp->rows, (unsigned long long)fp->trace_hash,
	               fp->state == INPUT_REC_FP_REACHED ? "reached" : "unreached",
	               (unsigned long long)fp->reached) < 0 ? -1 : 0;
}
