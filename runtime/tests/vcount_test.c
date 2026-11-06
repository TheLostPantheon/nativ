/* vcount_test.c -- DISP-01: sceDisplayGetVcount (runtime/gfx/display.c, hle_display[]). The import was
 * unimplemented (the generic stub returned 0 forever), so a game polling the count for change hung at its
 * first frame. The handler reports the vblanks since the display started, 60 a second on the host's
 * monotonic clock, anchored at the first read.
 *
 * Cases: registered under its import name; the first read is the anchor (0); two reads at least 1/60 s
 * apart differ and the count only moves forward; 500 ms at 60/s reads as >= 25 vblanks (lower bound only:
 * a loaded host only ever makes the sleep longer).
 *
 * Two translation units: this one and ../gfx/display.c compiled separately (a fail-before run compiles an
 * older copy of display.c in its place on the build line). The stubs below satisfy display.c's link by NAME
 * only -- none of them is called on the vcount path. Build and run: see README.md here.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../core/hle.h"   /* Cpu, HleEntry, hle_display[] */

/* ---- display.c's link surface, by name ---- */
uint8_t *recomp_mem;
uint32_t recomp_watch_lo = 0, recomp_watch_hi = 0;
void recomp_watch_write(uint32_t a, uint32_t v, int width) { (void)a; (void)v; (void)width; }
const char *recomp_cur_tname(void) { return "vcount_test"; }
void audio_open(void) {}
void audio_output(void) {}
void ev_log(void) {}
void gfx_gpu_last_frame_costs(void) {}
void gfx_present(void) {}
void hook_seq_arm(void) {}
void input_rec_close_row(void) {}
void input_rec_flags_ok(void) {}
void input_rec_fp_print(void) {}
void input_rec_frame_at_ms(void) {}
void input_rec_header_check(void) {}
void input_rec_header_write(void) {}
void input_rec_latest_at_or_before(void) {}
void input_rec_latest_ms_at_or_before(void) {}
void input_rec_loader_add(void) {}
void input_rec_loader_init(void) {}
void input_rec_observe(void) {}
void input_rec_replay_key_parse(void) {}
void input_rec_writer_init(void) {}
void io_read_stats_take(void) {}
int kernel_callback_valid(uint32_t uid) { (void)uid; return 0; }
void kernel_cb_sleep(Cpu *c, uint64_t until_us) { (void)c; (void)until_us; }
uint32_t kernel_check_callbacks(Cpu *c) { (void)c; return 0; }
void kernel_notify_callback(uint32_t uid, uint32_t count, uint32_t arg) { (void)uid; (void)count; (void)arg; }
void memscan_frame(void) {}
void metrics_game_frame(void) {}
int sched_block_begin(void) { return 0; }
void sched_block_end(void) {}
void motion_live(void) {}
void motion_matrix(void) {}
void motion_recenter(void) {}
void overlay_game_paused(void) {}
void overlay_layout_ask(void) {}
int settings_int(void) { return 0; }
void stallprof_present(void) {}
void sysmem_dump_maybe(void) {}
void video_last_picture_frame(void) {}
void waitstats_present(void) {}
void window_active(void) {}
void window_gamepad_connected(void) {}
void window_input(void) {}
void window_input_pad2(void) {}
void window_touch_contacts(void) {}

static int fails;
static void check(const char *what, int ok) {
	printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok) fails++;
}

static HleFn find(const char *name) {
	for (const HleEntry *e = hle_display; e->name; e++)
		if (!strcmp(e->name, name)) return e->fn;
	return 0;
}

static uint32_t read_vcount(HleFn vcount) {
	Cpu cpu;
	memset(&cpu, 0, sizeof cpu);
	vcount(&cpu);
	return cpu.r[0];
}

int main(void) {
	HleFn vcount = find("sceDisplayGetVcount");
	check("sceDisplayGetVcount is registered in hle_display", vcount != 0);
	if (!vcount) { printf("1 check FAILED\n"); return 1; }

	uint32_t v0 = read_vcount(vcount);
	check("the first read is the anchor (0)", v0 == 0);

	struct timespec ts = { 0, 30 * 1000 * 1000 };   /* 30 ms, ~2 vblanks */
	nanosleep(&ts, 0);
	uint32_t v1 = read_vcount(vcount);
	check("two reads 30 ms apart differ", v1 != v0);
	check("the count only moves forward", v1 >= v0);

	ts.tv_nsec = 500 * 1000 * 1000;   /* 500 ms, ~30 vblanks at 60/s */
	nanosleep(&ts, 0);
	uint32_t v2 = read_vcount(vcount);
	check("500 ms later: >= 25 vblanks (60/s)", v2 - v1 >= 25);

	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed (4 ok)\n");
	return 0;
}
