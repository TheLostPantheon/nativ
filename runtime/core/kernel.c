/*
 * The Vita kernel's threads and synchronisation on host threads. A guest thread is a pthread
 * running the recompiled entry over its own Cpu (the recompiled code is re-entrant: all of its
 * state is the Cpu and guest memory). Semaphores, mutexes, lightweight mutexes and conditions,
 * event flags and simple events are one host object type (a pthread mutex guarding the object's
 * state and its wait queue), found by uid, or for the lightweight and SceUlt ones by the guest
 * work area's address.
 *
 * Semantics follow the firmware's thread manager (vitasdk's psp2/kernel headers for the codes and
 * structures; Vita3K's kernel read for the wake rules; libkernel's own user-mode halves of the
 * LwMutex calls disassembled for the error codes): every waiter is a queue entry of its own with
 * what it waits for, woken exactly when the kernel would wake it -- a SignalLwCond wakes one
 * waiter, SignalLwCondAll every one, an event flag's SetEventFlag walks its waiters in queue
 * order and each takes its own clear mode against the pattern the earlier ones left, a semaphore's
 * SignalSema grants the queue head first. The previous version shared one "wanted bits" word per
 * event flag between all its waiters and broadcast every condition signal, so a job system with
 * several workers on one flag or condition saw other workers' wakeups and lost its own.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* glibc: pthread_setname_np (thread_main names host threads after guest threads) */
#endif
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "hle.h"
#include "platform.h"

/* SceKernelThreadMgr's error codes (vitasdk psp2/kernel/error.h); the games compare against
 * SCE_KERNEL_ERROR_WAIT_TIMEOUT (all three do) and libkernel's own code against the mutex ones */
#define E_ILLEGAL_ADDR          0x80020006u
#define E_INVALID_ARGUMENT_SIZE 0x80020009u
#define E_ILLEGAL_PATTERN       0x8002000Du
#define E_ILLEGAL_COUNT         0x8002000Fu
#define E_ILLEGAL_MODE          0x80020010u
#define E_UID_CANNOT_FIND_BY_NAME 0x8002450Au
#define E_UNKNOWN_UID           0x80028001u
#define E_WAIT_TIMEOUT          0x80028005u
#define E_WAIT_DELETE           0x80028006u
#define E_UNKNOWN_THREAD_ID     0x80028021u
#define E_ILLEGAL_THREAD_ID     0x80028022u
#define E_NOT_DORMANT           0x80028028u
#define E_UNKNOWN_EVF_ID        0x800280E1u
#define E_EVF_MULTI             0x800280E2u   /* a second waiter on a flag created without SCE_EVENT_WAITMULTIPLE */
#define E_EVF_COND              0x800280E3u   /* PollEventFlag: the pattern isn't there */
#define E_UNKNOWN_SEMA_ID       0x80028101u
#define E_SEMA_ZERO             0x80028102u   /* PollSema: not enough */
#define E_SEMA_OVF              0x80028103u   /* SignalSema past the maximum */
#define E_UNKNOWN_MUTEX_ID      0x80028141u
#define E_MUTEX_RECURSIVE       0x80028142u   /* the owner locks a non-recursive mutex again */
#define E_MUTEX_LOCK_OVF        0x80028143u
#define E_MUTEX_UNLOCK_UDF      0x80028144u   /* unlocking more than is held */
#define E_MUTEX_FAILED_TO_OWN   0x80028145u   /* TryLock: someone else holds it */
#define E_MUTEX_NOT_OWNED       0x80028146u   /* unlock (or a condition wait) by a thread that doesn't hold it */
#define E_UNKNOWN_LW_MUTEX_ID   0x80028181u   /* the LwMutex codes are the mutex ones + 0x40 */
#define E_LW_MUTEX_NOT_OWNED    0x80028186u
#define E_UNKNOWN_COND_ID       0x800281A1u
#define E_WAIT_DELETE_MUTEX     0x800281A2u
#define E_WAIT_DELETE_COND      0x800281A4u
#define E_UNKNOWN_LW_COND_ID    0x800281C1u
#define E_WAIT_DELETE_LW_MUTEX  0x800281C2u
#define E_WAIT_DELETE_LW_COND   0x800281C3u
#define E_UNKNOWN_EVENT_ID      0x80028201u
#define E_EVENT_COND            0x80028202u   /* PollEvent: the pattern isn't there */
#define E_ILLEGAL_SIZE          0x8002000Bu
#define E_ILLEGAL_ATTR          0x8002000Eu
#define E_WAIT_CANCEL           0x80028007u   /* CancelMsgPipe woke the waiter */
#define E_UNKNOWN_CALLBACK_ID   0x800280A1u
#define E_UNKNOWN_RW_LOCK_ID    0x800281E1u
#define E_RW_LOCK_RECURSIVE     0x800281E2u   /* the writer locks a non-recursive lock again (or a reader asks to write) */
#define E_RW_LOCK_NOT_OWNED     0x800281E4u   /* an unlock by a thread holding nothing that way */
#define E_RW_LOCK_FAILED_TO_LOCK 0x800281E6u  /* TryLock: held the other way */
#define E_UNKNOWN_MSG_PIPE_ID   0x80028221u
#define E_MSG_PIPE_FULL         0x80028222u   /* TrySend: no room for it */
#define E_MSG_PIPE_EMPTY        0x80028223u   /* TryReceive: not there */
/* SceFiber's (psp2/fiber.h) */
#define E_FIBER_NULL            0x80590001u
#define E_FIBER_ALIGNMENT       0x80590002u
#define E_FIBER_RANGE           0x80590003u   /* a context under 512 bytes */
#define E_FIBER_INVALID         0x80590004u   /* context/size disagree, or not a fiber */
#define E_FIBER_PERMISSION      0x80590005u   /* a fiber's call from a thread, or a thread's from a fiber */
#define E_FIBER_STATE           0x80590006u   /* run or switched to while it runs */
/* wait_on's private return: the waiter left the queue to run its callbacks; the operation re-checks and waits again */
#define E_CB_RETRY              0x7fffffffu
/* SceUlt's (libult.suprx: 1 NULL, 2 ALIGNMENT, 3 RANGE, 6 STATE, 8 the try-call failure, read
 * off sceUltSemaphoreTryAcquire's returns; PERMISSION is the SDK's name for 5) */
#define E_ULT_RANGE             0x80810003u
#define E_ULT_PERMISSION        0x80810005u
#define E_ULT_BUSY              0x80810008u

/* ---- threads */
typedef struct Thread {
	uint32_t uid, entry, stack, stack_size, attr, affinity;
	int init_prio, prio;         /* 64 (highest) .. 191 for user threads; the default 160 */
	char name[32];
	Cpu *cpu;
	pthread_t th;
	int started, done, deleted;
	uint32_t status;
	uint32_t wait_type, wait_id; /* what it is blocked on (GetThreadInfo's status WAITING) */
	uint32_t tls, tls_keys;      /* its TLS block (TPIDRURO) and sceKernelGetTLSAddr's keys: a fiber running under it shares both */
	struct Fiber *fiber;         /* the fiber running in its place (fibers, below); 0 while the thread itself runs */
	pthread_cond_t *fcv; int fgo, fexit; uint32_t fret_arg;   /* the handoff back from its fiber: fgo set and fret_arg its argOnReturn; fexit: the fiber ended the thread */
	int cb_pending;              /* a callback it owns was notified (callbacks, below) */
	pthread_mutex_t *wait_m; pthread_cond_t *wait_cv;   /* where it sleeps in a CB wait, for a notifier to wake it */
	/* the run-slot scheduler (RECOMP_SCHED=vita, below). It belongs to the guest thread, not to a
	 * host thread: a fiber's host thread runs as its thread, on the thread's slot */
	int s_state, s_depth, s_slot;  /* S_NONE, S_RUN (holds slot s_slot), S_READY (in the wait list), S_BLOCKED (gave its slot up to sleep; s_depth: nested brackets), S_EVICTED (lost its slot to the timer while running on) */
	uint64_t s_since;            /* S_RUN: when it got its slot (the quantum); S_READY: when it began to wait for one */
	uint64_t s_boundaries, s_seen, s_quiet;   /* boundaries passed; the count the timer last saw, and since when it has stood still */
	pthread_cond_t s_cv;         /* where it waits for a slot (initialised when it is admitted) */
	struct Thread *s_next;       /* the wait list */
} Thread;
#define MAX_THREADS 1024
static Thread threads[MAX_THREADS];
static int nthreads;
static Thread main_thread = { .uid = 0x40010003, .name = "main", .init_prio = 160, .prio = 160, .started = 1, .stack_size = 1u << 20 };
static pthread_mutex_t klock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kcond = PTHREAD_COND_INITIALIZER;   /* thread ends */
static __thread uint32_t cur_tid = 0x40010003;              /* the main thread's */
static __thread Thread *cur_thread;                         /* 0 on the main thread */
#define SELF() (cur_thread ? cur_thread : &main_thread)
uint32_t recomp_cur_tid(void) { return cur_tid; }           /* the calling host thread's guest uid (verify.c's RECOMP_WATCH_*) */
const char *recomp_cur_tname(void) { return SELF()->name; }

static Thread *thread_of(uint32_t uid) {   /* 0: the calling thread, as the kernel takes it */
	if (!uid || uid == cur_tid) return SELF();
	if (uid == main_thread.uid) return &main_thread;
	for (int i = 0; i < nthreads; i++) if (threads[i].uid == uid) return threads[i].deleted ? 0 : &threads[i];
	return 0;
}
static int cur_prio(void) { return SELF()->prio; }
static uint64_t mono_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

/* ---- the scheduler (RECOMP_SCHED=vita; off by default). The Vita gives a game three cores and
 * schedules its threads strictly by priority: a lower-priority thread runs only while every
 * higher-priority runnable thread is blocked, and equal priorities share by round robin. Without
 * this the host runs every guest thread truly concurrently on all of its cores and applies no
 * priority at all, so a job chain whose order the game takes from its priorities can run in an
 * order the console never produces -- Unit 13's scripted enemy entrance (KynapseThread 160,
 * GOM_Worker and the Havok workers 144, the render and audio workers 160, FIOS 66, the display
 * queue 64) never happens on the host, after its door and ragdoll freezes turned out to be
 * sync-semantics bugs.
 *
 * The model: RECOMP_SCHED_CORES (default 3) run slots, slot i being core i (a thread's CPU
 * affinity mask, SCE_KERNEL_CPU_MASK_USER_0..2 = 0x10000 << i, says which it may use; 0 or
 * USER_ALL: any -- FIOS pins its I/O threads to core 2, libgxm its display queue to core 0). A
 * guest thread holds a slot while it runs guest code or a system call that doesn't sleep; it
 * gives the slot up before it sleeps (sched_block_begin: every wait in this file, the display's
 * vblank, input and audio waits) and waits for one again when it wakes (sched_block_end); it also
 * gives it up while the host does device work in its call (main.c's GXM observer rendering a
 * scene, display.c's present, ngs.c's mixing: the GPU's, display's and DSP's work on the
 * hardware -- billed to a CPU slot, the software renderer held one 100+ ms per scene, the timer
 * evicted the render worker mid-scene, and two of nine gate runs of Unit 13's menu came out a
 * black frame). Threads
 * waiting for a slot are in the wait list, by priority (64 highest .. 191 lowest), FIFO within
 * one; a released slot goes straight to the first waiter allowed on it (a handoff: nobody barges
 * in between). Preemption happens where the host can see a thread, at the system-call boundary
 * (main.c calls sched_yield_if_preempted before and after every import call): a strictly
 * higher-priority waiter takes the slot of the lowest-priority holder it may run on (the hardware
 * preempts the lowest-priority core), an equal-priority waiter takes it once the holder has had
 * its quantum (5 ms, RECOMP_SCHED_QUANTUM_US).
 *
 * The hardware also preempts a thread that never makes a system call (its timer interrupt); the
 * host can't stop a host thread in the middle of recompiled code. The monitor thread stands in
 * for the timer: a holder that hasn't passed a boundary for a whole quantum while a waiter is
 * entitled to its slot is evicted -- the slot goes to the waiter, and the evicted thread's host
 * thread, still running, joins the wait list at its next boundary. Without this Unit 13 stopped
 * for good on entering its level: the main thread, preempted inside a Havok job while holding
 * Havok's spinlock (a pure ldrex loop, no system call), waited for a slot that the Havok workers
 * spinning on that lock never gave back. (That preemption itself came from the main thread's
 * priority: the host's default 160, where the game's process parameters ask for 144
 * (sceUserMainThreadPriority 0x100000f0) -- the main thread takes its priority and affinity from
 * there under the scheduler.)
 *
 * Locking: sched_m is a leaf (nothing is taken under it), so giving a slot up (which never waits)
 * is safe under any host lock: wait_on does it with the object's mutex held, just before it sleeps.
 * Waiting for a slot is not: a thread waiting for a slot while holding an object's mutex would stop
 * every slot holder that touches the object (they would block on the mutex, keeping their slots),
 * so sched_block_end is only ever called with no host lock held -- wait_on drops the object's mutex
 * around it (its waiter stays queued meanwhile, and everything it re-checks afterwards is read
 * under the mutex again, so a wake in that gap isn't lost). A thread without a slot still runs host
 * code (the bookkeeping after a wake); only guest code needs a slot. RECOMP_SCHED_LOG=1 prints every
 * handoff and a once-a-second report (the holders, the longest waiter); =2 only the report. */
enum { S_NONE, S_RUN, S_READY, S_BLOCKED, S_EVICTED };
#define SCHED_MAX_CORES 16
int recomp_sched_on;                  /* main.c's dispatch tests it before calling in: nothing while off */
static int sched_cores = 3, sched_log;
static uint64_t sched_quantum = 5000; /* us */
/* round robin among equal priorities at system-call boundaries. The firmware's thread manager
 * (threadmgr.skprx) keeps no time-slice counter in its thread control block and exports nothing
 * like the PSP's RotateThreadReadyQueue: an equal-priority thread waits until the running one
 * blocks. RECOMP_SCHED_RR=0 gives that (run to block); the default keeps the 5 ms round robin
 * until an on-device test settles it. The timer eviction of a thread that makes no system call
 * at all stays either way (Unit 13's Havok spinlock needs it). */
static int sched_rr = 1;
static pthread_mutex_t sched_m = PTHREAD_MUTEX_INITIALIZER;
static Thread *sched_slot[SCHED_MAX_CORES];
static Thread *sched_head;            /* the wait list */
static int sched_best = INT_MAX;      /* the head's priority, read without the lock (the boundary's fast path) */
enum { W_BLOCK, W_PREEMPT, W_QUANTUM, W_EXIT, W_TIMER, W_AFFINITY };
static uint64_t sched_count[6];       /* handoffs by reason since the last report */
static const char *const sched_why[6] = { "block", "preempt", "quantum", "exit", "timer", "affinity" };
static int sched_state(Thread *t) { return __atomic_load_n(&t->s_state, __ATOMIC_RELAXED); }   /* the monitor may evict a holder under sched_m */
static int sched_allowed(Thread *t, int slot) {   /* affinity: core i is SCE_KERNEL_CPU_MASK_USER_i; slots past the console's three take anyone */
	uint32_t m = (t->affinity >> 16) & 7;
	return !m || slot >= 3 || (m >> slot) & 1;
}
static void sq_insert(Thread *t, int front) {   /* sched_m held: by priority, FIFO within one (front: ahead of its equals) */
	Thread **pp = &sched_head;
	while (*pp && ((*pp)->prio < t->prio || (!front && (*pp)->prio == t->prio))) pp = &(*pp)->s_next;
	t->s_next = *pp; *pp = t;
	__atomic_store_n(&t->s_state, S_READY, __ATOMIC_RELAXED); t->s_since = mono_us();
	__atomic_store_n(&sched_best, sched_head->prio, __ATOMIC_RELAXED);
}
static void sq_remove(Thread *t) {
	for (Thread **pp = &sched_head; *pp; pp = &(*pp)->s_next) if (*pp == t) { *pp = t->s_next; break; }
	t->s_next = 0;
	__atomic_store_n(&sched_best, sched_head ? sched_head->prio : INT_MAX, __ATOMIC_RELAXED);
}
static void sched_take(Thread *t, int slot) {   /* sched_m held */
	sched_slot[slot] = t; t->s_slot = slot; t->s_since = t->s_quiet = mono_us(); t->s_seen = t->s_boundaries;
	__atomic_store_n(&t->s_state, S_RUN, __ATOMIC_RELAXED);
}
static int sched_free_slot(Thread *t) { for (int i = 0; i < sched_cores; i++) if (!sched_slot[i] && sched_allowed(t, i)) return i; return -1; }
static Thread *sched_waiter_for(int slot) { for (Thread *w = sched_head; w; w = w->s_next) if (sched_allowed(w, slot)) return w; return 0; }
/* slot `slot` is free: to the first waiter allowed on it, woken here, or it stays free (sched_m
 * held). Invariant: a slot is free only while no waiter may run on it. */
