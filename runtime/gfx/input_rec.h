/* input_rec.h -- `# nativ-input v2`: the pure recording/replay format logic.
 *
 * Encode, parse, validate and the replay's frame lookup, with no runtime or SDL dependency so
 * runtime/tests/input_rec_test.c can drive it directly. display.c uses it at one point only: the
 * delivered pad state in ctrl_peek (what guest code actually reads, after window mapping,
 * RECOMP_INPUT script and replay).
 *
 * File shape (v2): a version header, `#` comments, and one `C` row per delivered-input CHANGE
 * (a held state adds no row -- the run-length encoding; the span row -> next row is the hold).
 * v1 files (no header, 6 columns) still load.
 */
#ifndef INPUT_REC_H
#define INPUT_REC_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define INPUT_REC_VERSION 2

/* one delivered pad state: `frame` = display_frames() at the read (0 before the first present and
 * frozen while nothing renders), `poll` = a monotonic per-read counter from the recording's first
 * read, `ms` = monotonic milliseconds since that read, then the delivered mask and analog bytes */
typedef struct {
	uint64_t frame;
	uint32_t poll;
	uint32_t ms;
	uint32_t buttons;
	uint8_t  st[4];          /* lx ly rx ry, as delivered */
} InputRecRow;

/* writer side: change-only rows, one v2 header, best effort (a row error returns, never dies) */
typedef struct {
	FILE        *f;
	InputRecRow  prev;
	int          have;       /* a previous state exists */
	uint32_t     poll;       /* one count per observed read */
	uint64_t     ms0;        /* monotonic origin, set at the first read */
	int          ms0_set;
	uint32_t     last_ms;    /* ms of the most recent read, seen or not */
} InputRecWriter;

/* loader side: cross-row validation (ordering) while rows are added */
typedef struct {
	InputRecRow  prev;
	int          have;
	int          version;    /* 0 = no header (legacy), else INPUT_REC_VERSION */
	int          rows;       /* rows added */
	int          lines;      /* lines examined */
	uint64_t     hash;       /* FNV-1a over the rows added (the trace identity) */
} InputRecLoader;

/* the file's first line: a version header -> 1 with *version set; unknown nativ-input version ->
 * -1; anything else -> 0 (comment or row, left to the caller) */
int input_rec_header_check(const char *line, int *version);
/* the header a recorder writes: 0 ok, -1 write error */
int input_rec_header_write(FILE *f);

void input_rec_writer_init(InputRecWriter *w, FILE *f);
/* once per guest read with the delivered state: bumps poll, and writes a row only when the state
 * changed (or none written yet). 1 row written, 0 unchanged, -1 write error */
int  input_rec_observe(InputRecWriter *w, uint64_t frame, uint64_t now_us,
                       const uint32_t *buttons, const uint8_t st[4]);
/* the final row on graceful exit (bounds the last hold): 0 ok, -1 write error */
int  input_rec_close_row(InputRecWriter *w);

void input_rec_loader_init(InputRecLoader *l, int version);
/* one row line: 1 parsed into *out (and ordered against previous rows), 0 not a state row,
 * -1 corrupt with a reason in err */
int  input_rec_loader_add(InputRecLoader *l, const char *line, InputRecRow *out,
                          char *err, size_t errsz);
/* a single row parse + field validation, without ordering (for direct tests) */
int  input_rec_parse_row(const char *line, int version, InputRecRow *out,
                         char *err, size_t errsz);

/* live record + replay together is refused: 1 allowed, -1 refused with a reason in err */
int  input_rec_flags_ok(int rec, int play, char *err, size_t errsz);
/* the replay lookup: the largest index with rows[i].frame <= frame, else -1; rows sorted by frame */
int  input_rec_latest_at_or_before(const InputRecRow *rows, int n, uint64_t frame);

/* --- replay checkpoint fingerprint -------------------------------------------------------
 * A small stable, RUNTIME-OBSERVABLE identity of what a replay run actually did: the loaded
 * trace's format/content, and how far the run presented frames at the checkpoint. Compared
 * strictly on the stable fields; an unreached checkpoint reports unknown rather than a mismatch
 * (a run may die early -- no invented hashes). */
#define INPUT_REC_FP_UNREACHED 0
#define INPUT_REC_FP_REACHED   1
typedef struct {
	int      version;       /* trace version read (0 = legacy) */
	uint64_t rows;          /* C rows loaded */
	uint64_t trace_hash;    /* FNV-1a 64 over every loaded C row, fields fixed-width LE */
	uint64_t reached;       /* presented frames at the checkpoint (display_frames()) */
	int      state;         /* INPUT_REC_FP_REACHED once the checkpoint was hit */
} InputRecFp;
/* FNV-1a over the stable fields (version, rows, trace_hash); state/reached are NOT part of the
 * hash: they say whether the comparison is meaningful, not what was observed identically */
uint64_t input_rec_fp_hash(const InputRecFp *fp);
/* 1 = matching fingerprints, -1 = a stable field differs (why says which), 0 = unknown (either
 * side's checkpoint was not reached, or no trace was loaded) */
int input_rec_fp_cmp(const InputRecFp *run, const InputRecFp *base, char *why, size_t why_sz);
/* the `  checkpoint:` line a replay run prints at exit (stable facts only) */
int input_rec_fp_print(const InputRecFp *fp);
/* feed one loaded C row into a running FNV-1a (loader side); starts from the empty-stream value */
uint64_t input_rec_hash_row(uint64_t h, const InputRecRow *r);
#endif
