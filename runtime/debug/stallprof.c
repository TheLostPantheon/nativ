/*
 * RECOMP_STALL_PROF=ms (vitarecomp run: 150): when no frame has been presented for that long, every thread's call
 * stack is sampled every 2 ms until the next present, and the stall's most common stacks are logged by thread --
 * recompiled guest functions by their fn_XXXXXXXX names, host code by its own -- so a stutter with nothing to show
 * in the hitch log (the renderer idle, no file reads: Killzone's 300-440 ms stalls at a new effect or UI, 2026-10-17)
 * says what the game was doing instead of being guessed at. macOS only (Mach thread states, frame-pointer walks);
 * elsewhere a no-op. Threads are suspended only while their registers and frame chain are copied into a static
 * buffer; names are looked up after the stall, with every thread running (dladdr may lock). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

void stallprof_present(void);

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#define DEPTH 14
#define MAXS 8192
typedef struct { thread_t th; uint64_t pc[DEPTH]; int n; } Sample;
static Sample samples[MAXS];
static int nsamples;
static volatile uint64_t last_present_us;
static int threshold_ms;
static pthread_t monitor;

static uint64_t now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u; }
static int read_words(uint64_t addr, uint64_t out[2]) {
	mach_vm_size_t got = 0;
	return mach_vm_read_overwrite(mach_task_self(), addr, 16, (mach_vm_address_t)out, &got) == KERN_SUCCESS && got == 16;
}
static void sample_all(thread_t self) {
	thread_act_array_t th; mach_msg_type_number_t n;
	if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return;
	for (mach_msg_type_number_t i = 0; i < n; i++) {
		if (th[i] == self || nsamples >= MAXS) { mach_port_deallocate(mach_task_self(), th[i]); continue; }
		if (thread_suspend(th[i]) == KERN_SUCCESS) {
			arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
			if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
				Sample *s = &samples[nsamples++]; s->th = th[i]; s->n = 0;
				s->pc[s->n++] = arm_thread_state64_get_pc(st);
				s->pc[s->n++] = arm_thread_state64_get_lr(st);
				uint64_t fp = arm_thread_state64_get_fp(st), w[2];
				while (s->n < DEPTH && fp && !(fp & 7) && read_words(fp, w)) {   /* {saved fp, return address} */
					if (w[1]) s->pc[s->n++] = w[1] & 0x0000ffffffffffffull;   /* (pointer-authentication bits off) */
					if (w[0] <= fp) break;
					fp = w[0];
				}
			}
			thread_resume(th[i]);
		}
		/* (the port is kept: the report names the thread through it, then lets it go) */
	}
	vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
}
static const char *sym(uint64_t pc) {
	Dl_info di; static char buf[64];
	if (dladdr((void *)pc, &di) && di.dli_sname) return di.dli_sname;
	snprintf(buf, sizeof buf, "%#llx", (unsigned long long)pc); return buf;
}
typedef struct { char key[512]; int count; } Stack;
static void report(uint64_t stall_us) {
	static Stack st[256]; int nst = 0;
	for (int i = 0; i < nsamples; i++) {
		Sample *s = &samples[i]; char name[64] = "?", key[512];
		pthread_t pt = pthread_from_mach_thread_np(s->th);
		if (pt) pthread_getname_np(pt, name, sizeof name);
		int k = snprintf(key, sizeof key, "[%s]", name[0] ? name : "?");
		char prev[64] = "";
		for (int f = 0; f < s->n && k < (int)sizeof key - 40; f++) {
			const char *nm = sym(s->pc[f]);
			if (!strcmp(prev, nm)) continue;   /* (pc and lr in one function) */
			k += snprintf(key + k, sizeof key - (size_t)k, " < %s", nm);
			snprintf(prev, sizeof prev, "%s", nm);
		}
		int j; for (j = 0; j < nst && strcmp(st[j].key, key); j++);
		if (j == nst) { if (nst == 256) continue; snprintf(st[nst].key, sizeof st[nst].key, "%s", key); st[nst++].count = 0; }
		st[j].count++;
	}
	for (int a = 1; a < nst; a++) for (int b = a; b > 0 && st[b].count > st[b - 1].count; b--) { Stack t = st[b]; st[b] = st[b - 1]; st[b - 1] = t; }
	fprintf(stderr, "    stall: %.0f ms without a present, %d thread samples; most common stacks (count, [thread] innermost < callers):\n", stall_us / 1e3, nsamples);
	/* running first (what took the time), then blocked in the kernel (who waits on whom): a stack whose innermost
	 * frame is a kernel wait -- condition variable, semaphore, sleep, Mach message, an idle work-queue thread */
	static const char *const waits[] = {"__psynch_", "__workq_kernreturn", "mach_msg", "__semwait", "__ulock_wait", "__select", "kevent", "__wait4"};
	for (int pass = 0; pass < 2; pass++) {
		int shown = 0;
		for (int i = 0; i < nst && shown < 10; i++) {
			const char *in = strstr(st[i].key, " < "); in = in ? in + 3 : st[i].key;
			int waiting = 0; for (size_t w = 0; w < sizeof waits / sizeof *waits; w++) if (!strncmp(in, waits[w], strlen(waits[w]))) waiting = 1;
			if (waiting != pass || (pass && strstr(st[i].key, "start_wqthread"))) continue;
			fprintf(stderr, "    stall:   %s %4d %s\n", pass ? "waiting" : "running", st[i].count, st[i].key); shown++;
		}
	}
	/* per thread: how much of the stall it ran and where (its innermost recompiled functions), how much it waited and in
	 * what (the host handler of the import it called, and the guest function that called it) -- the stack lists above
	 * split one busy thread across hundreds of one-sample stacks */
	{	typedef struct { thread_t th; char name[48]; int n, run; char rk[6][96]; int rc[6], nr; char wk[4][96]; int wc[4], nw; } Th;
		static Th ths[96]; int nth = 0;
		for (int i = 0; i < nsamples; i++) {
			Sample *sm = &samples[i]; Th *t = 0;
			for (int k = 0; k < nth; k++) if (ths[k].th == sm->th) t = &ths[k];
			if (!t) { if (nth == 96) continue; t = &ths[nth++]; memset(t, 0, sizeof *t); t->th = sm->th;
				pthread_t pt = pthread_from_mach_thread_np(sm->th); if (pt) pthread_getname_np(pt, t->name, sizeof t->name); if (!t->name[0]) snprintf(t->name, sizeof t->name, "?"); }
			t->n++;
			const char *in = sm->n ? sym(sm->pc[0]) : "?"; int waiting = 0;
			for (size_t w = 0; w < sizeof waits / sizeof *waits; w++) if (!strncmp(in, waits[w], strlen(waits[w]))) waiting = 1;
			char key[96] = "";
			if (!waiting) {   /* the innermost recompiled function on the stack */
				for (int f = 0; f < sm->n && !key[0]; f++) { const char *nm = sym(sm->pc[f]); if (!strncmp(nm, "fn_", 3)) snprintf(key, sizeof key, "%s", nm); }
				if (!key[0]) snprintf(key, sizeof key, "%s", in);
				t->run++;
				int k; for (k = 0; k < t->nr && strcmp(t->rk[k], key); k++);
				if (k == t->nr && t->nr < 6) { snprintf(t->rk[t->nr], sizeof t->rk[0], "%s", key); t->rc[t->nr++] = 0; }
				if (k < 6) t->rc[k]++;
			} else {   /* the import it waits in: the frame below recomp_call_external, and its guest caller */
				const char *h = 0, *g = 0;
				for (int f = 0; f + 1 < sm->n; f++) { const char *nm = sym(sm->pc[f + 1]); if (!strcmp(nm, "recomp_call_external")) { static char hb[64]; snprintf(hb, sizeof hb, "%s", sym(sm->pc[f])); h = hb; if (f + 2 < sm->n) g = sym(sm->pc[f + 2]); break; } }
				if (!h) continue;   /* a host thread's own idle wait: not reported */
				snprintf(key, sizeof key, "%s from %s", h, g ? g : "?");
				int k; for (k = 0; k < t->nw && strcmp(t->wk[k], key); k++);
				if (k == t->nw && t->nw < 4) { snprintf(t->wk[t->nw], sizeof t->wk[0], "%s", key); t->wc[t->nw++] = 0; }
				if (k < 4) t->wc[k]++;
			}
		}
		for (int k = 0; k < nth; k++) {
			Th *t = &ths[k]; int wsum = 0; for (int j = 0; j < t->nw; j++) wsum += t->wc[j];
			if (!t->n || (t->run * 10 < t->n && !wsum) || (t->run * 5 < t->n && wsum * 5 < t->n)) continue;   /* busy 10%+, or 20%+ waiting in a guest import */
			char line[1024]; int o = snprintf(line, sizeof line, "    stall:   thread [%s] running %d%%", t->name, 100 * t->run / t->n);
			for (int a2 = 1; a2 < t->nr; a2++) for (int b2 = a2; b2 > 0 && t->rc[b2] > t->rc[b2 - 1]; b2--) { int c2 = t->rc[b2]; t->rc[b2] = t->rc[b2 - 1]; t->rc[b2 - 1] = c2; char tmp[96]; memcpy(tmp, t->rk[b2], 96); memcpy(t->rk[b2], t->rk[b2 - 1], 96); memcpy(t->rk[b2 - 1], tmp, 96); }
			for (int j = 0; j < t->nr && j < 4 && o < (int)sizeof line - 100; j++) o += snprintf(line + o, sizeof line - (size_t)o, "%s %s %d%%", j ? "," : " (in", t->rk[j], 100 * t->rc[j] / t->n);
			if (t->nr) o += snprintf(line + o, sizeof line - (size_t)o, ")");
			for (int j = 0; j < t->nw && o < (int)sizeof line - 100; j++) o += snprintf(line + o, sizeof line - (size_t)o, "%s %s %d%%", j ? "," : "; waiting in", t->wk[j], 100 * t->wc[j] / t->n);
			fprintf(stderr, "%s\n", line);
		}
	}
	for (int i = 0; i < nsamples; i++) mach_port_deallocate(mach_task_self(), samples[i].th);
}
extern void (*hook_stall_long)(uint64_t stall_us);   /* hooks.c: a game's own diagnostics for a long stall (Killzone's job trace) */
static void *monitor_main(void *arg) {
	(void)arg;
	pthread_setname_np("stallprof");
	thread_t self = mach_thread_self();
	for (;;) {
		usleep(2000);
		uint64_t lp = last_present_us, t = now_us();
		if (!lp || t - lp < (uint64_t)threshold_ms * 1000u) continue;
		uint64_t start = lp;
		/* a report at the stall's end -- and every 3 s while it lasts, so a hang (no present ever again) is reported too */
		while (last_present_us == start) {
			nsamples = 0;
			uint64_t from = now_us();
			while (last_present_us == start && nsamples < MAXS - 64 && now_us() - from < 3000000u) { sample_all(self); usleep(2000); }
			uint64_t end = last_present_us != start ? last_present_us : now_us();
			report(end - start);
			if (hook_stall_long) hook_stall_long(end - start);
		}
	}
	return 0;
}
void stallprof_present(void) {
	static int init = 0;
	if (!init) {
		init = 1; const char *e = getenv("RECOMP_STALL_PROF"); threshold_ms = e && *e ? atoi(e) : 0;
		if (threshold_ms > 0) { pthread_create(&monitor, 0, monitor_main, 0); pthread_detach(monitor); fprintf(stderr, "    stall profiler: stacks of every thread through any stall over %d ms\n", threshold_ms); }
	}
	if (threshold_ms > 0) last_present_us = now_us();
}
#else
void stallprof_present(void) {}
#endif