static void sched_hand(int slot, Thread *from, int why) {
	Thread *n = sched_waiter_for(slot);
	if (!n) return;
	if (sched_log == 1) fprintf(stderr, "    sched: core %d %s(%d) -> %s(%d) [%s; waited %llu us]\n", slot, from->name, from->prio, n->name, n->prio,
	                            sched_why[why], (unsigned long long)(mono_us() - n->s_since));
	sched_count[why]++;
	sq_remove(n); sched_take(n, slot);
	pthread_cond_signal(&n->s_cv);
}
static void sched_release(Thread *t, int why) { int slot = t->s_slot; sched_slot[slot] = 0; sched_hand(slot, t, why); }   /* sched_m held, t in S_RUN */
static void sched_wait_turn(Thread *t) { while (t->s_state != S_RUN) pthread_cond_wait(&t->s_cv, &sched_m); }   /* sched_m held */
static void sched_get_slot(Thread *t, int front) {   /* sched_m held: a free slot it may use (nobody waiting can use it), else the list */
	int slot = sched_free_slot(t);
	if (slot >= 0) sched_take(t, slot);
	else { sq_insert(t, front); sched_wait_turn(t); }
}
/* t enters the scheduler: a thread being started, by its starter (in the starter's call, so a
 * higher-priority thread preempts its starter at that call's boundary, as StartThread does on the
 * hardware); the main thread at start-up */
static void sched_admit(Thread *t) {
	if (!recomp_sched_on) return;
	pthread_mutex_lock(&sched_m);
	pthread_cond_init(&t->s_cv, 0);
	t->s_depth = 0; t->s_next = 0;
	int slot = sched_free_slot(t);
	if (slot >= 0) sched_take(t, slot); else sq_insert(t, 0);
	pthread_mutex_unlock(&sched_m);
}
static void sched_thread_start(Thread *t) {   /* on t's own host thread, before its guest code: until it holds a slot */
	if (!recomp_sched_on) return;
	pthread_mutex_lock(&sched_m); sched_wait_turn(t); pthread_mutex_unlock(&sched_m);
}
static void sched_thread_exit(Thread *t) {    /* t ends: its slot goes on */
	if (!recomp_sched_on) return;
	pthread_mutex_lock(&sched_m);
	if (t->s_state == S_RUN) sched_release(t, W_EXIT);
	if (t->s_state == S_RUN || t->s_state == S_EVICTED) __atomic_store_n(&t->s_state, S_NONE, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&sched_m);
}
/* before a sleep: give the slot up (safe under any host lock) -> 1 if sched_block_end must follow */
int sched_block_begin(void) {
	if (!recomp_sched_on) return 0;
	Thread *t = SELF();
	int s = sched_state(t);
	if (s == S_BLOCKED) { t->s_depth++; return 1; }
	if (s != S_RUN && s != S_EVICTED) return 0;   /* not a scheduled thread (a host thread; a unit test's helper) */
	pthread_mutex_lock(&sched_m);
	if (t->s_state == S_RUN) sched_release(t, W_BLOCK);   /* (evicted meanwhile: its slot is someone else's already) */
	__atomic_store_n(&t->s_state, S_BLOCKED, __ATOMIC_RELAXED); t->s_depth = 1;
	pthread_mutex_unlock(&sched_m);
	return 1;
}
/* after the sleep: a slot again, behind the waiting threads of its own priority (a woken thread
 * goes to the end of its ready queue). Never with a host lock held (above). */
void sched_block_end(void) {
	Thread *t = SELF();
	if (t->s_state != S_BLOCKED || --t->s_depth) return;
	pthread_mutex_lock(&sched_m); sched_get_slot(t, 0); pthread_mutex_unlock(&sched_m);
}
/* the system-call boundary (main.c, before and after every import call; ChangeThreadPriority and
 * ChangeThreadCpuAffinityMask): the slot goes to a waiter that should have it (the model above) */
void sched_yield_if_preempted(void) {
	if (!recomp_sched_on) return;
	Thread *t = SELF();
	int s = sched_state(t);
	if (s == S_RUN) {
		t->s_boundaries++;   /* the monitor's sign of life: a holder whose count stands still for a quantum is running without system calls */
		int best = __atomic_load_n(&sched_best, __ATOMIC_RELAXED);
		if (best > t->prio && sched_allowed(t, t->s_slot)) return;   /* the common case: no waiter outranks or ties this thread */
	} else if (s != S_EVICTED) return;
	uint64_t now = mono_us();
	pthread_mutex_lock(&sched_m);
	int why = -1;
	if (t->s_state == S_EVICTED) why = W_TIMER;   /* the monitor took its slot while it ran on: now it waits like a preempted thread */
	else if (t->s_state == S_RUN && !sched_allowed(t, t->s_slot)) why = W_AFFINITY;
	else if (t->s_state == S_RUN) {
		Thread *w = sched_waiter_for(t->s_slot);
		if (w && w->prio < t->prio) {
			int lowest = t->prio;   /* the lowest priority among the holders of the cores w may use */
			for (int i = 0; i < sched_cores; i++) if (sched_slot[i] && sched_allowed(w, i) && sched_slot[i]->prio > lowest) lowest = sched_slot[i]->prio;
			if (lowest == t->prio || now - w->s_since >= sched_quantum) why = W_PREEMPT;
		} else if (w && w->prio == t->prio && sched_rr && now - t->s_since >= sched_quantum) why = W_QUANTUM;
	}
	if (why >= 0) {
		if (why != W_TIMER) sched_release(t, why);
		sched_get_slot(t, why != W_QUANTUM);   /* preempted: first among its equals again; its quantum used up: last */
	}
	pthread_mutex_unlock(&sched_m);
}
/* a priority change (ChangeThreadPriority): a waiting thread moves to the end of its new
 * priority's place in the list; then the caller passes a boundary (it may be outranked now) */
static void sched_set_prio(Thread *t, int prio) {
	if (!recomp_sched_on) { t->prio = prio; return; }
	pthread_mutex_lock(&sched_m);
	t->prio = prio;
	if (t->s_state == S_READY) { uint64_t since = t->s_since; sq_remove(t); sq_insert(t, 0); t->s_since = since; }
	pthread_mutex_unlock(&sched_m);
	sched_yield_if_preempted();
}
/* an affinity change (ChangeThreadCpuAffinityMask): the caller moves at once if its core is no
 * longer allowed (its boundary); another running thread moves at its next one */
static void sched_set_affinity(Thread *t, uint32_t mask) {
	if (!recomp_sched_on) { t->affinity = mask; return; }
	pthread_mutex_lock(&sched_m);
	t->affinity = mask;
	if (t->s_state == S_READY) { uint64_t since = t->s_since; sq_remove(t); sq_insert(t, 0); t->s_since = since; }
	for (int i = 0; i < sched_cores; i++) if (!sched_slot[i] && sched_waiter_for(i)) sched_hand(i, t, W_AFFINITY);   /* a free core it may now use */
	pthread_mutex_unlock(&sched_m);
	sched_yield_if_preempted();
}
/* the timer (sched_m held): a holder that passed no boundary for a quantum loses its slot to a
 * waiter that would have preempted it, the lowest-priority such holder first */
static void sched_tick(uint64_t now) {
	for (int i = 0; i < sched_cores; i++) {   /* s_quiet: since when a holder has passed no boundary */
		Thread *h = sched_slot[i];
		if (h && h->s_boundaries != h->s_seen) { h->s_seen = h->s_boundaries; h->s_quiet = now; }
	}
	for (;;) {
		Thread *victim = 0, *to = 0;
		for (Thread *w = sched_head; w && !victim; w = w->s_next)
			for (int i = 0; i < sched_cores; i++) {
				Thread *h = sched_slot[i];
				if (!h || !sched_allowed(w, i) || now - h->s_quiet < sched_quantum) continue;
				if (w->prio > h->prio || (victim && h->prio <= victim->prio)) continue;   /* w outranks or ties it (a tie: its quantum is over) */
				victim = h; to = w;
			}
		if (!victim) break;
		int slot = victim->s_slot;
		sched_slot[slot] = 0;
		__atomic_store_n(&victim->s_state, S_EVICTED, __ATOMIC_RELAXED);
		if (sched_log == 1) fprintf(stderr, "    sched: core %d %s(%d) evicted for %s(%d) [timer: no system call for %llu us]\n", slot, victim->name, victim->prio,
		                            to->name, to->prio, (unsigned long long)(now - victim->s_quiet));
		sched_count[W_TIMER]++;
		sq_remove(to); sched_take(to, slot); pthread_cond_signal(&to->s_cv);
	}
}
static void sched_report(uint64_t now) {   /* RECOMP_SCHED_LOG: who holds the slots, who has waited longest (sched_m held) */
	char line[1024]; int n = snprintf(line, sizeof line, "    sched: slots");
	for (int i = 0; i < sched_cores && n < (int)sizeof line - 64; i++) {
		Thread *t = sched_slot[i];
		n += t ? snprintf(line + n, sizeof line - n, " [%s(%d) %llu ms]", t->name, t->prio, (unsigned long long)(now - t->s_since) / 1000)
		       : snprintf(line + n, sizeof line - n, " [idle]");
	}
	int nready = 0; Thread *longest = 0;
	for (Thread *t = sched_head; t; t = t->s_next) { nready++; if (!longest || t->s_since < longest->s_since) longest = t; }
	if (longest && n < (int)sizeof line - 96)
		n += snprintf(line + n, sizeof line - n, "; %d ready, longest %s(%d) %llu ms", nready, longest->name, longest->prio, (unsigned long long)(now - longest->s_since) / 1000);
	if (n < (int)sizeof line - 128)
		snprintf(line + n, sizeof line - n, "; handoffs %llu block, %llu preempt, %llu quantum, %llu exit, %llu timer, %llu affinity",
		         (unsigned long long)sched_count[0], (unsigned long long)sched_count[1], (unsigned long long)sched_count[2],
		         (unsigned long long)sched_count[3], (unsigned long long)sched_count[4], (unsigned long long)sched_count[5]);
	memset(sched_count, 0, sizeof sched_count);
	fprintf(stderr, "%s\n", line);
}
static void *sched_monitor(void *p) {   /* a host thread (never scheduled): the timer every quantum, the report every second */
	(void)p;
	uint64_t next_report = mono_us() + 1000000;
	for (;;) {
		uint64_t tick = sched_quantum < 2000 ? sched_quantum / 2 : 1000;   /* a fraction of the quantum: an eviction comes within 1-2 ms of it running out */
		struct timespec ts = { 0, (long)tick * 1000 };
		nanosleep(&ts, 0);
		pthread_mutex_lock(&sched_m);
		uint64_t now = mono_us();
		sched_tick(now);
		if (sched_log && now >= next_report) { sched_report(now); next_report = now + 1000000; }
		pthread_mutex_unlock(&sched_m);
	}
	return 0;
}
static int priority_of(uint32_t p);
extern uint32_t process_param;
/* start-up (main.c, on the main thread before any guest code): RECOMP_SCHED=vita turns the
 * scheduler on, gives the main thread the priority and affinity the program's process parameters
 * ask for (SceProcessParam +0x14 sceUserMainThreadPriority, +0x28 sceUserMainThreadCpuAffinityMask:
 * pointers to the values, 0 for the defaults) and admits it */
void kernel_sched_init(void) {
	const char *e = getenv("RECOMP_SCHED");
	/* the main thread's priority and affinity are the process parameters' (sceUserMainThreadPriority
	 * / ...CpuAffinityMask), scheduler or not: Unit 13 asks for 144 and the host ran it at 160, so
	 * its Havok workers (144) could preempt it -- impossible on hardware -- and GetThreadInfo on
	 * the main thread answered 160 */
	Thread *m = SELF();
	if (process_param && RD32(process_param + 4) == 0x32505350u) {   /* "PSP2" */
		uint32_t pp = RD32(process_param + 0x14), ap = RD32(process_param) >= 0x2c ? RD32(process_param + 0x28) : 0;
		if (pp) m->prio = m->init_prio = priority_of(RD32(pp));
		if (ap) m->affinity = RD32(ap);
	}
	if (!e || strcmp(e, "vita")) return;
	if (getenv("RECOMP_SCHED_CORES")) sched_cores = atoi(getenv("RECOMP_SCHED_CORES"));
	if (sched_cores < 1) sched_cores = 1;
	if (sched_cores > SCHED_MAX_CORES) sched_cores = SCHED_MAX_CORES;
	if (getenv("RECOMP_SCHED_QUANTUM_US")) sched_quantum = strtoull(getenv("RECOMP_SCHED_QUANTUM_US"), 0, 0);
	if (sched_quantum < 100) sched_quantum = 100;
	sched_log = getenv("RECOMP_SCHED_LOG") ? atoi(getenv("RECOMP_SCHED_LOG")) : 0;
	if (getenv("RECOMP_SCHED_RR")) sched_rr = atoi(getenv("RECOMP_SCHED_RR")) != 0;
	recomp_sched_on = 1;
	sched_admit(m);
	fprintf(stderr, "scheduler: %d cores, strict priority, %llu us quantum%s; main thread priority %d affinity %#x\n", sched_cores,
	        (unsigned long long)sched_quantum, sched_rr ? "" : ", no round robin among equal priorities", m->prio, m->affinity);
	pthread_t th; pthread_create(&th, 0, sched_monitor, 0); pthread_detach(th);
}
static void thread_waits(uint32_t type, uint32_t id) { Thread *t = SELF(); t->wait_type = type; t->wait_id = id; }
/* a priority as sceKernelCreateThread/ChangeThreadPriority take it: 64..191, or relative to the
 * default (SCE_KERNEL_DEFAULT_PRIORITY 0x10000100 +- n, the default itself being 160) */
static int priority_of(uint32_t p) {
	int v = (int)p;
	if (v >= 0x10000000) v = v - 0x10000100 + 160;
	if (v < 1) v = 1; if (v > 255) v = 255;   /* out of range is an error on the firmware; keep the thread runnable here (no scheduler applies it) */
	return v;
}
static void h_get_thread_id(Cpu *c) { RET(cur_tid); }
#define PROCESS_ID 0x40000005u   /* the one process: its uid, as GetThreadInfo reports it too (libfios2 asks for it) */
static void h_get_process_id(Cpu *c) { RET(PROCESS_ID); }

/* thread event handlers (sceKernelRegisterThreadEventHandler): called on the starting or exiting
 * thread itself, as (type, threadId, arg, common); type 4 start, 8 exit */
typedef struct { uint32_t uid, thid, mask, handler, common; int live; } ThreadEvent;
static ThreadEvent tevents[16];
static int ntevents;
static void thread_event(Cpu *base, Thread *t, uint32_t type) {
	for (int i = 0; i < ntevents; i++) {
		ThreadEvent *e = &tevents[i];
		if (!e->live || !(e->mask & type)) continue;
		if (e->thid && e->thid != t->uid && thread_of(e->thid)) continue;   /* a specific other thread; anything else means all threads */
		extern GuestFn guest_lookup(uint32_t a);
		GuestFn f = guest_lookup(e->handler & ~1u);
		if (!f) continue;
		Cpu n = *base;
		n.r[0] = type; n.r[1] = t->uid; n.r[2] = 0; n.r[3] = e->common;
		n.r[13] = (base->r[13] - 512) & ~7u; n.r[14] = 0xfffffffe;
		f(&n);
	}
}
static void h_register_thread_event(Cpu *c) {   /* (name, thid, eventMask, handler, common) */
	pthread_mutex_lock(&klock);
	if (ntevents >= 16) { pthread_mutex_unlock(&klock); RET(0x80028060u); return; }   /* THREAD_EVENT_ERROR */
	ThreadEvent *e = &tevents[ntevents++];
	e->uid = 0x40030001 + 2 * (ntevents - 1); e->thid = A1; e->mask = A2; e->handler = A3; e->common = STK(0); e->live = 1;
	pthread_mutex_unlock(&klock);
	RET(e->uid);
}
static void h_unregister_thread_event(Cpu *c) {
	for (int i = 0; i < ntevents; i++) if (tevents[i].uid == A0 && tevents[i].live) { tevents[i].live = 0; RET(0); return; }
	RET(0x80028061u);   /* UNKNOWN_THREAD_EVENT_ID */
}

