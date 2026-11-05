/* input_rec_test.c -- the `# nativ-input v2` format (runtime/gfx/input_rec.c) against the
 * fixture list: encode/decode round trip, neutral spans, quick taps, held buttons,
 * frame gaps, malformed traces, replay EOF, mask edges and record/replay exclusivity. Pure logic,
 * no runtime, no SDL -- see runtime/tests/README.md for how to run it. */
#include "input_rec.h"
#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t NEUTRAL[4] = {128, 128, 128, 128};
static const uint8_t LEFT[4]    = {0, 128, 128, 128};

/* --- header validation ------------------------------------------------------------------- */
static void test_header(void)
{
	int v = -1;
	CK(input_rec_header_check("# nativ-input v2\n", &v) == 1 && v == 2);
	CK(input_rec_header_check("# origin frame=0 poll=0 ms=0\n", &v) == 0);
	CK(input_rec_header_check("# nativ-input v99\n", &v) == -1);   /* newer reader refuses loudly */
	CK(input_rec_header_check("# nativ-input banana\n", &v) == -1);
	CK(input_rec_header_check("C 10 4000 128 128 128 128 3 0\n", &v) == 0);
}

/* --- round trip: observe -> file -> parse ------------------------------------------------- */
static void test_round_trip(void)
{
	FILE *f = tmpfile();
	InputRecWriter w;
	InputRecLoader l;
	InputRecRow r, out;
	char line[128], err[128];
	int v = 0, got;
	uint32_t b = 0x4000;   /* CROSS, plus sticks 0 / 128 / 255 over the rows */
	uint8_t st[4] = {0, 128, 255, 128};

	CK(f != NULL);
	CK(input_rec_header_write(f) == 0);
	input_rec_writer_init(&w, f);
	CK(input_rec_observe(&w, 7, 1000000, &b, st) == 1);      /* first read always writes */
	b = 0;
	CK(input_rec_observe(&w, 7, 1005000, &b, NEUTRAL) == 1);  /* release */
	CK(input_rec_observe(&w, 12, 1010000, &b, NEUTRAL) == 0); /* held -> no row */
	CK(input_rec_close_row(&w) == 0);                          /* reads happened -> bounds it */
	rewind(f);

	/* the loader's exact flow: header, then rows */
	input_rec_loader_init(&l, 0);
	CK(fgets(line, sizeof line, f));
	got = input_rec_header_check(line, &v);
	CK(got == 1 && v == 2);
	l.version = v;
	while (fgets(line, sizeof line, f)) {
		got = input_rec_loader_add(&l, line, &out, err, sizeof err);
		CK(got >= 0);
		if (got < 0) return;
	}
	CK(l.rows == 3);   /* press, release, close-row */
	/* field-exact: first row */
	rewind(f);
	CK(fgets(line, sizeof line, f));          /* header */
	CK(fgets(line, sizeof line, f));          /* row 1 */
	CK(input_rec_parse_row(line, 2, &r, err, sizeof err) == 1);
	CK(r.frame == 7 && r.buttons == 0x4000 && r.poll == 1 && r.ms == 0);
	CK(r.st[0] == 0 && r.st[1] == 128 && r.st[2] == 255 && r.st[3] == 128);
	/* close row carries the last state at the newest poll */
	CK(input_rec_parse_row("C 12 0 128 128 128 128 3 10\n", 2, &r, err, sizeof err) == 1);
	CK(r.poll == 3 && r.ms == 10);
	fclose(f);
}

/* --- neutral spans and quick taps (change-only rows, exact edges) ------------------------- */
static void test_rle_and_taps(void)
{
	FILE *f = tmpfile();
	InputRecWriter w;
	uint32_t b = 0;
	int i, rows = 0;
	char line[128];

	CK(f != NULL);
	input_rec_writer_init(&w, f);
	for (i = 0; i < 10; i++) CK(input_rec_observe(&w, 3, 1000000 + i * 16666, &b, NEUTRAL) <= 1);
	rewind(f);
	while (fgets(line, sizeof line, f)) rows++;
	CK(rows == 1);                                  /* one neutral row, not ten */
	fclose(f);

	/* a quick tap: down at read 1, up at read 2, same frame -- rows through the loader */
	f = tmpfile(); CK(f != NULL);
	CK(input_rec_header_write(f) == 0);
	input_rec_writer_init(&w, f);
	b = 0x4000 | 0x1000 | 0x8;
	CK(input_rec_observe(&w, 5, 2000000, &b, NEUTRAL) == 1);
	b = 0;
	CK(input_rec_observe(&w, 5, 2003000, &b, NEUTRAL) == 1);
	rewind(f);
	{
		InputRecLoader l;
		InputRecRow r1, r2, out;
		char err[64], line2[128];
		int v = 0;
		input_rec_loader_init(&l, 0);
		CK(fgets(line2, sizeof line2, f));
		CK(input_rec_header_check(line2, &v) == 1);
		l.version = v;
		CK(fgets(line2, sizeof line2, f) && input_rec_loader_add(&l, line2, &out, err, sizeof err) == 1);
		r1 = out;
		CK(fgets(line2, sizeof line2, f) && input_rec_loader_add(&l, line2, &out, err, sizeof err) == 1);
		r2 = out;
		CK(!fgets(line2, sizeof line2, f));          /* exactly two rows: the tap */
		CK(r1.poll == 1 && r2.poll == 2);            /* consecutive reads, no lost tap */
		CK(r1.buttons == (0x4000 | 0x1000 | 0x8));   /* down edge holds every bit */
		CK(r2.buttons == 0);                         /* up edge is an explicit row */
		CK(r1.frame == 5 && r2.frame == 5);
	}
	fclose(f);
}

