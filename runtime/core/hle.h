/* The host's implementations of the Vita's system calls, by import name (hle.c). */
#ifndef RECOMP_HLE_H
#define RECOMP_HLE_H
#include "rt.h"

typedef void (*HleFn)(Cpu *c);
typedef struct { const char *name; HleFn fn; } HleEntry;   /* a subsystem's table, {0, 0}-terminated */
uint64_t guest_time_us(void);   /* display.c: the time the game sees (RECOMP_CLOCK=fixed: deterministic) */
int guest_clock_fixed(void);
void guest_time_slept(uint32_t us);   /* display.c: a thread slept this long (the fixed clock advances by it) */
extern const HleEntry hle_kernel[], hle_display[], hle_ngs[], hle_libc[], hle_io[], hle_sysmem[], hle_apputil[], hle_fios[], hle_gpu[], hle_video[], hle_gesture[], hle_jpeg[], hle_camera[], hle_net[], hle_np[], hle_http[], hle_ssl_local[];

/* a system call's arguments: r0-r3, then the stack (the 5th is STK(0)); the result in r0 */
#define A0 c->r[0]
#define A1 c->r[1]
#define A2 c->r[2]
#define A3 c->r[3]
#define STK(n) RD32(c->r[13] + 4 * (n))
#define RET(v) (c->r[0] = (uint32_t)(v))
HleFn hle_find(const char *name);
void hle_init(Cpu *c);
/* an imported variable's slot (mkimage.py): the subsystem that owns it fills it in */
void hle_import_var(const char *name, uint32_t addr);
void hle_import_vars_done(void);
uint32_t hle_var_addr(const char *name);
int libc_import_var(const char *name, uint32_t addr);
int cxx_import_var(const char *name, uint32_t addr);
uint32_t guest_alloc(uint32_t size, uint32_t align);
uint32_t gpu_dev_to_cpu(uint32_t dev);   /* the CPU memory behind a GPU device address (gpu.c) */
uint32_t gpu_render_target_info(uint32_t handle, uint32_t *width, uint32_t *height);   /* samples per pixel x:16 y:16, 0 unknown (gpu.c) */
int guest_mem_ranges(uint32_t lo[], uint32_t hi[], int max);   /* image segments + heap in use */
uint32_t guest_call(Cpu *parent, uint32_t fn, uint32_t a0, uint32_t a1);   /* run guest code from a system call */
uint32_t guest_call4(Cpu *parent, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);   /* the same with four arguments (kernel.c) */
/* kernel callbacks (kernel.c) for display.c's vblank callback: a notification (count: how many; the
 * owner runs it at its next CheckCallback/CB wait), the owner's pending ones run now, a sleep to a
 * CLOCK_MONOTONIC time that runs them as they come; display_vblank_callbacks (display.c) notifies
 * the registered vblank callbacks with the edges since their last notification */
void kernel_notify_callback(uint32_t uid, uint32_t count, uint32_t arg);
int kernel_callback_valid(uint32_t uid);
uint32_t kernel_check_callbacks(Cpu *c);
void kernel_cb_sleep(Cpu *c, uint64_t until_us);
void display_vblank_callbacks(void);
/* the guest-thread scheduler (kernel.c, RECOMP_SCHED=vita; every call a no-op while it is off):
 * sched_block_begin before anything that sleeps (gives the run slot up; safe under host locks) ->
 * 1 if sched_block_end must follow the sleep (waits for a slot again: never with a host lock held);
 * sched_yield_if_preempted at the system-call boundary (main.c's import dispatch) */
extern int recomp_sched_on;
void kernel_sched_init(void);
int sched_block_begin(void);
void sched_block_end(void);
void sched_yield_if_preempted(void);
void recomp_runfast(void);
extern char hle_last_format[1024];
#endif