static void h_create_thread(Cpu *c) {   /* (name, entry, priority, stack size, attr, affinity, opt) */
	pthread_mutex_lock(&klock);
	if (nthreads >= MAX_THREADS) { pthread_mutex_unlock(&klock); fprintf(stderr, "thread table full (%d)\n", MAX_THREADS); RET(0x80028020u); return; }   /* THREAD_ERROR */
	Thread *t = &threads[nthreads];
	memset(t, 0, sizeof *t);
	t->uid = 0x40010100 + 2 * nthreads++;
	t->entry = A1;
	t->init_prio = t->prio = priority_of(A2);
	t->stack_size = A3 < 0x4000 ? 0x4000 : A3;
	t->stack = guest_alloc(t->stack_size, 4096);
	t->attr = STK(0); t->affinity = STK(1);
	snprintf(t->name, sizeof t->name, "%s", A0 ? (char *)G(A0) : "?");
	pthread_mutex_unlock(&klock);
	fprintf(stderr, "    thread '%s' uid %#x entry %#x stack %#x prio %d affinity %#x\n", t->name, t->uid, t->entry, t->stack_size, t->prio, t->affinity);
	RET(t->uid);
}
static void thread_done(Thread *t, uint32_t status) {
	sched_thread_exit(t);
	pthread_mutex_lock(&klock);
	t->status = status; t->done = 1;
	pthread_cond_broadcast(&kcond);
	pthread_mutex_unlock(&klock);
}
static void *thread_main(void *p) {
	Thread *t = p;
	{	/* the guest thread's name on the host thread: profilers, samplers and /proc show it (15 chars on Linux) */
		char nm[16]; snprintf(nm, sizeof nm, "%s", t->name);
#ifdef __APPLE__
		pthread_setname_np(nm);
#else
		pthread_setname_np(pthread_self(), nm);
#endif
	}
	recomp_runfast();   /* FPCR is per host thread */
	cur_tid = t->uid; cur_thread = t;
	extern uint32_t tls_block(void);
	recomp_tpidruro = tls_block();
	extern GuestFn guest_lookup(uint32_t a);
	GuestFn f = guest_lookup(t->entry & ~1u);
	sched_thread_start(t);   /* admitted by its starter (spawn): runs once it has a slot */
	if (f) {
		thread_event(t->cpu, t, 4);
		f(t->cpu);
		thread_event(t->cpu, t, 8);
	} else fprintf(stderr, "thread '%s': entry %#x is not recompiled code\n", t->name, t->entry);
	thread_done(t, t->cpu->r[0]);
	return 0;
}
static void spawn(Thread *t, Cpu *n) {
	t->cpu = n; t->started = 1;
	pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16u << 20);   /* host stack: recompiled frames are big */
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);   /* nobody joins: WaitThreadEnd uses kcond */
	sched_admit(t);
	pthread_create(&t->th, &at, thread_main, t);
}
static void h_start_thread(Cpu *c) {   /* (uid, arg size, arg block): the block is copied to the new stack */
	Thread *t = thread_of(A0);
	if (!t || t == &main_thread) { RET(E_UNKNOWN_THREAD_ID); return; }
	if (t->started) { RET(E_NOT_DORMANT); return; }
	Cpu *n = calloc(1, sizeof *n);
	uint32_t sp = t->stack + t->stack_size - 16, size = A1;
	if (size && A2) { sp = (sp - size) & ~7u; memcpy(G(sp), G(A2), size); }
	n->r[0] = size; n->r[1] = size && A2 ? sp : 0;
	n->r[13] = sp & ~7u; n->r[14] = 0xfffffffe;
	n->fpscr = 0x03000000;
	spawn(t, n);
	RET(0);
}
/* run guest code on a new thread and wait for it (the loader runs a module's module_start that
 * way: some end it with sceKernelExitThread) -> its exit status */
uint32_t kernel_run_thread(const char *name, uint32_t entry) {
	pthread_mutex_lock(&klock);
	Thread *t = &threads[nthreads];
	memset(t, 0, sizeof *t);
	t->uid = 0x40010100 + 2 * nthreads++;
	t->entry = entry; t->stack_size = 0x40000; t->stack = guest_alloc(t->stack_size, 4096);
	t->init_prio = t->prio = 160;
	snprintf(t->name, sizeof t->name, "%s", name);
	pthread_mutex_unlock(&klock);
	Cpu *n = calloc(1, sizeof *n);
	n->r[13] = t->stack + t->stack_size - 16; n->r[14] = 0xfffffffe; n->fpscr = 0x03000000;
	spawn(t, n);
	pthread_mutex_lock(&klock);
	int b = 0;
	while (!t->done) { if (!b) b = sched_block_begin(); pthread_cond_wait(&kcond, &klock); }
	pthread_mutex_unlock(&klock);
	if (b) sched_block_end();
	return t->status;
}
static void fiber_thread_exits(void);   /* below: a fiber ending its thread releases the thread's host thread too */
static void h_exit_thread(Cpu *c) {
	Thread *t = cur_thread;
	fiber_thread_exits();
	if (!t) { fprintf(stderr, "main thread exited (%#x)\n", A0); exit(0); }
	uint32_t status = A0;
	thread_event(c, t, 8);
	thread_done(t, status);
	pthread_exit(0);
}
static void h_exit_delete_thread(Cpu *c) { if (cur_thread) cur_thread->deleted = 1; h_exit_thread(c); }
static void h_delete_thread(Cpu *c) {
	Thread *t = thread_of(A0);
	if (!t || t == &main_thread) { RET(E_UNKNOWN_THREAD_ID); return; }
	if (t->started && !t->done) { RET(E_NOT_DORMANT); return; }   /* only a dormant thread can be deleted */
	t->deleted = 1;
	RET(0);
}
/* the *CB system calls (WaitSemaCB, WaitEventFlagCB, ...): while the thread sleeps, a notification
 * of a callback it owns wakes it to run the callback, then the wait goes on with the time left --
 * here the waiter leaves the queue and the operation starts over (E_CB_RETRY, see cb_call), as the
 * kernel's re-check after a callback does. Inside a callback a CB wait is a plain wait (no nesting). */
static __thread int cb_wait, in_callbacks;
static pthread_mutex_t cblock = PTHREAD_MUTEX_INITIALIZER;   /* a thread's cb_pending and its published sleep */
static Thread *cb_thread(void) { return cb_wait && !in_callbacks ? SELF() : 0; }
static void cb_publish(Thread *t, pthread_mutex_t *m, pthread_cond_t *cv) {   /* o->m held by the caller: cblock is a leaf for the waiter */
	pthread_mutex_lock(&cblock); t->wait_m = m; t->wait_cv = cv; pthread_mutex_unlock(&cblock);
}
static int cb_pending(Thread *t) { return __atomic_load_n(&t->cb_pending, __ATOMIC_ACQUIRE); }
/* a wait's deadline from the guest's *timeout (microseconds; 0 pointer: forever); on return the
 * kernel writes the time left back into *timeout (0 after a timeout), as the PSP's did */
static int deadline(uint32_t timeout_p, struct timespec *ts) {
	if (!timeout_p) return 0;
	uint32_t us = RD32(timeout_p);
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += us / 1000000; ts->tv_nsec += (us % 1000000) * 1000;
	if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
	return 1;
}
static void timeout_left(uint32_t timeout_p, int timed, const struct timespec *ts) {
	if (!timeout_p || !timed) return;
	struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
	int64_t left = (int64_t)(ts->tv_sec - now.tv_sec) * 1000000 + (ts->tv_nsec - now.tv_nsec) / 1000;
	WR32(timeout_p, left > 0 ? (uint32_t)left : 0);
}
static void h_wait_thread_end(Cpu *c) {   /* (uid, *status, *timeout) -> 0, the status through the pointer */
	Thread *t = thread_of(A0);
	if (!t || t == &main_thread) { RET(E_UNKNOWN_THREAD_ID); return; }
	struct timespec ts; int timed = deadline(A2, &ts);
	thread_waits(9, A0);
	Thread *me = cb_thread();   /* WaitThreadEndCB: a notified callback interrupts (kcond is broadcast by the notifier) */
	pthread_mutex_lock(&klock);
	if (me) cb_publish(me, &klock, &kcond);
	int b = 0;   /* the slot is given up only if it actually sleeps, and taken back once klock is released */
	while (!t->done && !(me && cb_pending(me))) {
		if (!b) b = sched_block_begin();
		if (timed ? pthread_cond_timedwait(&kcond, &klock, &ts) == ETIMEDOUT : pthread_cond_wait(&kcond, &klock) != 0) break;
	}
	if (me) cb_publish(me, 0, 0);
	int done = t->done;
	pthread_mutex_unlock(&klock);
	thread_waits(0, 0);
	if (b) sched_block_end();
	if (!done && me && cb_pending(me)) { timeout_left(A2, timed, &ts); RET(E_CB_RETRY); return; }
	if (!done) { if (A2) WR32(A2, 0); RET(E_WAIT_TIMEOUT); return; }
	timeout_left(A2, timed, &ts);
	if (A1) WR32(A1, t->status);
	RET(0);
}
static void h_thread_exit_status(Cpu *c) {
	Thread *t = thread_of(A0);
	if (!t) { RET(E_UNKNOWN_THREAD_ID); return; }
	if (t->started && !t->done) { RET(E_NOT_DORMANT); return; }
	if (A1) WR32(A1, t->status);
	RET(0);
}
static void h_delay(Cpu *c) {
	uint32_t us = A0 ? A0 : 1;
	struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
	thread_waits(2, 0);
	int b = sched_block_begin();
	while (nanosleep(&ts, &ts) && errno == EINTR) ;
	thread_waits(0, 0);
	if (b) sched_block_end();
	guest_time_slept(us);
	RET(0);
}
/* (uid, *SceKernelThreadInfo): the 0x80-byte structure (vitasdk psp2common/kernel/threadmgr.h);
 * the size field must say so. Stubbed, the game read whatever was on its stack. */
static void h_thread_info(Cpu *c) {
	Thread *t = thread_of(A0);
	if (!t) { RET(E_UNKNOWN_THREAD_ID); return; }
	if (!A1) { RET(E_ILLEGAL_ADDR); return; }
	if (RD32(A1) != 0x80) { RET(E_INVALID_ARGUMENT_SIZE); return; }
	uint32_t p = A1;
	memset(G(p + 4), 0, 0x7c);
	WR32(p + 4, PROCESS_ID);
	memcpy(G(p + 8), t->name, 32);
	WR32(p + 0x28, t->attr);
	uint32_t status = t == SELF() ? 1 : (!t->started || t->done) ? 16 : t->wait_type ? 8 : 2;   /* RUNNING, DORMANT, WAITING, READY */
	WR32(p + 0x2c, status);
	{	/* a thread polling another's state in a loop is waiting for it to change (Killzone's job hand-off spins on a worker
		 * being WAITING): past a million polls of one thread, which one, as what, from where -- once per pair */
		static __thread uint32_t last_uid, polls; static __thread int said;
		if (A0 != last_uid) { last_uid = A0; polls = 0; }
		if (++polls == 1000000 && said++ < 4)
			fprintf(stderr, "    kernel: thread '%s' polled thread '%s' (uid %#x) %u times from %#x: status %u, wait type %u, wait id %#x\n",
			        SELF() ? SELF()->name : "?", t->name, A0, polls, c->r[14], status, t->wait_type, t->wait_id);
	}
	WR32(p + 0x30, t->entry);
	WR32(p + 0x34, t->stack); WR32(p + 0x38, t->stack_size);
	WR32(p + 0x3c, t->init_prio); WR32(p + 0x40, t->prio);
	WR32(p + 0x44, t->affinity); WR32(p + 0x48, t->affinity ? t->affinity : 0x70000);   /* SCE_KERNEL_CPU_MASK_USER_ALL */
	WR32(p + 0x4c, 0); WR32(p + 0x50, 0);                      /* current / last CPU */
	WR32(p + 0x54, t->wait_type); WR32(p + 0x58, t->wait_id);
	WR32(p + 0x5c, t->done ? t->status : 0);
	WR64(p + 0x60, guest_time_us());                           /* runClocks: no accounting; a time */
	RET(0);
}
static void h_thread_current_priority(Cpu *c) { RET(cur_prio()); }
static void h_change_priority(Cpu *c) {   /* (uid, priority): the scheduler (RECOMP_SCHED=vita) applies it at once; without it, kept for the queries */
	Thread *t = thread_of(A0);
	if (!t) { RET(E_UNKNOWN_THREAD_ID); return; }
	RET(0);
	sched_set_prio(t, priority_of(A1));   /* last: it may hand the caller's slot over */
}
static void h_thread_affinity(Cpu *c) { Thread *t = thread_of(A0); if (!t) { RET(E_UNKNOWN_THREAD_ID); return; } RET(t->affinity ? t->affinity : 0x70000); }
static void h_change_affinity(Cpu *c) { Thread *t = thread_of(A0); if (!t) { RET(E_UNKNOWN_THREAD_ID); return; } RET(0); sched_set_affinity(t, A1); }   /* the scheduler applies it (RECOMP_SCHED=vita) */
static void h_zero_ok(Cpu *c) { RET(0); }   /* ChangeThreadVfpException, CloseSema/CloseMutex: nothing to apply */
static void h_exit_process(Cpu *c) { fprintf(stderr, "process exited (sceKernelExitProcess %#x)\n", A0); fflush(stderr); exit(0); }
static void h_abort_handler(Cpu *c) {   /* libc's abort() ends here; a stub that returned let the program run on past its own abort */
	fprintf(stderr, "abort (sceKernelCallAbortHandler from lr %#x); last formatted string: %s\n", c->r[14], hle_last_format);
	fflush(stderr); exit(1);
}

/* ---- sync objects */
enum { SEMA = 1, MUTEX, LWMUTEX, LWCOND, EVF, KCOND, SEVENT, RWLOCK, MSGPIPE, CALLBACK };
/* a thread blocked on an object: what it waits for and the condition variable it sleeps on (one
 * per host thread: a thread waits on one object at a time, and a pthread condition may serve
 * different mutexes over time), so a wake is one signal to one thread */
typedef struct Waiter {
	uint32_t tid; int prio;
	int need;                       /* semaphore: the count wanted; mutex: the lock count to take */
	uint32_t bits, mode, out, udata;/* event flag / simple event: the pattern wanted, the wait mode, where the result pattern and user data go */
	int woken; uint32_t result;
	pthread_cond_t *cv;
	struct Waiter *next, *prev;
} Waiter;
typedef struct {
	int kind;
	uint32_t uid, work, attr;       /* lightweight objects are found by their guest work area; attr bit 0x2000: waiters queued by priority */
	pthread_mutex_t m;
	int count, max, init, recursive;/* semaphore: count/maximum/initial; mutex: lock count, recursive */
	uint32_t owner;                 /* a mutex's owner (thread uid) */
	uint32_t lwmutex;               /* a lightweight condition's mutex (work address) */
	uint32_t mutex_uid;             /* a (kernel) condition's mutex */
	uint32_t bits;                  /* an event flag's or simple event's pattern */
	uint64_t udata;                 /* a simple event's user data (the last SetEvent's) */
	int deleted;
	Waiter *head, *tail; int nwait;
	char name[32];
	struct { uint32_t tid; int n; } *readers; int nreaders, readers_cap;   /* a reader-writer lock's readers and their counts (owner/count: the writer) */
	uint8_t *ring; int cap, used, rpos;   /* a message pipe's bytes: a ring of the created size */
	uint32_t func, common, notify_arg, notifier; int ncount;   /* a callback: its function and common argument; the notifications not yet delivered */
} Obj;
static __thread pthread_cond_t *my_cv;
static pthread_cond_t *wait_cv(void) { if (!my_cv) { my_cv = malloc(sizeof *my_cv); pthread_cond_init(my_cv, 0); } return my_cv; }
/* objects: pages of pointers (an object's address never moves, readers need no lock; a deleted
 * object stays allocated, so a stale uid or pointer is an error rather than a crash), and the
 * lightweight ones also indexed by their guest work area */
#define OBJ_PAGE 1024
static Obj **obj_pages[1024];
static int nobjs;
static Obj *obj_at(uint32_t i) { Obj **p = i / OBJ_PAGE < 1024 ? obj_pages[i / OBJ_PAGE] : 0; return p ? p[i % OBJ_PAGE] : 0; }
typedef struct WorkNode { Obj *o; struct WorkNode *next; } WorkNode;
static WorkNode *by_work_hash[1 << 16];
static Obj *new_obj(int kind, uint32_t name, uint32_t attr) {
	Obj *o = calloc(1, sizeof *o);
	pthread_mutex_init(&o->m, 0);
	o->kind = kind; o->attr = attr;
	snprintf(o->name, sizeof o->name, "%s", name ? (char *)G(name) : "");
	pthread_mutex_lock(&klock);
	o->uid = 0x40020001 + 2 * nobjs;
	if (!obj_pages[nobjs / OBJ_PAGE]) obj_pages[nobjs / OBJ_PAGE] = calloc(OBJ_PAGE, sizeof(Obj *));
	obj_pages[nobjs / OBJ_PAGE][nobjs % OBJ_PAGE] = o;
	nobjs++;
	pthread_mutex_unlock(&klock);
	return o;
}
static Obj *by_uid(uint32_t uid, int kind) {
	uint32_t i = (uid - 0x40020001) / 2;
	Obj *o = (uid & 1) && i < (uint32_t)nobjs ? obj_at(i) : 0;
	return o && o->kind == kind && !o->deleted ? o : 0;
}
static Obj *by_work(uint32_t work, int kind) {
	pthread_mutex_lock(&klock);
	Obj *r = 0;
	for (WorkNode *n = by_work_hash[(work >> 2) & 0xffff]; n; n = n->next) if (n->o->work == work && n->o->kind == kind) { r = n->o; break; }
	pthread_mutex_unlock(&klock);
	return r;
}
static Obj *new_obj_at(int kind, uint32_t name, uint32_t attr, uint32_t work) {   /* a lightweight object, found by its work area (newest first: a work area can be reused) */
	Obj *o = new_obj(kind, name, attr);
	o->work = work;
	WorkNode *n = malloc(sizeof *n);
	pthread_mutex_lock(&klock);
	n->o = o; n->next = by_work_hash[(work >> 2) & 0xffff]; by_work_hash[(work >> 2) & 0xffff] = n;
	pthread_mutex_unlock(&klock);
	return o;
}
static void unlink_work(Obj *o) {
	pthread_mutex_lock(&klock);
	for (WorkNode **pp = &by_work_hash[(o->work >> 2) & 0xffff]; *pp; pp = &(*pp)->next) if ((*pp)->o == o) { WorkNode *n = *pp; *pp = n->next; free(n); break; }
	pthread_mutex_unlock(&klock);
}

/* the wait queue (o->m held): FIFO, or by priority when the object was created with
 * SCE_KERNEL_ATTR_THREAD_PRIO (a higher-priority -- lower-numbered -- thread goes before the first lower one) */