/* --- held buttons: one row spans the hold (polls/frames move on silently) ----------------- */
static void test_hold(void)
{
	FILE *f = tmpfile();
	InputRecWriter w;
	InputRecLoader l;
	InputRecRow out;
	char err[64], line[128];
	uint32_t b = 0x4000;
	int i, v = 0, rows = 0;

	CK(f != NULL);
	CK(input_rec_header_write(f) == 0);
	input_rec_writer_init(&w, f);
	CK(input_rec_observe(&w, 4, 3000000, &b, NEUTRAL) == 1);           /* press */
	for (i = 1; i <= 10; i++)
		CK(input_rec_observe(&w, 4 + i, 3000000 + i * 16666, &b, NEUTRAL) == 0); /* held over frames */
	CK(input_rec_observe(&w, 20, 3400000, &b, LEFT) == 1);             /* stick change only */
	rewind(f);
	input_rec_loader_init(&l, 0);
	CK(fgets(line, sizeof line, f) && input_rec_header_check(line, &v) == 1);
	l.version = v;
	while (fgets(line, sizeof line, f)) {
		CK(input_rec_loader_add(&l, line, &out, err, sizeof err) == 1);
		rows++;
	}
	CK(rows == 2);
	/* frame gaps (jittered title arrival) are legal: only backwards is not */
	CK(input_rec_parse_row("C 20 4000 128 128 128 128 12 400\n", 2, &out, err, sizeof err) == 1);
	CK(out.frame == 20);
	fclose(f);
}

/* --- malformed traces --------------------------------------------------------------------- */
static void test_malformed(void)
{
	InputRecRow r;
	char err[128];
	InputRecLoader l;

	CK(input_rec_parse_row("C 10 4000 128 128 128\n", 2, &r, err, sizeof err) == -1);   /* missing  */
	CK(err[0]);                                                                          /* ...message */
	CK(input_rec_parse_row("C 10 4000 128 128 128 128\n", 2, &r, err, sizeof err) == -1); /* v2 no poll/ms */
	CK(input_rec_parse_row("C 10 4000 128 300 128 128 1 0\n", 2, &r, err, sizeof err) == -1); /* stick >255 */
	CK(input_rec_parse_row("C 10 4000 128 128 128 128 1 0 junk\n", 2, &r, err, sizeof err) == -1); /* unsupported */
	CK(input_rec_parse_row("C 10 zz 128 128 128 128 1 0\n", 2, &r, err, sizeof err) == -1); /* bad hex */
	CK(input_rec_parse_row("C 10 100000000 128 128 128 128 1 0\n", 2, &r, err, sizeof err) == -1); /* >32 bits */
	CK(input_rec_parse_row("X 10 4000\n", 2, &r, err, sizeof err) == 0);                  /* not a row */
	CK(input_rec_parse_row("T 10 1234567890\n", 2, &r, err, sizeof err) == 0);            /* legacy T skips */
	/* legacy v1 rows (6 columns) still load at version 0 */
	CK(input_rec_parse_row("C 10 4000 128 128 128 128\n", 0, &r, err, sizeof err) == 1);
	CK(r.poll == 0 && r.ms == 0);

	/* ordering rules, per row pair */
	input_rec_loader_init(&l, 2);
	CK(input_rec_loader_add(&l, "C 10 0 128 128 128 128 5 100\n", &r, err, sizeof err) == 1);
	CK(input_rec_loader_add(&l, "C 9 0 128 128 128 128 6 200\n", &r, err, sizeof err) == -1);   /* frame back */
	input_rec_loader_init(&l, 2);
	CK(input_rec_loader_add(&l, "C 10 0 128 128 128 128 5 100\n", &r, err, sizeof err) == 1);
	CK(input_rec_loader_add(&l, "C 11 0 128 128 128 128 5 200\n", &r, err, sizeof err) == -1);   /* poll repeat */
	input_rec_loader_init(&l, 2);
	CK(input_rec_loader_add(&l, "C 10 0 128 128 128 128 5 100\n", &r, err, sizeof err) == 1);
	CK(input_rec_loader_add(&l, "C 11 0 128 128 128 128 6 50\n", &r, err, sizeof err) == -1);    /* ms back */
}