static void enqueue(Obj *o, Waiter *w) {
	w->woken = 0; w->next = w->prev = 0;
	Waiter *at = 0;
	if (o->attr & 0x2000) for (Waiter *x = o->head; x; x = x->next) if (x->prio > w->prio) { at = x; break; }
	if (at) { w->next = at; w->prev = at->prev; if (at->prev) at->prev->next = w; else o->head = w; at->prev = w; }
	else { w->prev = o->tail; if (o->tail) o->tail->next = w; else o->head = w; o->tail = w; }
	o->nwait++;
}
static void dequeue(Obj *o, Waiter *w) {
	if (w->prev) w->prev->next = w->next; else o->head = w->next;
	if (w->next) w->next->prev = w->prev; else o->tail = w->prev;
	w->next = w->prev = 0; o->nwait--;
}
static void wake(Obj *o, Waiter *w, uint32_t result) { dequeue(o, w); w->result = result; w->woken = 1; pthread_cond_signal(w->cv); }
/* block as w on o (o->m held) until woken (-> the waker's result), the deadline (WAIT_TIMEOUT,
 * *timeout = 0) or the object's deletion (the kind's WAIT_DELETE code) */
static uint32_t wait_on(Obj *o, Waiter *w, uint32_t timeout_p, uint32_t edelete, uint32_t wait_type) {
	struct timespec ts; int timed = deadline(timeout_p, &ts);
	w->tid = cur_tid; w->prio = cur_prio(); w->cv = wait_cv();
	enqueue(o, w);
	thread_waits(wait_type, o->uid);
	Thread *t = cb_thread();
	if (t) cb_publish(t, &o->m, w->cv);
	int b = 0;
	while (!w->woken && !o->deleted && !(t && cb_pending(t))) {
		if (!b) b = sched_block_begin();   /* the slot goes before the first sleep (o->m held: giving it up never waits) */
		if (!timed) pthread_cond_wait(w->cv, &o->m);
		else if (pthread_cond_timedwait(w->cv, &o->m, &ts) == ETIMEDOUT) break;
	}
	if (t) cb_publish(t, 0, 0);
	thread_waits(0, 0);
	if (b) {   /* a slot again, without o->m: a slot holder may need o->m meanwhile (the wake, the result, a deletion are all re-read under it below) */
		pthread_mutex_unlock(&o->m);
		sched_block_end();
		pthread_mutex_lock(&o->m);
	}
	if (w->woken) { timeout_left(timeout_p, timed, &ts); return w->result; }   /* a wake that raced the timeout counts as a wake */
	dequeue(o, w);
	if (o->deleted) return edelete;
	if (t && cb_pending(t)) { timeout_left(timeout_p, timed, &ts); return E_CB_RETRY; }   /* *timeout now holds what is left for the retry */
	if (timeout_p) WR32(timeout_p, 0);
	return E_WAIT_TIMEOUT;
}
/* delete: every waiter returns WAIT_DELETE; the uid and work area stop resolving */
static void obj_delete(Obj *o) {
	pthread_mutex_lock(&o->m);
	o->deleted = 1;
	for (Waiter *w = o->head; w; w = w->next) pthread_cond_signal(w->cv);
	pthread_mutex_unlock(&o->m);
	if (o->work) unlink_work(o);
}
static void h_delete_uid(Cpu *c, int kind, uint32_t eunknown) { Obj *o = by_uid(A0, kind); if (!o) { RET(eunknown); return; } obj_delete(o); RET(0); }
static void h_delete_work(Cpu *c, int kind, uint32_t eunknown) { Obj *o = by_work(A0, kind); if (!o) { RET(eunknown); return; } obj_delete(o); RET(0); }

/* ---- semaphores: (name, attr, init, max, opt); a wait takes `need` when the count allows, else
 * queues; a signal adds and then grants the queue in order, stopping at the first waiter it can't
 * satisfy (so a waiter for 3 isn't starved by a stream of waiters for 1) */
static void sema_grant(Obj *o) { while (o->head && o->count >= o->head->need) { o->count -= o->head->need; wake(o, o->head, 0); } }
/* a creation the kernel refuses is said once: the game then runs with a bad uid, silently */
static void refused(const char *what, uint32_t lr) { static int said; if (said++ < 8) fprintf(stderr, "    %s refused (ILLEGAL_COUNT) from lr %#x\n", what, lr); }
static void h_create_sema(Cpu *c) {
	int init = (int)A2, max = (int)A3;
	if (init < 0 || max <= 0 || init > max) { refused("CreateSema", c->r[14]); RET(E_ILLEGAL_COUNT); return; }
	Obj *o = new_obj(SEMA, A0, A1); o->count = o->init = init; o->max = max; RET(o->uid);
}
static uint32_t sema_wait(Obj *o, int need, uint32_t timeout_p, int poll, uint32_t epoll, uint32_t edelete) {
	if (need <= 0 || (o->max && need > o->max)) return E_ILLEGAL_COUNT;
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	if (o->count >= need && !o->head) o->count -= need;   /* queued waiters go first (they're there because the count ran short) */
	else if (poll) r = epoll;
	else { Waiter w = { .need = need }; r = wait_on(o, &w, timeout_p, edelete, 3); }
	pthread_mutex_unlock(&o->m);
	return r;
}
static uint32_t sema_signal(Obj *o, int n, uint32_t eovf) {
	if (n <= 0) return E_ILLEGAL_COUNT;
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	if (o->max && o->count + n > o->max) r = eovf;   /* the count is left alone: the signal is refused */
	else { o->count += n; sema_grant(o); }
	pthread_mutex_unlock(&o->m);
	return r;
}
static void h_wait_sema(Cpu *c) { Obj *o = by_uid(A0, SEMA); RET(o ? sema_wait(o, (int)A1, A2, 0, 0, E_WAIT_DELETE) : E_UNKNOWN_SEMA_ID); }   /* (uid, need, *timeout) */
static void h_poll_sema(Cpu *c) { Obj *o = by_uid(A0, SEMA); RET(o ? sema_wait(o, (int)A1, 0, 1, E_SEMA_ZERO, E_WAIT_DELETE) : E_UNKNOWN_SEMA_ID); }
static void h_signal_sema(Cpu *c) { Obj *o = by_uid(A0, SEMA); RET(o ? sema_signal(o, (int)A1, E_SEMA_OVF) : E_UNKNOWN_SEMA_ID); }
static void h_delete_sema(Cpu *c) { h_delete_uid(c, SEMA, E_UNKNOWN_SEMA_ID); }
static void h_sema_info(Cpu *c) {   /* (uid, *SceKernelSemaInfo 0x3c) */
	Obj *o = by_uid(A0, SEMA);
	if (!o) { RET(E_UNKNOWN_SEMA_ID); return; }
	if (!A1 || RD32(A1) != 0x3c) { RET(A1 ? E_INVALID_ARGUMENT_SIZE : E_ILLEGAL_ADDR); return; }
	pthread_mutex_lock(&o->m);
	WR32(A1 + 4, o->uid); memcpy(G(A1 + 8), o->name, 32); WR32(A1 + 0x28, o->attr);
	WR32(A1 + 0x2c, o->init); WR32(A1 + 0x30, o->count); WR32(A1 + 0x34, o->max); WR32(A1 + 0x38, o->nwait);
	pthread_mutex_unlock(&o->m);
	RET(0);
}

/* ---- mutexes (uid) and lightweight mutexes (work area): attr bit 1 = recursive; the LwMutex
 * calls return the mutex codes + 0x40. An unlock that frees the mutex hands it to the queue head
 * directly (owner and count set by the unlocker), so a waiter can't lose it to a thread that
 * barges in between the unlock and its own wakeup. */
static uint32_t mutex_lock(Obj *o, int n, uint32_t timeout_p, int try_only, int lw) {
	uint32_t L = lw ? 0x40 : 0;
	if (n <= 0 || (!o->recursive && n != 1)) return E_ILLEGAL_COUNT;
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	if (o->deleted) r = E_UNKNOWN_MUTEX_ID + L;
	else if (o->count && o->owner == cur_tid) {
		if (!o->recursive) r = E_MUTEX_RECURSIVE + L;
		else if (o->count > INT_MAX - n) r = E_MUTEX_LOCK_OVF + L;
		else o->count += n;
	} else if (!o->count) { o->owner = cur_tid; o->count = n; }
	else if (try_only) r = E_MUTEX_FAILED_TO_OWN + L;
	else { Waiter w = { .need = n }; r = wait_on(o, &w, timeout_p, lw ? E_WAIT_DELETE_LW_MUTEX : E_WAIT_DELETE_MUTEX, lw ? 7 : 6); }
	pthread_mutex_unlock(&o->m);
	return r;
}
static void mutex_release(Obj *o) {   /* o->m held, the owner's last count gone: to the queue head, or free */
	o->count = 0; o->owner = 0;
	if (o->head) { Waiter *w = o->head; o->owner = w->tid; o->count = w->need; wake(o, w, 0); }
}
static uint32_t mutex_unlock(Obj *o, int n, int lw) {
	uint32_t L = lw ? 0x40 : 0;
	if (n <= 0 || (!o->recursive && n != 1)) return E_ILLEGAL_COUNT;
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	if (!o->count || o->owner != cur_tid) r = E_MUTEX_NOT_OWNED + L;
	else if (n > o->count) r = E_MUTEX_UNLOCK_UDF + L;
	else if (!(o->count -= n)) mutex_release(o);
	pthread_mutex_unlock(&o->m);
	return r;
}
static uint32_t mutex_init(Obj *o, uint32_t attr, int init) {
	o->recursive = (attr & 2) != 0;
	if (init < 0 || (!o->recursive && init > 1)) return E_ILLEGAL_COUNT;
	if (init) { o->owner = cur_tid; o->count = init; }
	o->init = init;
	return 0;
}
static void h_create_mutex(Cpu *c) {   /* (name, attr, init count, opt) */
	Obj *o = new_obj(MUTEX, A0, A1);
	uint32_t r = mutex_init(o, A1, (int)A2);
	if (r) { o->deleted = 1; refused("CreateMutex", c->r[14]); RET(r); return; }
	RET(o->uid);
}
static void h_lock_mutex(Cpu *c) { Obj *o = by_uid(A0, MUTEX); RET(o ? mutex_lock(o, (int)A1, A2, 0, 0) : E_UNKNOWN_MUTEX_ID); }
static void h_trylock_mutex(Cpu *c) { Obj *o = by_uid(A0, MUTEX); RET(o ? mutex_lock(o, (int)A1, 0, 1, 0) : E_UNKNOWN_MUTEX_ID); }
static void h_unlock_mutex(Cpu *c) { Obj *o = by_uid(A0, MUTEX); RET(o ? mutex_unlock(o, (int)A1, 0) : E_UNKNOWN_MUTEX_ID); }
static void h_delete_mutex(Cpu *c) { h_delete_uid(c, MUTEX, E_UNKNOWN_MUTEX_ID); }
static void h_mutex_info(Cpu *c) {   /* (uid, *SceKernelMutexInfo 0x3c) */
	Obj *o = by_uid(A0, MUTEX);
	if (!o) { RET(E_UNKNOWN_MUTEX_ID); return; }
	if (!A1 || RD32(A1) != 0x3c) { RET(A1 ? E_INVALID_ARGUMENT_SIZE : E_ILLEGAL_ADDR); return; }
	pthread_mutex_lock(&o->m);
	WR32(A1 + 4, o->uid); memcpy(G(A1 + 8), o->name, 32); WR32(A1 + 0x28, o->attr);
	WR32(A1 + 0x2c, o->init); WR32(A1 + 0x30, o->count); WR32(A1 + 0x34, o->owner); WR32(A1 + 0x38, o->nwait);
	pthread_mutex_unlock(&o->m);
	RET(0);
}
static void h_create_lwmutex(Cpu *c) {   /* (work, name, attr, init count, opt) */
	if (!A0) { RET(E_ILLEGAL_ADDR); return; }
	Obj *o = new_obj_at(LWMUTEX, A1, A2, A0);
	uint32_t r = mutex_init(o, A2, (int)A3);
	if (r) { obj_delete(o); refused("CreateLwMutex", c->r[14]); RET(r); return; }
	RET(0);
}
static Obj *lwmutex(uint32_t work) {   /* one made by hand (work area zeroed, never created) is created on first use */
	Obj *o = by_work(work, LWMUTEX);
	if (!o) { o = new_obj_at(LWMUTEX, 0, 2, work); o->recursive = 1; }
	return o;
}
/* RECOMP_LOG_LWMUTEX=1: every failing lock/unlock of a lightweight mutex, once per (work area, call site, code) --
 * a lock count that only grows (Killzone's ScreamSynth, owner scream_tick, count 118948 at the end of a hung run)
 * means some unlock isn't landing */
static struct { uint32_t work, lr, tid, r, up; int count; char op; } lw_ring[1024]; static unsigned lw_ring_n;   /* the last operations (RECOMP_LOG_LWMUTEX) */
static void lw_log(const char *what, uint32_t work, uint32_t r, Cpu *c) {
	static int on = -1; if (on < 0) on = getenv("RECOMP_LOG_LWMUTEX") != 0;
	if (!on) return;
	Obj *ob = by_work(work, LWMUTEX); int cnt = ob ? ob->count : -1;
	{	unsigned k = __atomic_fetch_add(&lw_ring_n, 1, __ATOMIC_RELAXED) % 1024;
		lw_ring[k].work = work; lw_ring[k].lr = c->r[14]; lw_ring[k].up = c->r[13] + 24 > c->r[13] ? RD32(c->r[13] + 20) : 0; lw_ring[k].tid = cur_tid; lw_ring[k].r = r; lw_ring[k].count = cnt; lw_ring[k].op = what[0];
	}
	static uint32_t deep[16]; static int ndeep;
	if (!r && cnt >= 64) {   /* a recursion this deep is a leak: the operations on this lock that led here */
		int i; for (i = 0; i < ndeep && deep[i] != work; i++);
		if (i == ndeep && ndeep < 16) {
			deep[ndeep++] = work;
			fprintf(stderr, "    lwmutex %#x: lock count %d (owner %#x %s); its last operations, oldest first:\n", work, cnt, cur_tid, SELF()->name);
			unsigned end = lw_ring_n;
			for (unsigned j = end > 1024 ? end - 1024 : 0; j < end; j++) { unsigned k = j % 1024; if (lw_ring[k].work == work) fprintf(stderr, "      %c tid %#x lr %#x (stack +20: %#x) -> %#x count %d\n", lw_ring[k].op, lw_ring[k].tid, lw_ring[k].lr, lw_ring[k].up, lw_ring[k].r, lw_ring[k].count); }
		}
	}
	if (!r || r == E_MUTEX_FAILED_TO_OWN + 0x40) return;
	static struct { uint32_t work, lr, r; } seen[256]; static int n; int i;
	for (i = 0; i < n && !(seen[i].work == work && seen[i].lr == c->r[14] && seen[i].r == r); i++);
	if (i < n || n == 256) return;
	seen[n].work = work; seen[n].lr = c->r[14]; seen[n].r = r; n++;
	Obj *o = by_work(work, LWMUTEX);
	fprintf(stderr, "    lwmutex %#x %s by %#x (%s) from lr %#x: %#x; owner %#x count %d\n", work, what, cur_tid, SELF()->name, c->r[14], r, o ? o->owner : 0, o ? o->count : -1);
}
static void h_lock_lwmutex(Cpu *c) { uint32_t w = A0, r = mutex_lock(lwmutex(w), (int)A1, A2, 0, 1); lw_log("lock", w, r, c); RET(r); }
static void h_trylock_lwmutex(Cpu *c) { uint32_t w = A0, r = mutex_lock(lwmutex(w), (int)A1, 0, 1, 1); lw_log("trylock", w, r, c); RET(r); }
static void h_unlock_lwmutex(Cpu *c) { uint32_t w = A0, r = mutex_unlock(lwmutex(w), (int)A1, 1); lw_log("unlock", w, r, c); RET(r); }
static void h_delete_lwmutex(Cpu *c) { h_delete_work(c, LWMUTEX, E_UNKNOWN_LW_MUTEX_ID); }
/* for hooks.c: a lightweight mutex the calling thread holds let go of wholly, and taken again with the same count -- a game
 * holding its own lock across a call that waits for a thread that wants that lock (Killzone's video wrapper) */
int kernel_lwmutex_release_all(uint32_t work) {   /* the count let go of: 0 if this thread didn't hold it */
	Obj *o = lwmutex(work); if (!o) return 0;
	pthread_mutex_lock(&o->m); int n = o->count && o->owner == cur_tid ? o->count : 0; pthread_mutex_unlock(&o->m);
	return n && !mutex_unlock(o, n, 1) ? n : 0;
}
void kernel_lwmutex_retake(uint32_t work, int n) { if (n > 0) mutex_lock(lwmutex(work), n, 0, 0, 1); }

/* ---- conditions, lightweight (work, name, attr, lwmutex work, opt) and on kernel mutexes (name,
 * attr, mutex uid, opt). A wait by the mutex's owner gives the mutex up (to the queue head if
 * any), sleeps, and takes it back with the same count whatever the outcome -- signalled, timed
 * out or deleted. Signal wakes the queue head, SignalAll everyone, SignalTo one named thread. */
static uint32_t cond_wait(Obj *o, Obj *mx, uint32_t timeout_p, int lw) {
	uint32_t r = 0;
	pthread_mutex_lock(&o->m);
	if (o->deleted) { pthread_mutex_unlock(&o->m); return lw ? E_UNKNOWN_LW_COND_ID : E_UNKNOWN_COND_ID; }
	pthread_mutex_lock(&mx->m);
	if (!mx->count || mx->owner != cur_tid) {
		pthread_mutex_unlock(&mx->m); pthread_mutex_unlock(&o->m);
		static int said; if (!said++) fprintf(stderr, "    %s waited on by thread %#x without holding its mutex (returns NOT_OWNED, as the kernel's unlock would)\n", lw ? "LwCond" : "Cond", cur_tid);
		return lw ? E_LW_MUTEX_NOT_OWNED : E_MUTEX_NOT_OWNED;
	}
	int held = mx->count;
	mutex_release(mx);
	pthread_mutex_unlock(&mx->m);   /* the signaller needs only o->m, which stays held into the sleep: no signal can slip between the release and the wait */
	Waiter w = { 0 };
	r = wait_on(o, &w, timeout_p, lw ? E_WAIT_DELETE_LW_COND : E_WAIT_DELETE_COND, lw ? 11 : 10);
	pthread_mutex_unlock(&o->m);
	int cbw = cb_wait; cb_wait = 0;   /* taking the mutex back is part of the wait, and not one a callback may interrupt: a retry re-enters with the mutex held */
	mutex_lock(mx, held, 0, 0, lw);
	cb_wait = cbw;
	return r;
}
static uint32_t cond_signal(Obj *o, int all, uint32_t to) {   /* to: a thread uid (SignalTo), 0 for the queue head */
	uint32_t r = 0;
	pthread_mutex_lock(&o->m);
	if (all) while (o->head) wake(o, o->head, 0);
	else if (to) { Waiter *w = o->head; while (w && w->tid != to) w = w->next; if (w) wake(o, w, 0); else r = E_UNKNOWN_THREAD_ID; }
	else if (o->head) wake(o, o->head, 0);
	pthread_mutex_unlock(&o->m);
	return r;
}
static void h_create_lwcond(Cpu *c) {
	if (!A0 || !A3) { RET(E_ILLEGAL_ADDR); return; }
	Obj *o = new_obj_at(LWCOND, A1, A2, A0); o->lwmutex = A3; RET(0);
}
static void h_wait_lwcond(Cpu *c) {   /* (work, *timeout) */
	Obj *o = by_work(A0, LWCOND);
	RET(o ? cond_wait(o, lwmutex(o->lwmutex), A1, 1) : E_UNKNOWN_LW_COND_ID);
}
static void h_signal_lwcond(Cpu *c) { Obj *o = by_work(A0, LWCOND); RET(o ? cond_signal(o, 0, 0) : E_UNKNOWN_LW_COND_ID); }
static void h_signal_lwcond_all(Cpu *c) { Obj *o = by_work(A0, LWCOND); RET(o ? cond_signal(o, 1, 0) : E_UNKNOWN_LW_COND_ID); }
static void h_signal_lwcond_to(Cpu *c) { Obj *o = by_work(A0, LWCOND); RET(o ? cond_signal(o, 0, A1 ? A1 : cur_tid) : E_UNKNOWN_LW_COND_ID); }
static void h_delete_lwcond(Cpu *c) { h_delete_work(c, LWCOND, E_UNKNOWN_LW_COND_ID); }
static void h_create_cond(Cpu *c) {
	if (!by_uid(A2, MUTEX)) { RET(E_UNKNOWN_MUTEX_ID); return; }
	Obj *o = new_obj(KCOND, A0, A1); o->mutex_uid = A2; RET(o->uid);
}
static void h_wait_cond(Cpu *c) {   /* (uid, *timeout) */
	Obj *o = by_uid(A0, KCOND), *mx = o ? by_uid(o->mutex_uid, MUTEX) : 0;
	RET(!o ? E_UNKNOWN_COND_ID : !mx ? E_UNKNOWN_MUTEX_ID : cond_wait(o, mx, A1, 0));
}
static void h_signal_cond(Cpu *c) { Obj *o = by_uid(A0, KCOND); RET(o ? cond_signal(o, 0, 0) : E_UNKNOWN_COND_ID); }
static void h_signal_cond_all(Cpu *c) { Obj *o = by_uid(A0, KCOND); RET(o ? cond_signal(o, 1, 0) : E_UNKNOWN_COND_ID); }
static void h_signal_cond_to(Cpu *c) { Obj *o = by_uid(A0, KCOND); RET(o ? cond_signal(o, 0, A1 ? A1 : cur_tid) : E_UNKNOWN_COND_ID); }
static void h_delete_cond(Cpu *c) { h_delete_uid(c, KCOND, E_UNKNOWN_COND_ID); }
static void h_cond_info(Cpu *c) {   /* (uid, *SceKernelCondInfo 0x34) */
	Obj *o = by_uid(A0, KCOND);
	if (!o) { RET(E_UNKNOWN_COND_ID); return; }
	if (!A1 || RD32(A1) != 0x34) { RET(A1 ? E_INVALID_ARGUMENT_SIZE : E_ILLEGAL_ADDR); return; }
	pthread_mutex_lock(&o->m);
	WR32(A1 + 4, o->uid); memcpy(G(A1 + 8), o->name, 32); WR32(A1 + 0x28, o->attr); WR32(A1 + 0x2c, o->mutex_uid); WR32(A1 + 0x30, o->nwait);
	pthread_mutex_unlock(&o->m);
	RET(0);
}

/* ---- SceUlt (the user-level thread library's synchronisation; on the firmware a user-mode
 * library whose waiters are kernel threads parked with _sceKernelWaitSignal/sceKernelSendSignal):
 * the same host objects, found by the guest object's address. Error codes are SceUlt's. */
static void h_ult_pool_size(Cpu *c) { RET(64 + 16 * (A0 + A1)); }   /* (threads, sync objects) -> work area bytes; the host keeps nothing in it */
static void h_ult_sema_create(Cpu *c) {             /* (semaphore, name, initial count, pool, opt, version) */
	if (!A0) { RET(0x80810001u); return; }
	Obj *o = new_obj_at(SEMA, A1, 0, A0); o->count = o->init = (int)A2; o->max = 0; RET(0);   /* max 0: unbounded */
}
static Obj *ult_sema(uint32_t a) { Obj *o = by_work(a, SEMA); if (!o) o = new_obj_at(SEMA, 0, 0, a); return o; }
static void h_ult_sema_acquire(Cpu *c) { uint32_t r = sema_wait(ult_sema(A0), (int)A1, 0, 0, 0, 0x80810006u); RET(r == E_ILLEGAL_COUNT ? E_ULT_RANGE : r); }   /* (semaphore, count): no timeout */
static void h_ult_sema_try(Cpu *c) { uint32_t r = sema_wait(ult_sema(A0), (int)A1, 0, 1, E_ULT_BUSY, 0x80810006u); RET(r == E_ILLEGAL_COUNT ? E_ULT_RANGE : r); }
static void h_ult_sema_release(Cpu *c) { uint32_t r = sema_signal(ult_sema(A0), (int)A1, 0); RET(r == E_ILLEGAL_COUNT ? E_ULT_RANGE : r); }
static void h_ult_sema_destroy(Cpu *c) { Obj *o = by_work(A0, SEMA); if (!o) { RET(0x80810006u); return; } obj_delete(o); RET(0); }
/* ULT mutexes: taken as recursive here (a superset: a thread that double-locks deadlocks on the
 * firmware, so no shipped game does) */
static void h_ult_mutex_create(Cpu *c) { if (!A0) { RET(0x80810001u); return; } Obj *o = new_obj_at(LWMUTEX, A1, 2, A0); o->recursive = 1; RET(0); }   /* (mutex, name, pool, opt, version) */
static void h_ult_mutex_lock(Cpu *c) { RET(mutex_lock(lwmutex(A0), 1, 0, 0, 1)); }
static void h_ult_mutex_trylock(Cpu *c) { uint32_t r = mutex_lock(lwmutex(A0), 1, 0, 1, 1); RET(r == E_MUTEX_FAILED_TO_OWN + 0x40 ? E_ULT_BUSY : r); }
static void h_ult_mutex_unlock(Cpu *c) { uint32_t r = mutex_unlock(lwmutex(A0), 1, 1); RET(r == E_LW_MUTEX_NOT_OWNED ? E_ULT_PERMISSION : r); }
static void h_ult_mutex_destroy(Cpu *c) { Obj *o = by_work(A0, LWMUTEX); if (!o) { RET(0x80810006u); return; } obj_delete(o); RET(0); }
static void h_ult_cond_create(Cpu *c) { if (!A0 || !A2) { RET(0x80810001u); return; } Obj *o = new_obj_at(LWCOND, A1, 0, A0); o->lwmutex = A2; RET(0); }   /* (cv, name, mutex, opt, version) */
static void h_ult_cond_wait(Cpu *c) {   /* (cv): no timeout; the calling thread holds the mutex */
	Obj *o = by_work(A0, LWCOND);
	if (!o) { RET(0x80810006u); return; }
	uint32_t r = cond_wait(o, lwmutex(o->lwmutex), 0, 1);
	RET(r == E_LW_MUTEX_NOT_OWNED ? E_ULT_PERMISSION : r);
}
static void h_ult_cond_signal(Cpu *c) { Obj *o = by_work(A0, LWCOND); RET(o ? cond_signal(o, 0, 0) : 0x80810006u); }
static void h_ult_cond_signal_all(Cpu *c) { Obj *o = by_work(A0, LWCOND); RET(o ? cond_signal(o, 1, 0) : 0x80810006u); }
static void h_ult_cond_destroy(Cpu *c) { Obj *o = by_work(A0, LWCOND); if (!o) { RET(0x80810006u); return; } obj_delete(o); RET(0); }

/* ---- event flags: (name, attr, initial pattern, opt); attr 0x1000 (SCE_EVENT_WAITMULTIPLE) lets
 * several threads wait at once. A wait is for all (mode bit 0 clear) or any (set) of its bits;
 * when they are there the waiter gets the whole pattern and then clears the whole pattern (mode
 * bit 2, SCE_EVENT_WAITCLEAR) or just its own bits (bit 4, SCE_EVENT_WAITCLEAR_PAT). The
 * previous version had those two clears swapped: a worker waiting for its one bit with
 * WAITCLEAR_PAT wiped every other worker's bit. */
static int evf_match(uint32_t pat, uint32_t bits, uint32_t mode) { return (mode & 1) ? (pat & bits) != 0 : (pat & bits) == bits; }
static void evf_take(Obj *o, uint32_t bits, uint32_t mode, uint32_t out) {
	if (out) WR32(out, o->bits);
	if (mode & 2) o->bits = 0;
	if (mode & 4) o->bits &= ~bits;
}
static void h_create_evf(Cpu *c) { Obj *o = new_obj(EVF, A0, A1); o->bits = o->init = A2; RET(o->uid); }
static uint32_t evf_wait(Obj *o, uint32_t bits, uint32_t mode, uint32_t out, uint32_t timeout_p, int poll) {
	if (!bits) return E_ILLEGAL_PATTERN;
	if (mode & ~7u) return E_ILLEGAL_MODE;
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	if (!poll && !(o->attr & 0x1000) && o->nwait) r = E_EVF_MULTI;
	else if (evf_match(o->bits, bits, mode)) evf_take(o, bits, mode, out);
	else if (poll) { if (out) WR32(out, o->bits); r = E_EVF_COND; }
	else {
		Waiter w = { .bits = bits, .mode = mode, .out = out };
		r = wait_on(o, &w, timeout_p, E_WAIT_DELETE, 4);   /* the setter wrote *out and cleared for us */
		if (r == E_WAIT_TIMEOUT && out) WR32(out, o->bits);
	}
	pthread_mutex_unlock(&o->m);
	return r;
}
static void h_wait_evf(Cpu *c) { Obj *o = by_uid(A0, EVF); RET(o ? evf_wait(o, A1, A2, A3, STK(0), 0) : E_UNKNOWN_EVF_ID); }   /* (uid, bits, mode, *out, *timeout) */
static void h_poll_evf(Cpu *c) { Obj *o = by_uid(A0, EVF); RET(o ? evf_wait(o, A1, A2, A3, 0, 1) : E_UNKNOWN_EVF_ID); }       /* (uid, bits, mode, *out) */
static void h_set_evf(Cpu *c) {
	Obj *o = by_uid(A0, EVF); if (!o) { RET(E_UNKNOWN_EVF_ID); return; }
	pthread_mutex_lock(&o->m);
	o->bits |= A1;
	for (Waiter *w = o->head, *n; w; w = n) {   /* in queue order; each sees what the earlier ones left */
		n = w->next;
		if (evf_match(o->bits, w->bits, w->mode)) { evf_take(o, w->bits, w->mode, w->out); wake(o, w, 0); }
	}
	pthread_mutex_unlock(&o->m);
	RET(0);
}
static void h_clear_evf(Cpu *c) { Obj *o = by_uid(A0, EVF); if (!o) { RET(E_UNKNOWN_EVF_ID); return; } pthread_mutex_lock(&o->m); o->bits &= A1; pthread_mutex_unlock(&o->m); RET(0); }   /* (uid, bits to keep) */
static void h_delete_evf(Cpu *c) { h_delete_uid(c, EVF, E_UNKNOWN_EVF_ID); }
static void h_evf_info(Cpu *c) {   /* (uid, *SceKernelEventFlagInfo 0x38) */
	Obj *o = by_uid(A0, EVF);
	if (!o) { RET(E_UNKNOWN_EVF_ID); return; }
	if (!A1 || RD32(A1) != 0x38) { RET(A1 ? E_INVALID_ARGUMENT_SIZE : E_ILLEGAL_ADDR); return; }
	pthread_mutex_lock(&o->m);
	WR32(A1 + 4, o->uid); memcpy(G(A1 + 8), o->name, 32); WR32(A1 + 0x28, o->attr); WR32(A1 + 0x2c, o->init); WR32(A1 + 0x30, o->bits); WR32(A1 + 0x34, o->nwait);
	pthread_mutex_unlock(&o->m);
	RET(0);
}

/* ---- simple events (sceKernelCreateSimpleEvent: name, attr, initial pattern, opt): a pattern a
 * thread waits on with WaitEvent (any of its bits), which hands back the whole pattern and the
 * 64-bit user data of the last SetEvent, and clears the bits it waited for only when the event was
 * created with SCE_KERNEL_EVENT_ATTR_AUTO_RESET (0x100). Stubbed (every call returned 0 at once) a
 * thread waiting for a job's completion ran ahead of it. */
static void h_create_sevent(Cpu *c) { Obj *o = new_obj(SEVENT, A0, A1); o->bits = o->init = A2; RET(o->uid); }
static void sevent_take(Obj *o, uint32_t bits, uint32_t out, uint32_t udata) {
	if (out) WR32(out, o->bits);
	if (udata) WR64(udata, o->udata);
	if (o->attr & 0x100) o->bits &= ~bits;
}
static uint32_t sevent_wait(Obj *o, uint32_t bits, uint32_t out, uint32_t udata, uint32_t timeout_p, int poll) {
	if (!bits) return E_ILLEGAL_PATTERN;
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	if (o->bits & bits) sevent_take(o, bits, out, udata);
	else if (poll) { if (out) WR32(out, o->bits); r = E_EVENT_COND; }
	else {
		Waiter w = { .bits = bits, .out = out, .udata = udata };
		r = wait_on(o, &w, timeout_p, E_WAIT_DELETE, 12);
		if (r == E_WAIT_TIMEOUT) { if (out) WR32(out, o->bits); if (udata) WR64(udata, o->udata); }
	}
	pthread_mutex_unlock(&o->m);
	return r;
}
static void h_wait_sevent(Cpu *c) { Obj *o = by_uid(A0, SEVENT); RET(o ? sevent_wait(o, A1, A2, A3, STK(0), 0) : E_UNKNOWN_EVENT_ID); }   /* (uid, waitPattern, *resultPattern, *userData, *timeout) */
static void h_poll_sevent(Cpu *c) { Obj *o = by_uid(A0, SEVENT); RET(o ? sevent_wait(o, A1, A2, A3, 0, 1) : E_UNKNOWN_EVENT_ID); }
static void h_set_sevent(Cpu *c) {   /* (uid, setPattern, userData (r2:r3)) */
	Obj *o = by_uid(A0, SEVENT); if (!o) { RET(E_UNKNOWN_EVENT_ID); return; }
	pthread_mutex_lock(&o->m);
	o->bits |= A1; o->udata = (uint64_t)A2 | ((uint64_t)A3 << 32);
	for (Waiter *w = o->head, *n; w; w = n) {
		n = w->next;
		if (o->bits & w->bits) { sevent_take(o, w->bits, w->out, w->udata); wake(o, w, 0); }
	}
	pthread_mutex_unlock(&o->m);
	RET(0);
}
static void h_clear_sevent(Cpu *c) { Obj *o = by_uid(A0, SEVENT); if (!o) { RET(E_UNKNOWN_EVENT_ID); return; } pthread_mutex_lock(&o->m); o->bits &= A1; pthread_mutex_unlock(&o->m); RET(0); }   /* (uid, bits to keep) */
static void h_delete_sevent(Cpu *c) { h_delete_uid(c, SEVENT, E_UNKNOWN_EVENT_ID); }