/* --- replay EOF: serve the last state past the end ---------------------------------------- */
static void test_eof_lookup(void)
{
	InputRecRow rows[3];
	memset(rows, 0, sizeof rows);
	rows[0].frame = 10; rows[1].frame = 20; rows[2].frame = 30;
	CK(input_rec_latest_at_or_before(rows, 3, 0) == -1);     /* nothing before the first row */
	CK(input_rec_latest_at_or_before(rows, 3, 10) == 0);
	CK(input_rec_latest_at_or_before(rows, 3, 19) == 0);     /* in a gap: the held row */
	CK(input_rec_latest_at_or_before(rows, 3, 20) == 1);
	CK(input_rec_latest_at_or_before(rows, 3, 1000000) == 2); /* past EOF: hold the last state */
	CK(input_rec_latest_at_or_before(rows, 0, 5) == -1);      /* empty trace */
}

/* --- record + replay together is refused; masks stay whole -------------------------------- */
static void test_flags_and_masks(void)
{
	char err[128];
	InputRecRow r;

	CK(input_rec_flags_ok(1, 1, err, sizeof err) == -1 && err[0]);
	CK(input_rec_flags_ok(1, 0, err, sizeof err) == 1);
	CK(input_rec_flags_ok(0, 1, err, sizeof err) == 1);
	CK(input_rec_flags_ok(0, 0, err, sizeof err) == 1);
	/* simultaneous controls survive: every Vita button bit plus analog extremes */
	CK(input_rec_parse_row("C 3 ffffffff 0 0 255 255 9 4\n", 2, &r, err, sizeof err) == 1);
	CK(r.buttons == 0xffffffffu && r.st[0] == 0 && r.st[3] == 255);
	CK(input_rec_parse_row("C 3 0 255 255 0 0 9 4\n", 2, &r, err, sizeof err) == 1);
	CK(r.buttons == 0 && r.st[0] == 255 && r.st[2] == 0);
}

/* --- checkpoint fingerprint: repeatable state matches, a changed stable field is
 * detected, an unreached checkpoint reports unknown rather than a mismatch ------------------ */
static void test_fingerprint(void)
{
	InputRecFp a = {2, 3, 0x1234abcd5678ull, 230, INPUT_REC_FP_REACHED};
	InputRecFp b = a;                 /* the same facts, a second run */
	InputRecFp c = a;                 /* a deliberately changed stable field */
	char why[128];

	CK(input_rec_fp_hash(&a) == input_rec_fp_hash(&b));
	CK(input_rec_fp_cmp(&a, &b, why, sizeof why) == 1 && why[0] == 0);
	c.rows = 4;                                                        /* stable field changed */
	CK(input_rec_fp_hash(&c) != input_rec_fp_hash(&a));
	CK(input_rec_fp_cmp(&c, &a, why, sizeof why) == -1 && why[0]);
	c = a; c.trace_hash ^= 1;
	CK(input_rec_fp_cmp(&c, &a, why, sizeof why) == -1 && strstr(why, "hash") != NULL);
	c = a; c.version = 1;
	CK(input_rec_fp_cmp(&c, &a, why, sizeof why) == -1 && strstr(why, "version") != NULL);
	c = a; c.state = INPUT_REC_FP_UNREACHED; c.reached = 120;         /* died early: UNKNOWN */
	CK(input_rec_fp_cmp(&c, &a, why, sizeof why) == 0 && strstr(why, "not reached") != NULL);
	c = a; c.reached = 300;                 /* same input identity, presentation differs: UNKNOWN */
	CK(input_rec_fp_cmp(&c, &a, why, sizeof why) == 0 && strstr(why, "presentation") != NULL);
	CK(input_rec_fp_print(&a) == 0);       /* the `  checkpoint:` line renders */

	/* loader-derived identity: identical rows hash identically, one changed field does not */
	{
		InputRecLoader l1, l2, l3;
		InputRecRow r;
		char err[64];
		static const char *rows[] = {
			"C 1 4000 128 128 128 128 1 0\n",
			"C 5 0 128 128 128 128 9 40\n"};
		input_rec_loader_init(&l1, 2);
		input_rec_loader_init(&l2, 2);
		input_rec_loader_init(&l3, 2);
		for (size_t i = 0; i < 2; i++) {
			CK(input_rec_loader_add(&l1, rows[i], &r, err, sizeof err) == 1);
			CK(input_rec_loader_add(&l2, rows[i], &r, err, sizeof err) == 1);
		}
		CK(l1.rows == 2 && l1.hash == l2.hash);
		CK(input_rec_loader_add(&l3, rows[0], &r, err, sizeof err) == 1);
		CK(l3.hash != l1.hash);                 /* fewer rows, different identity */
	}
}

int main(void)
{
	test_header();
	test_round_trip();
	test_rle_and_taps();
	test_hold();
	test_malformed();
	test_eof_lookup();
	test_flags_and_masks();
	test_fingerprint();
	printf("%d checks, %d failed\n", checks, failed);
	return failed ? 1 : 0;
}