/* ---- kernel callbacks (sceKernelCreateCallback: name, attr, function, common): a function the
 * kernel runs on the thread that created the callback, as (notifyId, notifyCount, notifyArg,
 * common), when that thread next gives it the chance: sceKernelCheckCallback, or any *CB wait
 * (the wait is interrupted for it and goes on). A notification sets a count (the deliveries since
 * the last run), the last argument and the notifier; a callback that returns nonzero is deleted.
 * The display's vblank callback is one of these (display.c notifies it with the edges passed). */
uint32_t guest_call4(Cpu *parent, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {   /* guest_call (main.c) takes two arguments (hle.h; np.c's callbacks too) */
	extern GuestFn guest_lookup(uint32_t a);
	GuestFn f = guest_lookup(fn & ~1u);
	if (!f) { fprintf(stderr, "callback %#x is not recompiled code\n", fn); return 0; }
	Cpu n = *parent;
	n.r[0] = a0; n.r[1] = a1; n.r[2] = a2; n.r[3] = a3;
	n.r[13] = (parent->r[13] - 512) & ~7u; n.r[14] = 0xfffffffe;
	f(&n);
	return n.r[0];
}
/* a notification for thread t: the flag, then the sleep it published (cblock is released first:
 * the waiter takes cblock under its object's mutex, so the notifier must not hold cblock while
 * taking that mutex; the mutex and condition outlive every use -- objects are never freed) */
static void cb_wake(Thread *t) {
	pthread_mutex_lock(&cblock);
	__atomic_store_n(&t->cb_pending, 1, __ATOMIC_RELEASE);
	pthread_mutex_t *m = t->wait_m; pthread_cond_t *cv = t->wait_cv;
	pthread_mutex_unlock(&cblock);
	if (m) { pthread_mutex_lock(m); pthread_cond_broadcast(cv); pthread_mutex_unlock(m); }
}
static uint32_t cb_notify(Obj *o, uint32_t notifier, uint32_t count, uint32_t arg) {
	pthread_mutex_lock(&o->m);
	o->ncount += count; o->notify_arg = arg; o->notifier = notifier;
	uint32_t owner = o->owner;
	pthread_mutex_unlock(&o->m);
	Thread *t = thread_of(owner);
	if (t) cb_wake(t);
	return 0;
}
/* the calling thread's pending callbacks, run in creation order -> how many ran. The display's
 * vblank callbacks are brought up to date first (their notifications come from the vblank grid,
 * not a thread). */
static uint32_t run_callbacks(Cpu *c) {
	if (in_callbacks) return 0;
	Thread *t = SELF();
	in_callbacks = 1;
	display_vblank_callbacks();
	__atomic_store_n(&t->cb_pending, 0, __ATOMIC_RELEASE);
	uint32_t ran = 0;
	for (int i = 0; i < nobjs; i++) {
		Obj *o = obj_at((uint32_t)i);
		if (!o || o->kind != CALLBACK || o->deleted || o->owner != t->uid) continue;
		pthread_mutex_lock(&o->m);
		int n = o->ncount; uint32_t arg = o->notify_arg, id = o->notifier;
		o->ncount = 0; o->notifier = 0;
		pthread_mutex_unlock(&o->m);
		if (!n) continue;
		ran++;
		if (guest_call4(c, o->func, id, (uint32_t)n, arg, o->common)) obj_delete(o);   /* nonzero: the callback deletes itself */
	}
	in_callbacks = 0;
	return ran;
}
uint32_t kernel_check_callbacks(Cpu *c) { return run_callbacks(c); }
/* the run's end summary (main.c): every mutex and lightweight mutex held while threads wait on it -- its owner
 * and the waiters, by uid and name. A hang shows as threads parked in LockLwMutex; this names who holds the lock
 * (Killzone, 2026-10-16: the main thread and both SndStreamThreads in an in-level LockLwMutex(0x81c49a00), owner
 * unknown). Try-locks only: a summary taken while some thread sits inside an object's lock never blocks on it. */
void kernel_report_locks(void) {
	int said = 0;
	for (int i = 0; i < nobjs; i++) {
		Obj *o = obj_at((uint32_t)i);
		if (!o || (o->kind != MUTEX && o->kind != LWMUTEX) || o->deleted || !o->nwait) continue;
		if (pthread_mutex_trylock(&o->m)) continue;
		if (!said++) fprintf(stderr, "== contended locks (owner, waiters)\n");
		Thread *ow = o->count ? thread_of(o->owner) : 0;
		fprintf(stderr, "  %s %#x%s%.32s: owner %#x (%s) count %d; waiting:", o->kind == LWMUTEX ? "lwmutex" : "mutex", o->kind == LWMUTEX ? o->work : o->uid,
		        o->name[0] ? " " : "", o->name, o->count ? o->owner : 0, ow ? ow->name : o->count ? "?" : "free", o->count);
		for (Waiter *w = o->head; w; w = w->next) { Thread *t = thread_of(w->tid); fprintf(stderr, " %#x (%s)", w->tid, t ? t->name : "?"); }
		fprintf(stderr, "\n");
		pthread_mutex_unlock(&o->m);
	}
}
void kernel_notify_callback(uint32_t uid, uint32_t count, uint32_t arg) { Obj *o = by_uid(uid, CALLBACK); if (o) cb_notify(o, 0, count, arg); }
int kernel_callback_valid(uint32_t uid) { return by_uid(uid, CALLBACK) != 0; }
/* sleep until a CLOCK_MONOTONIC time, running this thread's callbacks as they are notified
 * (DelayThreadCB, the display's CB waits) */
void kernel_cb_sleep(Cpu *c, uint64_t until_us) {
	Thread *t = SELF();
	for (;;) {
		uint64_t now = mono_us();
		if (until_us <= now) return;
		uint64_t left = until_us - now;
		if (in_callbacks) { int b = sched_block_begin(); usleep((useconds_t)left); if (b) sched_block_end(); return; }   /* no nesting: a plain sleep */
		struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += (time_t)(left / 1000000); ts.tv_nsec += (long)((left % 1000000) * 1000);
		if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		pthread_cond_t *cv = wait_cv();
		pthread_mutex_lock(&cblock);
		t->wait_m = &cblock; t->wait_cv = cv;
		int b = cb_pending(t) ? 0 : sched_block_begin();
		while (!cb_pending(t) && pthread_cond_timedwait(cv, &cblock, &ts) != ETIMEDOUT) ;
		t->wait_m = 0; t->wait_cv = 0;
		int pending = cb_pending(t);
		pthread_mutex_unlock(&cblock);
		if (b) sched_block_end();   /* the callbacks are guest code: they run on a slot */
		if (pending) run_callbacks(c);
	}
}
/* a *CB system call: the pending callbacks first (the kernel delivers them before the wait
 * begins), then the operation, repeated with the time left each time a callback interrupted it */
static void cb_call(Cpu *c, HleFn inner) {
	if (in_callbacks) { inner(c); return; }
	uint32_t a[4] = { c->r[0], c->r[1], c->r[2], c->r[3] };
	run_callbacks(c);
	cb_wait++;
	for (;;) {
		c->r[0] = a[0]; c->r[1] = a[1]; c->r[2] = a[2]; c->r[3] = a[3];
		inner(c);
		if (c->r[0] != E_CB_RETRY) break;
		run_callbacks(c);
	}
	cb_wait--;
}
static void h_create_callback(Cpu *c) {   /* (name, attr, function, common): owned by the calling thread */
	if (A1) { RET(E_ILLEGAL_ATTR); return; }
	if (!A2) { RET(E_ILLEGAL_ADDR); return; }
	Obj *o = new_obj(CALLBACK, A0, A1); o->owner = cur_tid; o->func = A2; o->common = A3; RET(o->uid);
}
static void h_delete_callback(Cpu *c) { h_delete_uid(c, CALLBACK, E_UNKNOWN_CALLBACK_ID); }
static void h_notify_callback(Cpu *c) { Obj *o = by_uid(A0, CALLBACK); RET(o ? cb_notify(o, cur_tid, 1, A1) : E_UNKNOWN_CALLBACK_ID); }   /* (uid, notifyArg) */
static void h_cancel_callback(Cpu *c) { Obj *o = by_uid(A0, CALLBACK); if (!o) { RET(E_UNKNOWN_CALLBACK_ID); return; } pthread_mutex_lock(&o->m); o->ncount = 0; pthread_mutex_unlock(&o->m); RET(0); }
static void h_callback_count(Cpu *c) { Obj *o = by_uid(A0, CALLBACK); RET(o ? (uint32_t)o->ncount : E_UNKNOWN_CALLBACK_ID); }
static void h_check_callback(Cpu *c) { RET(run_callbacks(c)); }
static void h_callback_info(Cpu *c) {   /* (uid, *SceKernelCallbackInfo 0x3c) */
	Obj *o = by_uid(A0, CALLBACK);
	if (!o) { RET(E_UNKNOWN_CALLBACK_ID); return; }
	if (!A1 || RD32(A1) != 0x3c) { RET(A1 ? E_INVALID_ARGUMENT_SIZE : E_ILLEGAL_ADDR); return; }
	pthread_mutex_lock(&o->m);
	WR32(A1 + 4, o->uid); memcpy(G(A1 + 8), o->name, 32); WR32(A1 + 0x28, o->owner); WR32(A1 + 0x2c, o->func); WR32(A1 + 0x30, o->common);
	WR32(A1 + 0x34, (uint32_t)o->ncount); WR32(A1 + 0x38, o->notify_arg);
	pthread_mutex_unlock(&o->m);
	RET(0);
}
static void h_wait_sema_cb(Cpu *c) { cb_call(c, h_wait_sema); }
static void h_wait_evf_cb(Cpu *c) { cb_call(c, h_wait_evf); }
static void h_wait_sevent_cb(Cpu *c) { cb_call(c, h_wait_sevent); }
static void h_lock_mutex_cb(Cpu *c) { cb_call(c, h_lock_mutex); }
static void h_lock_lwmutex_cb(Cpu *c) { cb_call(c, h_lock_lwmutex); }
static void h_wait_cond_cb(Cpu *c) { cb_call(c, h_wait_cond); }
static void h_wait_lwcond_cb(Cpu *c) { cb_call(c, h_wait_lwcond); }
static void h_wait_thread_end_cb(Cpu *c) { cb_call(c, h_wait_thread_end); }
static void h_delay_cb(Cpu *c) {
	uint32_t us = A0 ? A0 : 1;
	run_callbacks(c);
	thread_waits(2, 0);
	kernel_cb_sleep(c, mono_us() + us);
	thread_waits(0, 0);
	guest_time_slept(us);
	RET(0);
}

/* ---- reader-writer locks (sceKernelCreateRWLock: name, attr, opt): any number of readers or
 * one writer; a reader may lock read again, the writer may lock write again only with attr bit 1
 * (recursive) and may also read; a new reader waits behind a queued writer (else a steady stream
 * of readers starves the writer, and under host scheduling that is a stall), a reader that
 * already holds it passes. An unlock that frees the lock grants the queue in order: the readers
 * up to the first writer, or that writer alone if it is first. Codes are the SDK's RW_LOCK ones. */
static int rw_reader(Obj *o, uint32_t tid) { for (int i = 0; i < o->nreaders; i++) if (o->readers[i].tid == tid) return i; return -1; }
static void rw_add_reader(Obj *o, uint32_t tid) {
	int i = rw_reader(o, tid);
	if (i >= 0) { o->readers[i].n++; return; }
	if (o->nreaders == o->readers_cap) { o->readers_cap = o->readers_cap ? 2 * o->readers_cap : 8; o->readers = realloc(o->readers, o->readers_cap * sizeof *o->readers); }
	o->readers[o->nreaders].tid = tid; o->readers[o->nreaders].n = 1; o->nreaders++;
}
static int rw_writer_queued(Obj *o) { for (Waiter *w = o->head; w; w = w->next) if (w->mode == 2) return 1; return 0; }
static void rw_release(Obj *o) {   /* o->m held, nobody holds the lock: grant the queue */
	for (Waiter *w = o->head, *n; w; w = n) {
		n = w->next;
		if (w->mode == 2) { if (!o->nreaders) { o->owner = w->tid; o->count = 1; wake(o, w, 0); break; } }   /* a writer behind granted readers keeps waiting */
		else { rw_add_reader(o, w->tid); wake(o, w, 0); }
	}
}
static uint32_t rwlock_lock(Obj *o, int write, uint32_t timeout_p, int try_only) {
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	int held_read = rw_reader(o, cur_tid) >= 0;
	if (!write) {
		if (held_read || (!o->owner && !rw_writer_queued(o)) || o->owner == cur_tid) rw_add_reader(o, cur_tid);
		else if (try_only) r = E_RW_LOCK_FAILED_TO_LOCK;
		else { Waiter w = { .mode = 1 }; r = wait_on(o, &w, timeout_p, E_WAIT_DELETE, 14); }
	} else {
		if (o->owner == cur_tid) { if (o->attr & 2) o->count++; else r = E_RW_LOCK_RECURSIVE; }
		else if (held_read) { r = try_only ? E_RW_LOCK_FAILED_TO_LOCK : E_RW_LOCK_RECURSIVE; static int said; if (!try_only && !said++) fprintf(stderr, "    RWLock: thread %#x asked to write while reading (deadlock on the hardware; refused)\n", cur_tid); }
		else if (!o->owner && !o->nreaders) { o->owner = cur_tid; o->count = 1; }
		else if (try_only) r = E_RW_LOCK_FAILED_TO_LOCK;
		else { Waiter w = { .mode = 2 }; r = wait_on(o, &w, timeout_p, E_WAIT_DELETE, 14); }
	}
	pthread_mutex_unlock(&o->m);
	return r;
}
static uint32_t rwlock_unlock(Obj *o, int write) {
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	if (write) {
		if (o->owner != cur_tid) r = E_RW_LOCK_NOT_OWNED;
		else if (!--o->count) { o->owner = 0; if (!o->nreaders) rw_release(o); }
	} else {
		int i = rw_reader(o, cur_tid);
		if (i < 0) r = E_RW_LOCK_NOT_OWNED;
		else if (!--o->readers[i].n) { o->readers[i] = o->readers[--o->nreaders]; if (!o->nreaders && !o->owner) rw_release(o); }
	}
	pthread_mutex_unlock(&o->m);
	return r;
}
static void h_create_rwlock(Cpu *c) { if (!A0) { RET(E_ILLEGAL_ADDR); return; } Obj *o = new_obj(RWLOCK, A0, A1); RET(o->uid); }
static void h_delete_rwlock(Cpu *c) { h_delete_uid(c, RWLOCK, E_UNKNOWN_RW_LOCK_ID); }
static void h_lock_read_rwlock(Cpu *c) { Obj *o = by_uid(A0, RWLOCK); RET(o ? rwlock_lock(o, 0, A1, 0) : E_UNKNOWN_RW_LOCK_ID); }   /* (uid, *timeout) */
static void h_lock_write_rwlock(Cpu *c) { Obj *o = by_uid(A0, RWLOCK); RET(o ? rwlock_lock(o, 1, A1, 0) : E_UNKNOWN_RW_LOCK_ID); }
static void h_trylock_read_rwlock(Cpu *c) { Obj *o = by_uid(A0, RWLOCK); RET(o ? rwlock_lock(o, 0, 0, 1) : E_UNKNOWN_RW_LOCK_ID); }
static void h_trylock_write_rwlock(Cpu *c) { Obj *o = by_uid(A0, RWLOCK); RET(o ? rwlock_lock(o, 1, 0, 1) : E_UNKNOWN_RW_LOCK_ID); }
static void h_unlock_read_rwlock(Cpu *c) { Obj *o = by_uid(A0, RWLOCK); RET(o ? rwlock_unlock(o, 0) : E_UNKNOWN_RW_LOCK_ID); }
static void h_unlock_write_rwlock(Cpu *c) { Obj *o = by_uid(A0, RWLOCK); RET(o ? rwlock_unlock(o, 1) : E_UNKNOWN_RW_LOCK_ID); }
static void h_lock_read_rwlock_cb(Cpu *c) { cb_call(c, h_lock_read_rwlock); }
static void h_lock_write_rwlock_cb(Cpu *c) { cb_call(c, h_lock_write_rwlock); }
static void h_rwlock_info(Cpu *c) {   /* (uid, *SceKernelRWLockInfo 0x3c): lockCount is the writer's count, or the read holds */
	Obj *o = by_uid(A0, RWLOCK);
	if (!o) { RET(E_UNKNOWN_RW_LOCK_ID); return; }
	if (!A1 || RD32(A1) != 0x3c) { RET(A1 ? E_INVALID_ARGUMENT_SIZE : E_ILLEGAL_ADDR); return; }
	pthread_mutex_lock(&o->m);
	int reads = 0, rwait = 0, wwait = 0;
	for (int i = 0; i < o->nreaders; i++) reads += o->readers[i].n;
	for (Waiter *w = o->head; w; w = w->next) { if (w->mode == 2) wwait++; else rwait++; }
	WR32(A1 + 4, o->uid); memcpy(G(A1 + 8), o->name, 32); WR32(A1 + 0x28, o->attr);
	WR32(A1 + 0x2c, o->owner ? (uint32_t)o->count : (uint32_t)reads); WR32(A1 + 0x30, o->owner); WR32(A1 + 0x34, (uint32_t)rwait); WR32(A1 + 0x38, (uint32_t)wwait);
	pthread_mutex_unlock(&o->m);
	RET(0);
}

/* ---- message pipes (sceKernelCreateMsgPipe: name, type, attr, bufSize, opt): a byte stream of
 * bufSize bytes. Send/Receive(uid, buf, size, waitMode, *result, *timeout) move as much as fits
 * at once (ASAP, mode bit 0 clear) or all of it (FULL, bit 0 set), waiting otherwise; the Try
 * forms and mode bit 4 (DONT_WAIT) return MSG_PIPE_FULL/EMPTY instead; mode bit 8 (DONT_REMOVE)
 * peeks. *result gets the bytes moved. Senders go in order and receivers in order (a later FULL
 * sender can't overtake an earlier one: messages stay whole), each side's queue head served
 * first; a transfer by one side grants the other's waiters. Cancel wakes every waiter with
 * WAIT_CANCEL and counts them. */
#define PIPE_SENDER 0x10000u   /* in a waiter's mode, above the wait-mode bits */
static int pipe_try(Obj *o, int send, uint32_t buf, int size, uint32_t mode, uint32_t result_p) {   /* o->m held -> bytes moved, or -1: must wait */
	int avail = send ? o->cap - o->used : o->used;
	int n = (mode & 1) ? (avail >= size ? size : 0) : (avail < size ? avail : size);
	if (!n && size) return -1;
	for (int i = 0; i < n; i++) {
		if (send) o->ring[(o->rpos + o->used + i) % o->cap] = RD8(buf + (uint32_t)i);
		else WR8(buf + (uint32_t)i, o->ring[(o->rpos + i) % o->cap]);
	}
	if (send) o->used += n;
	else if (!(mode & 0x100)) { o->rpos = (o->rpos + n) % o->cap; o->used -= n; }
	if (result_p) WR32(result_p, (uint32_t)n);
	return n;
}
static void pipe_grant(Obj *o) {   /* o->m held: waiters whose request now fits, each side in queue order, until neither side can move */
	for (int progress = 1; progress; ) {
		progress = 0;
		int sblocked = 0, rblocked = 0;
		for (Waiter *w = o->head, *n; w; w = n) {
			n = w->next;
			int send = (w->mode & PIPE_SENDER) != 0;
			if (send ? sblocked : rblocked) continue;
			if (pipe_try(o, send, w->out, (int)w->bits, w->mode & 0xffff, w->udata) >= 0) { wake(o, w, 0); progress = 1; }
			else if (send) sblocked = 1; else rblocked = 1;
		}
	}
}
static uint32_t pipe_xfer(Obj *o, int send, uint32_t buf, uint32_t size, uint32_t mode, uint32_t result_p, uint32_t timeout_p, int try_only) {
	if (size > (uint32_t)o->cap) return E_ILLEGAL_SIZE;
	pthread_mutex_lock(&o->m);
	uint32_t r = 0;
	int queued = 0;   /* a waiter of my side ahead of me: I queue behind it */
	for (Waiter *w = o->head; w; w = w->next) if (((w->mode & PIPE_SENDER) != 0) == send) { queued = 1; break; }
	if (!queued && pipe_try(o, send, buf, (int)size, mode, result_p) >= 0) pipe_grant(o);
	else if (try_only || (mode & 0x10)) r = send ? E_MSG_PIPE_FULL : E_MSG_PIPE_EMPTY;
	else { Waiter w = { .mode = (send ? PIPE_SENDER : 0) | (mode & 0xffff), .bits = size, .out = buf, .udata = result_p }; r = wait_on(o, &w, timeout_p, E_WAIT_DELETE, 13); }
	pthread_mutex_unlock(&o->m);
	return r;
}
static void h_create_msgpipe(Cpu *c) {   /* (name, type, attr, bufSize, opt) */
	if (!A0) { RET(E_ILLEGAL_ADDR); return; }
	if (!A3 || A3 > (64u << 20)) { RET(E_ILLEGAL_SIZE); return; }
	Obj *o = new_obj(MSGPIPE, A0, A2); o->cap = (int)A3; o->ring = malloc(A3); RET(o->uid);
}
static void h_delete_msgpipe(Cpu *c) { h_delete_uid(c, MSGPIPE, E_UNKNOWN_MSG_PIPE_ID); }
static void h_send_msgpipe(Cpu *c) { Obj *o = by_uid(A0, MSGPIPE); RET(o ? pipe_xfer(o, 1, A1, A2, A3, STK(0), STK(1), 0) : E_UNKNOWN_MSG_PIPE_ID); }
static void h_receive_msgpipe(Cpu *c) { Obj *o = by_uid(A0, MSGPIPE); RET(o ? pipe_xfer(o, 0, A1, A2, A3, STK(0), STK(1), 0) : E_UNKNOWN_MSG_PIPE_ID); }
static void h_trysend_msgpipe(Cpu *c) { Obj *o = by_uid(A0, MSGPIPE); RET(o ? pipe_xfer(o, 1, A1, A2, A3, STK(0), 0, 1) : E_UNKNOWN_MSG_PIPE_ID); }   /* (uid, buf, size, mode, *result) */
static void h_tryreceive_msgpipe(Cpu *c) { Obj *o = by_uid(A0, MSGPIPE); RET(o ? pipe_xfer(o, 0, A1, A2, A3, STK(0), 0, 1) : E_UNKNOWN_MSG_PIPE_ID); }
static void h_send_msgpipe_cb(Cpu *c) { cb_call(c, h_send_msgpipe); }
static void h_receive_msgpipe_cb(Cpu *c) { cb_call(c, h_receive_msgpipe); }
static void h_cancel_msgpipe(Cpu *c) {   /* (uid, *sendersCancelled, *receiversCancelled) */
	Obj *o = by_uid(A0, MSGPIPE);
	if (!o) { RET(E_UNKNOWN_MSG_PIPE_ID); return; }
	pthread_mutex_lock(&o->m);
	int ns = 0, nr = 0;
	while (o->head) { if (o->head->mode & PIPE_SENDER) ns++; else nr++; wake(o, o->head, E_WAIT_CANCEL); }
	pthread_mutex_unlock(&o->m);
	if (A1) WR32(A1, (uint32_t)ns);
	if (A2) WR32(A2, (uint32_t)nr);
	RET(0);
}
static void h_msgpipe_info(Cpu *c) {   /* (uid, *SceKernelMppInfo 0x3c) */
	Obj *o = by_uid(A0, MSGPIPE);
	if (!o) { RET(E_UNKNOWN_MSG_PIPE_ID); return; }
	if (!A1 || RD32(A1) != 0x3c) { RET(A1 ? E_INVALID_ARGUMENT_SIZE : E_ILLEGAL_ADDR); return; }
	pthread_mutex_lock(&o->m);
	int ns = 0, nr = 0;
	for (Waiter *w = o->head; w; w = w->next) { if (w->mode & PIPE_SENDER) ns++; else nr++; }
	WR32(A1 + 4, o->uid); memcpy(G(A1 + 8), o->name, 32); WR32(A1 + 0x28, o->attr);
	WR32(A1 + 0x2c, (uint32_t)o->cap); WR32(A1 + 0x30, (uint32_t)(o->cap - o->used)); WR32(A1 + 0x34, (uint32_t)ns); WR32(A1 + 0x38, (uint32_t)nr);
	pthread_mutex_unlock(&o->m);
	RET(0);
}

/* ---- fibers (SceFiber, libfiber.suprx on the console). A fiber is a coroutine of a thread:
 * sceFiberRun hands the thread's control to it (its entry(argOnInitialize, argOnRun) the first
 * time, or back to the instruction after the sceFiberReturnToThread/sceFiberSwitch it last left
 * by), sceFiberReturnToThread hands control back to the thread, sceFiberSwitch to another fiber
 * directly; exactly one of the thread and its fibers runs at a time, and the fiber runs AS the
 * thread (its uid, its TLS). libfiber itself can't be recompiled: its switch resumes in the middle
 * of a function, which the recompiled C cannot do -- so a fiber here is a host thread of its own,
 * over its own Cpu and the guest stack the game supplied, parked on a handoff while it isn't the
 * one running: the same way the host runs ULT ulthreads. A fiber with no context (addrContext 0)
 * runs on its thread's stack below the caller and starts from its entry at every Run, as the
 * SDK's does. The game's SceFiber (128 bytes it owns) holds a tag and the host object's address. */
typedef struct Fiber {
	uint32_t addr;                    /* the guest SceFiber */
	uint32_t entry, arg_init, ctx, ctx_size;
	char name[32];
	enum { F_INIT, F_RUNNING, F_SUSPENDED } state;
	int started, go, finalize;        /* started: it has a host thread; go: it may run; finalize: it must end */
	Thread *thread;                   /* the thread it runs under: its identity */
	uint32_t run_arg, run_arg_p;      /* the argOnRun handed to it by the Run/Switch resuming it, and where it asked for it to be written */
	uint32_t sp0;                     /* a context-less fiber's stack (below the thread's) */
	pthread_cond_t cv;
	Cpu cpu;
} Fiber;
#define FIBER_TAG 0x52424946u   /* 'FIBR' at +0 of the game's SceFiber; the Fiber's address at +8 */
static pthread_mutex_t flock = PTHREAD_MUTEX_INITIALIZER;
static __thread Fiber *cur_fiber;   /* on a fiber's host thread: the fiber */
static Fiber *fiber_of(uint32_t addr) {
	if (!addr || (addr & 7) || RD32(addr) != FIBER_TAG) return 0;
	return (Fiber *)(uintptr_t)RD64(addr + 8);
}
static pthread_cond_t *thread_fcv(Thread *t) { if (!t->fcv) { t->fcv = malloc(sizeof *t->fcv); pthread_cond_init(t->fcv, 0); } return t->fcv; }   /* flock held */
static void fiber_identity(Fiber *f) {   /* the fiber's host thread is the thread it runs under: uid, TLS (sceKernelGetThreadId, errno, ...) */
	Thread *t = f->thread;
	cur_tid = t->uid; cur_thread = t == &main_thread ? 0 : t; recomp_tpidruro = t->tls; cur_fiber = f;
}
static void fiber_park(Fiber *f) {   /* flock held: until a Run/Switch resumes it; a finalize ends the host thread (its guest stack is abandoned) */
	while (!f->go && !f->finalize) pthread_cond_wait(&f->cv, &flock);
	f->go = 0;
	if (f->finalize) { pthread_mutex_unlock(&flock); pthread_cond_destroy(&f->cv); free(f); pthread_exit(0); }
	fiber_identity(f);
	if (f->run_arg_p) WR32(f->run_arg_p, f->run_arg);
}
static void *fiber_main(void *p) {
	Fiber *f = p;
	{ char nm[16]; snprintf(nm, sizeof nm, "%s", f->name);
#ifdef __APPLE__
	  pthread_setname_np(nm);
#else
	  pthread_setname_np(pthread_self(), nm);
#endif
	}
	recomp_runfast();
	pthread_mutex_lock(&flock);
	fiber_identity(f);
	Cpu *c = &f->cpu;
	memset(c, 0, sizeof *c);
	c->r[0] = f->arg_init; c->r[1] = f->run_arg;
	c->r[13] = f->ctx ? (f->ctx + f->ctx_size) & ~7u : f->sp0; c->r[14] = 0xfffffffe; c->fpscr = 0x03000000;
	pthread_mutex_unlock(&flock);
	extern GuestFn guest_lookup(uint32_t a);
	GuestFn fn = guest_lookup(f->entry & ~1u);
	if (fn) fn(c); else fprintf(stderr, "fiber '%s': entry %#x is not recompiled code\n", f->name, f->entry);
	/* an entry that returns: the SDK forbids it (a fiber ends by returning to its thread); hand control back so the thread isn't lost */
	fprintf(stderr, "fiber '%s' returned from its entry %#x (the SDK requires sceFiberReturnToThread)\n", f->name, f->entry);
	pthread_mutex_lock(&flock);
	Thread *t = f->thread;
	f->state = F_SUSPENDED; f->run_arg_p = 0; t->fiber = 0; t->fret_arg = 0; t->fgo = 1; pthread_cond_broadcast(thread_fcv(t));
	fiber_park(f);
	pthread_mutex_unlock(&flock);
	return 0;
}
static void fiber_resume(Fiber *f, Thread *t, uint32_t arg) {   /* flock held: f runs under t, told arg */
	f->thread = t; f->run_arg = arg; f->state = F_RUNNING; t->fiber = f;
	if (f->started) { f->go = 1; pthread_cond_broadcast(&f->cv); return; }
	f->started = 1;
	pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16u << 20); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_t th; pthread_create(&th, &at, fiber_main, f);
}
static void fiber_thread_exits(void) {   /* h_exit_thread on a fiber's host thread: the thread's own host thread, parked in h_fiber_run, ends too */
	Fiber *f = cur_fiber;
	if (!f) return;
	pthread_mutex_lock(&flock);
	Thread *t = f->thread;
	t->fiber = 0; t->fexit = 1; t->fgo = 1; pthread_cond_broadcast(thread_fcv(t));
	pthread_mutex_unlock(&flock);
}
static uint32_t fiber_init(Cpu *c, uint32_t fiber, uint32_t name, uint32_t entry, uint32_t arg, uint32_t ctx, uint32_t size) {
	if (!fiber || !entry || !name) return E_FIBER_NULL;
	if (fiber & 7) return E_FIBER_ALIGNMENT;
	if (size && size < 512) return E_FIBER_RANGE;
	if ((!ctx) != (!size) || (size & 7) || (ctx & 7)) return E_FIBER_INVALID;
	Fiber *f = calloc(1, sizeof *f);
	pthread_cond_init(&f->cv, 0);
	f->addr = fiber; f->entry = entry; f->arg_init = arg; f->ctx = ctx; f->ctx_size = size;
	snprintf(f->name, sizeof f->name, "%s", (const char *)G(name));
	WR32(fiber, FIBER_TAG); WR32(fiber + 4, 0); WR64(fiber + 8, (uint64_t)(uintptr_t)f);
	(void)c;
	return 0;
}
static void h_fiber_init(Cpu *c) { RET(fiber_init(c, A0, A1, A2, A3, STK(0), STK(1))); }   /* (fiber, name, entry, argOnInitialize, addrContext, sizeContext, params[, sdk version]) */
static void h_fiber_opt_param_init(Cpu *c) { if (!A0) { RET(E_FIBER_NULL); return; } memset(G(A0), 0, 128); RET(0); }
static void h_fiber_run(Cpu *c) {   /* (fiber, argOnRunTo, *argOnReturn): from a thread */
	Fiber *f = fiber_of(A0);
	if (!A0) { RET(E_FIBER_NULL); return; }
	if (!f) { RET(E_FIBER_INVALID); return; }
	if (cur_fiber) { RET(E_FIBER_PERMISSION); return; }
	Thread *t = SELF();
	pthread_mutex_lock(&flock);
	if (f->state == F_RUNNING) { pthread_mutex_unlock(&flock); RET(E_FIBER_STATE); return; }
	t->tls = recomp_tpidruro;   /* this host thread IS the thread: its TLS is what its fibers share */
	if (!f->ctx) {   /* context-less: a fresh start every run, on this thread's stack below the caller */
		if (f->started) {   /* its previous run is parked mid-entry: that incarnation ends; a new host thread starts the entry over */
			Fiber *old = f; f = calloc(1, sizeof *f); *f = *old; f->started = f->go = 0; pthread_cond_init(&f->cv, 0);
			WR64(f->addr + 8, (uint64_t)(uintptr_t)f);
			old->finalize = 1; pthread_cond_broadcast(&old->cv);
		}
		f->sp0 = (c->r[13] - 512) & ~7u;
	}
	fiber_resume(f, t, A1);
	while (!t->fgo) pthread_cond_wait(thread_fcv(t), &flock);
	t->fgo = 0;
	if (t->fexit) { pthread_mutex_unlock(&flock); pthread_exit(0); }   /* the fiber ended the thread */
	uint32_t ret = t->fret_arg;
	pthread_mutex_unlock(&flock);
	if (A2) WR32(A2, ret);
	RET(0);
}
static void h_fiber_return(Cpu *c) {   /* (argOnReturn, *argOnRun): from a fiber, back to its thread */
	Fiber *f = cur_fiber;
	if (!f) { RET(E_FIBER_PERMISSION); return; }
	pthread_mutex_lock(&flock);
	Thread *t = f->thread;
	f->state = F_SUSPENDED; f->run_arg_p = A1; t->fiber = 0; t->fret_arg = A0; t->fgo = 1; pthread_cond_broadcast(thread_fcv(t));
	fiber_park(f);   /* resumed by a later Run (perhaps from another thread): *argOnRun written */
	pthread_mutex_unlock(&flock);
	RET(0);
}
static void h_fiber_switch(Cpu *c) {   /* (fiber, argOnRunTo, *argOnRun): from a fiber, to another, under the same thread */
	Fiber *f = cur_fiber, *g = fiber_of(A0);
	if (!A0) { RET(E_FIBER_NULL); return; }
	if (!g) { RET(E_FIBER_INVALID); return; }
	if (!f) { RET(E_FIBER_PERMISSION); return; }
	pthread_mutex_lock(&flock);
	if (g->state == F_RUNNING) { pthread_mutex_unlock(&flock); RET(E_FIBER_STATE); return; }
	if (!g->ctx) {
		if (g->started) { Fiber *old = g; g = calloc(1, sizeof *g); *g = *old; g->started = g->go = 0; pthread_cond_init(&g->cv, 0); WR64(g->addr + 8, (uint64_t)(uintptr_t)g); old->finalize = 1; pthread_cond_broadcast(&old->cv); }
		g->sp0 = (c->r[13] - 512) & ~7u;   /* below this fiber's own stack (the thread's, if this one is context-less too) */
	}
	f->state = F_SUSPENDED; f->run_arg_p = A2;
	fiber_resume(g, f->thread, A1);
	fiber_park(f);
	pthread_mutex_unlock(&flock);
	RET(0);
}
static void h_fiber_get_self(Cpu *c) { if (!A0) { RET(E_FIBER_NULL); return; } WR32(A0, cur_fiber ? cur_fiber->addr : 0); RET(0); }   /* (SceFiber **): 0 on a thread */
static void h_fiber_finalize(Cpu *c) {
	Fiber *f = fiber_of(A0);
	if (!A0) { RET(E_FIBER_NULL); return; }
	if (!f) { RET(E_FIBER_INVALID); return; }
	pthread_mutex_lock(&flock);
	if (f->state == F_RUNNING) { pthread_mutex_unlock(&flock); RET(E_FIBER_STATE); return; }
	WR32(f->addr, 0); WR64(f->addr + 8, 0);
	if (f->started) { f->finalize = 1; pthread_cond_broadcast(&f->cv); }   /* its parked host thread frees it */
	else { pthread_cond_destroy(&f->cv); free(f); }
	pthread_mutex_unlock(&flock);
	RET(0);
}
static void h_fiber_info(Cpu *c) {   /* (fiber, *SceFiberInfo 0x170): entry, argOnInitialize, addrContext, sizeContext, name[32], sizeContextMargin */
	Fiber *f = fiber_of(A0);
	if (!A0 || !A1) { RET(E_FIBER_NULL); return; }
	if (!f) { RET(E_FIBER_INVALID); return; }
	memset(G(A1), 0, 0x170);
	WR32(A1, f->entry); WR32(A1 + 4, f->arg_init); WR32(A1 + 8, f->ctx); WR32(A1 + 12, f->ctx_size); memcpy(G(A1 + 16), f->name, 32); WR32(A1 + 48, 0xffffffffu);
	RET(0);
}

/* ---- odds and ends of the thread manager */
static void h_lwmutex_info(Cpu *c) {   /* (work, *SceKernelLwMutexInfo 0x40) */
	Obj *o = by_work(A0, LWMUTEX);
	if (!o) { RET(E_UNKNOWN_LW_MUTEX_ID); return; }
	if (!A1 || RD32(A1) != 0x40) { RET(A1 ? E_INVALID_ARGUMENT_SIZE : E_ILLEGAL_ADDR); return; }
	pthread_mutex_lock(&o->m);
	WR32(A1 + 4, o->uid); memcpy(G(A1 + 8), o->name, 32); WR32(A1 + 0x28, o->attr); WR32(A1 + 0x2c, o->work);
	WR32(A1 + 0x30, (uint32_t)o->init); WR32(A1 + 0x34, (uint32_t)o->count); WR32(A1 + 0x38, o->owner); WR32(A1 + 0x3c, (uint32_t)o->nwait);
	pthread_mutex_unlock(&o->m);
	RET(0);
}
/* (SceKernelCallFrame *buf, bytes, *numFrames, mode): the recompiled code keeps no guest frames to walk -- an empty backtrace */
static void h_backtrace_self(Cpu *c) { if (A2) WR32(A2, 0); RET(0); }

/* a named semaphore/mutex opened by another part of the program: found by its name among the created ones */
static void open_named(Cpu *c, int kind) {
	const char *name = A0 ? (const char *)G(A0) : "";
	Obj *found = 0;
	pthread_mutex_lock(&klock);
	for (int i = nobjs - 1; i >= 0 && !found; i--) { Obj *o = obj_at((uint32_t)i); if (o && o->kind == kind && !o->deleted && o->name[0] && !strcmp(o->name, name)) found = o; }
	pthread_mutex_unlock(&klock);
	RET(found ? found->uid : E_UID_CANNOT_FIND_BY_NAME);
}
static void h_open_sema(Cpu *c) { open_named(c, SEMA); }
static void h_open_mutex(Cpu *c) { open_named(c, MUTEX); }

/* sceKernelGetTLSAddr(key): a word of per-thread storage per key (0..255; the C library keeps
 * errno and its per-thread state there) */
/* __tls_get_addr(const TlsIndex *ti): the address, in the calling thread, of a module's thread-local
 * variable -- the thread's TLS block (TPIDRURO points at it; tls_block() lays the executable's TLS
 * template at its start) plus the variable's offset. ti->module is 0xffffffff for the executable,
 * the only module whose TLS the games address this way (Killzone keeps its per-thread Thread
 * object there; as a silent stub returning 0 its thread trampoline stored the object at guest
 * address 0 and every wait read [0] + 0xc). */
static void h_tls_get_addr(Cpu *c) {
	uint32_t module = RD32(A0), off = RD32(A0 + 4);
	static int warned;
	if (module != 0xffffffffu && !warned++) fprintf(stderr, "    __tls_get_addr: module %#x offset %#x (taken as the executable's block)\n", module, off);
	static int trace = -1; if (trace < 0) trace = getenv("RECOMP_LOG_TLS") != 0;
	if (trace) fprintf(stderr, "    tls t%#x +%#x holds %#x (lr %#x)\n", cur_tid, off, RD32(recomp_tpidruro + off), c->r[14]);
	RET(recomp_tpidruro + off);
}
static void h_tls_addr(Cpu *c) {   /* the guest thread's, not the host thread's: a fiber shares its thread's keys */
	Thread *t = SELF();
	if (!t->tls_keys) { t->tls_keys = guest_alloc(256 * 4, 16); memset(G(t->tls_keys), 0, 256 * 4); }
	RET(A0 < 256 ? t->tls_keys + 4 * A0 : 0);
}
static void h_system_time_wide(Cpu *c) { uint64_t t = guest_time_us(); c->r[0] = (uint32_t)t; c->r[1] = (uint32_t)(t >> 32); }

/* the program's process parameters (mkimage.py's PPRM): older C libraries read their heap size
 * there; the standard streams are the console's TTYs, i.e. the host's */
uint32_t process_param;
static void h_process_param(Cpu *c) { RET(process_param); }
/* the SDK version the program was built with (SceLibKernel 9F793F84: libkernel's own copy of the
 * process parameters' +0xc, read at its start; 0 without valid ones) -- libgxm picks behaviour by it */
static void h_sdk_version(Cpu *c) { RET(process_param && RD32(process_param + 4) == 0x32505350u && RD32(process_param + 8) ? RD32(process_param + 0xc) : 0); }
/* (pid, buffer, size): the title id (RECOMP_TITLE, which vitarecomp sets) */
static void h_title_id(Cpu *c) {
	const char *t = getenv("RECOMP_TITLE");
	if (!A1 || !A2) { RET(0x80020001u); return; }
	snprintf((char *)G(A1), A2, "%s", t ? t : "");
	RET(0);
}
static void h_is_game_budget(Cpu *c) { RET(1); }   /* a game, not a system application */
static void h_std_stream(Cpu *c, int fd) { (void)c; extern uint32_t io_guest_fd(int fd); RET(io_guest_fd(dup(fd))); }
static void h_stdin(Cpu *c) { h_std_stream(c, 0); }
static void h_stdout(Cpu *c) { h_std_stream(c, 1); }
static void h_stderr(Cpu *c) { h_std_stream(c, 2); }

static void h_random_number(Cpu *c) { if (!A0) { RET(0x80020001u); return; } plat_random(G(A0), A1); RET(0); }   /* sceKernelGetRandomNumber(buf, size): libssl seeds its RNG with it */
const HleEntry hle_kernel[] = {
	{"sceKernelGetRandomNumber", h_random_number},
	{"sceUltWaitingQueueResourcePoolGetWorkAreaSize", h_ult_pool_size}, {"_sceUltWaitingQueueResourcePoolCreate", h_zero_ok},
	{"sceUltWaitingQueueResourcePoolDestroy", h_zero_ok},
	{"_sceUltSemaphoreCreate", h_ult_sema_create}, {"sceUltSemaphoreAcquire", h_ult_sema_acquire},
	{"sceUltSemaphoreTryAcquire", h_ult_sema_try}, {"sceUltSemaphoreRelease", h_ult_sema_release}, {"sceUltSemaphoreDestroy", h_ult_sema_destroy},
	{"_sceUltMutexOptParamInitialize", h_zero_ok}, {"_sceUltMutexCreate", h_ult_mutex_create}, {"sceUltMutexLock", h_ult_mutex_lock},
	{"sceUltMutexTryLock", h_ult_mutex_trylock}, {"sceUltMutexUnlock", h_ult_mutex_unlock}, {"sceUltMutexDestroy", h_ult_mutex_destroy},
	{"_sceUltConditionVariableCreate", h_ult_cond_create}, {"sceUltConditionVariableWait", h_ult_cond_wait},
	{"sceUltConditionVariableSignal", h_ult_cond_signal}, {"sceUltConditionVariableSignalAll", h_ult_cond_signal_all},
	{"sceUltConditionVariableDestroy", h_ult_cond_destroy},
	{"sceKernelGetThreadId", h_get_thread_id}, {"sceKernelGetProcessId", h_get_process_id}, {"sceKernelCreateThread", h_create_thread},
	{"sceKernelStartThread", h_start_thread}, {"sceKernelExitThread", h_exit_thread},
	{"sceKernelExitDeleteThread", h_exit_delete_thread},
	{"SceThreadmgr_0C8A38E1", h_exit_thread},   /* the system call under libkernel's sceKernelExitThread (the same NID in SceThreadmgrForDriver) */
	{"sceKernelWaitThreadEnd", h_wait_thread_end}, {"sceKernelWaitThreadEndCB", h_wait_thread_end_cb},
	{"sceKernelGetThreadExitStatus", h_thread_exit_status}, {"sceKernelDeleteThread", h_delete_thread},
	{"sceKernelDelayThread", h_delay}, {"sceKernelDelayThreadCB", h_delay_cb},
	{"sceKernelGetThreadInfo", h_thread_info}, {"sceKernelGetThreadCurrentPriority", h_thread_current_priority},
	{"sceKernelChangeThreadPriority", h_change_priority}, {"sceKernelGetThreadCpuAffinityMask", h_thread_affinity},
	{"sceKernelChangeThreadCpuAffinityMask", h_change_affinity}, {"sceKernelChangeThreadVfpException", h_zero_ok},
	{"sceKernelRegisterThreadEventHandler", h_register_thread_event}, {"sceKernelUnregisterThreadEventHandler", h_unregister_thread_event},
	{"sceKernelExitProcess", h_exit_process}, {"sceKernelCallModuleExit", h_exit_process}, {"sceKernelCallAbortHandler", h_abort_handler},
	{"sceKernelGetTLSAddr", h_tls_addr}, {"__tls_get_addr", h_tls_get_addr}, {"sceKernelGetProcessParam", h_process_param}, {"sceKernelGetSystemTimeWide", h_system_time_wide},
	{"SceLibKernel_9F793F84", h_sdk_version}, {"sceKernelGetProcessTitleId", h_title_id}, {"sceKernelIsGameBudget", h_is_game_budget},
	{"sceKernelGetStdin", h_stdin}, {"sceKernelGetStdout", h_stdout}, {"sceKernelGetStderr", h_stderr},
	{"sceKernelCreateSema", h_create_sema}, {"sceKernelCreateSema_16XX", h_create_sema},
	{"sceKernelWaitSema", h_wait_sema}, {"sceKernelWaitSemaCB", h_wait_sema_cb}, {"sceKernelPollSema", h_poll_sema},
	{"sceKernelSignalSema", h_signal_sema}, {"sceKernelDeleteSema", h_delete_sema}, {"sceKernelGetSemaInfo", h_sema_info},
	{"sceKernelOpenSema", h_open_sema}, {"sceKernelCloseSema", h_zero_ok}, {"sceKernelOpenMutex", h_open_mutex}, {"sceKernelCloseMutex", h_zero_ok},
	{"sceKernelCreateEventFlag", h_create_evf}, {"sceKernelWaitEventFlag", h_wait_evf}, {"sceKernelWaitEventFlagCB", h_wait_evf_cb},
	{"sceKernelPollEventFlag", h_poll_evf}, {"sceKernelSetEventFlag", h_set_evf}, {"sceKernelClearEventFlag", h_clear_evf},
	{"sceKernelDeleteEventFlag", h_delete_evf}, {"sceKernelGetEventFlagInfo", h_evf_info},
	{"sceKernelCreateSimpleEvent", h_create_sevent}, {"sceKernelDeleteSimpleEvent", h_delete_sevent}, {"sceKernelWaitEvent", h_wait_sevent}, {"sceKernelWaitEventCB", h_wait_sevent_cb},
	{"sceKernelPollEvent", h_poll_sevent}, {"sceKernelSetEvent", h_set_sevent}, {"sceKernelClearEvent", h_clear_sevent},
	{"sceKernelCreateMutex", h_create_mutex}, {"sceKernelLockMutex", h_lock_mutex}, {"sceKernelLockMutexCB", h_lock_mutex_cb},
	{"sceKernelTryLockMutex", h_trylock_mutex}, {"sceKernelUnlockMutex", h_unlock_mutex}, {"sceKernelDeleteMutex", h_delete_mutex}, {"sceKernelGetMutexInfo", h_mutex_info},
	{"sceKernelCreateCond", h_create_cond}, {"sceKernelWaitCond", h_wait_cond}, {"sceKernelWaitCondCB", h_wait_cond_cb}, {"sceKernelSignalCond", h_signal_cond},
	{"sceKernelSignalCondAll", h_signal_cond_all}, {"sceKernelSignalCondTo", h_signal_cond_to}, {"sceKernelDeleteCond", h_delete_cond}, {"sceKernelGetCondInfo", h_cond_info},
	{"sceKernelCreateLwMutex", h_create_lwmutex}, {"sceKernelLockLwMutex", h_lock_lwmutex}, {"sceKernelLockLwMutexCB", h_lock_lwmutex_cb},
	{"sceKernelTryLockLwMutex", h_trylock_lwmutex}, {"sceKernelUnlockLwMutex", h_unlock_lwmutex},
	{"sceKernelLockLwMutex2", h_lock_lwmutex}, {"sceKernelUnlockLwMutex2", h_unlock_lwmutex},
	{"sceKernelDeleteLwMutex", h_delete_lwmutex},
	{"sceKernelCreateLwCond", h_create_lwcond}, {"sceKernelWaitLwCond", h_wait_lwcond}, {"sceKernelWaitLwCondCB", h_wait_lwcond_cb},
	{"sceKernelSignalLwCond", h_signal_lwcond}, {"sceKernelSignalLwCondAll", h_signal_lwcond_all}, {"sceKernelSignalLwCondTo", h_signal_lwcond_to},
	{"sceKernelDeleteLwCond", h_delete_lwcond}, {"sceKernelGetLwMutexInfo", h_lwmutex_info},
	{"sceKernelCreateCallback", h_create_callback}, {"sceKernelDeleteCallback", h_delete_callback}, {"sceKernelNotifyCallback", h_notify_callback},
	{"sceKernelCancelCallback", h_cancel_callback}, {"sceKernelGetCallbackCount", h_callback_count}, {"sceKernelCheckCallback", h_check_callback},
	{"sceKernelGetCallbackInfo", h_callback_info},
	{"sceKernelCreateRWLock", h_create_rwlock}, {"sceKernelDeleteRWLock", h_delete_rwlock},
	{"sceKernelLockReadRWLock", h_lock_read_rwlock}, {"sceKernelLockReadRWLockCB", h_lock_read_rwlock_cb},
	{"sceKernelLockWriteRWLock", h_lock_write_rwlock}, {"sceKernelLockWriteRWLockCB", h_lock_write_rwlock_cb},
	{"sceKernelTryLockReadRWLock", h_trylock_read_rwlock}, {"sceKernelTryLockWriteRWLock", h_trylock_write_rwlock},
	{"sceKernelUnlockReadRWLock", h_unlock_read_rwlock}, {"sceKernelUnlockWriteRWLock", h_unlock_write_rwlock}, {"sceKernelGetRWLockInfo", h_rwlock_info},
	{"sceKernelCreateMsgPipe", h_create_msgpipe}, {"sceKernelDeleteMsgPipe", h_delete_msgpipe},
	{"sceKernelSendMsgPipe", h_send_msgpipe}, {"sceKernelSendMsgPipeCB", h_send_msgpipe_cb}, {"sceKernelTrySendMsgPipe", h_trysend_msgpipe},
	{"sceKernelReceiveMsgPipe", h_receive_msgpipe}, {"sceKernelReceiveMsgPipeCB", h_receive_msgpipe_cb}, {"sceKernelTryReceiveMsgPipe", h_tryreceive_msgpipe},
	{"sceKernelCancelMsgPipe", h_cancel_msgpipe}, {"sceKernelGetMsgPipeInfo", h_msgpipe_info},
	{"_sceFiberInitializeImpl", h_fiber_init}, {"_sceFiberInitializeWithInternalOptionImpl", h_fiber_init}, {"sceFiberOptParamInitialize", h_fiber_opt_param_init},
	{"sceFiberRun", h_fiber_run}, {"sceFiberSwitch", h_fiber_switch}, {"sceFiberReturnToThread", h_fiber_return}, {"sceFiberGetSelf", h_fiber_get_self},
	{"sceFiberFinalize", h_fiber_finalize}, {"sceFiberGetInfo", h_fiber_info},
	{"sceKernelBacktraceSelf", h_backtrace_self},
	{0, 0},
};
