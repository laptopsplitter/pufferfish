/*
 * tonicsud: syscall dispatch by code address for FreeBSD (Linux's Syscall
 * User Dispatch). A `syscall` executed inside a registered range is not run;
 * the thread gets the chosen signal instead (si_code TSUD_CODE; arg 4 is in
 * the rcx slot). Every other syscall runs unchanged.
 *
 * Control call (number from modstat("sys/tonicsud")):
 *   0: version   1, sig: on   2, lo, hi: add range   3: off   4, lo, hi: remove range
 * A process's entry goes away when it exits or execs. amd64 only.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/signalvar.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#include <sys/eventhandler.h>
#include <machine/atomic.h>
#include <machine/frame.h>

#ifndef __amd64__
#error "tonicsud: amd64 only"
#endif

struct image_params;

#define TSUD_VERSION  1
#define TSUD_CODE     0x7473  /* si_code of the signal */
#define TSUD_MAXPROC  16
#define TSUD_MAXRANGE 256

struct tsud_range {
	uintptr_t lo, hi;
};
struct tsud_proc {
	struct proc *volatile p;  /* NULL: free */
	int sig;
	volatile u_int n;
	struct tsud_range r[TSUD_MAXRANGE];
};

static struct tsud_proc tsud_procs[TSUD_MAXPROC];
static volatile u_int tsud_nprocs;  /* entries in use: 0 = nothing to check */
static struct mtx tsud_mtx;         /* writers only; readers go without */

/* The system call vectors whose argument fetch we wrap (normally just one). */
struct tsud_hook {
	struct sysentvec *sv;
	int (*orig)(struct thread *);
};
#define TSUD_MAXHOOK 4
static struct tsud_hook tsud_hooks[TSUD_MAXHOOK];
static volatile u_int tsud_nhooks;

static eventhandler_tag tsud_exit_tag, tsud_exec_tag;

static struct tsud_proc *
tsud_find(struct proc *p)
{
	int i;

	for (i = 0; i < TSUD_MAXPROC; i++)
		if (tsud_procs[i].p == p)
			return (&tsud_procs[i]);
	return (NULL);
}

static int
tsud_in_range(struct tsud_proc *e, uintptr_t pc)
{
	u_int i, n;

	n = atomic_load_acq_int(&e->n);
	for (i = 0; i < n && i < TSUD_MAXRANGE; i++)
		if (pc >= e->r[i].lo && pc < e->r[i].hi)  /* (an empty slot, 0-0, never matches) */
			return (1);
	return (0);
}

/* What a dispatched call runs instead: the signal, and nothing else. */
static int
tsud_trap(struct thread *td, void *uap __unused)
{
	struct proc *p = td->td_proc;
	struct tsud_proc *e;
	ksiginfo_t ksi;
	int sig;

	e = tsud_find(p);
	sig = e != NULL ? e->sig : SIGSYS;
	/*
	 * Delivered even where the thread blocks it (a thread tonic made with
	 * everything blocked may run Roblox's code): the signal is the process's
	 * own, for this alone.
	 */
	PROC_LOCK(p);
	SIGDELSET(td->td_sigmask, sig);
	PROC_UNLOCK(p);
	ksiginfo_init_trap(&ksi);
	ksi.ksi_signo = sig;
	ksi.ksi_code = TSUD_CODE;
	ksi.ksi_addr = (void *)td->td_frame->tf_rip;
	ksi.ksi_trapno = (int)td->td_frame->tf_rax;
	trapsignal(td, &ksi);
	return (EJUSTRETURN);  /* registers untouched: rax still holds the call's number */
}

static struct sysent tsud_trap_sysent = {
	.sy_narg = 0,
	.sy_call = (sy_call_t *)tsud_trap,
	.sy_auevent = AUE_NULL,
	.sy_flags = 0,
	.sy_thrcnt = SY_THR_STATIC,
};

/* In place of the vector's sv_fetch_syscall_args, for every process using it. */
static int
tsud_fetch(struct thread *td)
{
	struct proc *p = td->td_proc;
	struct sysentvec *sv = p->p_sysent;
	int (*orig)(struct thread *) = NULL;
	struct tsud_proc *e;
	u_int i, n;
	int error;

	n = atomic_load_acq_int(&tsud_nhooks);
	for (i = 0; i < n; i++)
		if (tsud_hooks[i].sv == sv) {
			orig = tsud_hooks[i].orig;
			break;
		}
	if (orig == NULL)
		return (ENOSYS);  /* (cannot happen: only hooked vectors call this) */
	error = orig(td);
	if (__predict_true(atomic_load_acq_int(&tsud_nprocs) == 0))
		return (error);
	e = tsud_find(p);
	if (__predict_true(e == NULL))
		return (error);
	/* tf_rip is just past the 2-byte syscall instruction. */
	if (!tsud_in_range(e, (uintptr_t)td->td_frame->tf_rip - 2))
		return (error);
	/*
	 * A Linux call (whatever FreeBSD made of its number: Linux read, 0, looks
	 * like FreeBSD's indirect syscall(2) and may even have failed to fetch).
	 */
	td->td_sa.code = (u_int)td->td_frame->tf_rax;  /* (what ktrace and audit see) */
	td->td_sa.original_code = td->td_sa.code;
	td->td_sa.callp = &tsud_trap_sysent;
	return (0);
}

static int
tsud_hook_vector(struct sysentvec *sv)
{
	u_int i, n;

	mtx_assert(&tsud_mtx, MA_OWNED);
	n = tsud_nhooks;
	for (i = 0; i < n; i++)
		if (tsud_hooks[i].sv == sv)
			return (0);
	if (n >= TSUD_MAXHOOK)
		return (ENOSPC);
	tsud_hooks[n].sv = sv;
	tsud_hooks[n].orig = sv->sv_fetch_syscall_args;
	atomic_store_rel_int(&tsud_nhooks, n + 1);
	atomic_thread_fence_seq_cst();
	sv->sv_fetch_syscall_args = tsud_fetch;
	return (0);
}

static void
tsud_drop(struct proc *p)
{
	struct tsud_proc *e;

	if (atomic_load_acq_int(&tsud_nprocs) == 0)
		return;
	mtx_lock(&tsud_mtx);
	e = tsud_find(p);
	if (e != NULL) {
		e->p = NULL;
		atomic_store_rel_int(&e->n, 0);
		atomic_subtract_rel_int(&tsud_nprocs, 1);
	}
	mtx_unlock(&tsud_mtx);
}

static void
tsud_on_exit(void *arg __unused, struct proc *p)
{
	tsud_drop(p);
}

static void
tsud_on_exec(void *arg __unused, struct proc *p, struct image_params *imgp __unused)
{
	tsud_drop(p);
}

/* The control call. */
struct tsud_args {  /* (amd64: one 8-byte register each) */
	register_t cmd;
	uint64_t a;
	uint64_t b;
};

static int
tsud_syscall(struct thread *td, struct tsud_args *uap)
{
	struct proc *p = td->td_proc;
	struct tsud_proc *e;
	int error = 0, i;
	u_int n;

	switch (uap->cmd) {
	case 0:
		td->td_retval[0] = TSUD_VERSION;
		return (0);
	case 1:
		if (!_SIG_VALID((int)uap->a) || uap->a == SIGKILL || uap->a == SIGSTOP)
			return (EINVAL);
		mtx_lock(&tsud_mtx);
		error = tsud_hook_vector(p->p_sysent);
		if (error == 0) {
			e = tsud_find(p);
			if (e == NULL) {
				for (i = 0; i < TSUD_MAXPROC; i++)
					if (tsud_procs[i].p == NULL)
						break;
				if (i == TSUD_MAXPROC) {
					error = EAGAIN;
				} else {
					e = &tsud_procs[i];
					e->sig = (int)uap->a;
					atomic_store_rel_int(&e->n, 0);
					atomic_store_rel_ptr((volatile uintptr_t *)&e->p, (uintptr_t)p);
					atomic_add_rel_int(&tsud_nprocs, 1);
				}
			} else {
				e->sig = (int)uap->a;
			}
		}
		mtx_unlock(&tsud_mtx);
		return (error);
	case 2:
		if (uap->a >= uap->b)
			return (EINVAL);
		mtx_lock(&tsud_mtx);
		e = tsud_find(p);
		if (e == NULL) {
			error = ESRCH;
		} else {
			int fr = -1;

			n = e->n;
			/* Grow a range it overlaps or touches, if any (empty slots are 0-0). */
			for (i = 0; i < (int)n; i++) {
				if (e->r[i].lo == e->r[i].hi) {
					if (fr < 0)
						fr = i;
					continue;
				}
				if (uap->a <= e->r[i].hi && uap->b >= e->r[i].lo) {
					if (uap->a < e->r[i].lo)
						e->r[i].lo = uap->a;
					if (uap->b > e->r[i].hi)
						e->r[i].hi = uap->b;
					break;
				}
			}
			if (i == (int)n) {
				if (fr < 0 && n >= TSUD_MAXRANGE) {
					error = ENOSPC;
				} else if (fr >= 0) {
					e->r[fr].lo = uap->a;  /* (empty until hi is set) */
					atomic_thread_fence_rel();
					e->r[fr].hi = uap->b;
				} else {
					e->r[n].lo = uap->a;
					e->r[n].hi = uap->b;
					atomic_store_rel_int(&e->n, n + 1);
				}
			}
		}
		mtx_unlock(&tsud_mtx);
		return (error);
	case 4:
		if (uap->a >= uap->b)
			return (EINVAL);
		mtx_lock(&tsud_mtx);
		e = tsud_find(p);
		if (e != NULL) {
			n = e->n;
			for (i = 0; i < (int)n; i++) {
				struct tsud_range *r = &e->r[i];

				if (r->lo == r->hi || uap->b <= r->lo || uap->a >= r->hi)
					continue;
				if (uap->a <= r->lo && uap->b >= r->hi) {
					r->hi = 0;  /* (empty from here) */
					atomic_thread_fence_rel();
					r->lo = 0;
				} else if (uap->a <= r->lo) {
					r->lo = uap->b;
				} else if (uap->b >= r->hi) {
					r->hi = uap->a;
				}
				/* (a hole in the middle: the range stays whole) */
			}
		}
		mtx_unlock(&tsud_mtx);
		return (0);
	case 3:
		tsud_drop(p);
		return (0);
	}
	return (EINVAL);
}

static struct sysent tsud_sysent = {
	.sy_narg = 3,
	.sy_call = (sy_call_t *)tsud_syscall,
	.sy_auevent = AUE_NULL,
};
static int tsud_offset = NO_SYSCALL;

static int
tsud_modevent(struct module *m __unused, int what, void *arg __unused)
{
	u_int i;

	switch (what) {
	case MOD_LOAD:
		mtx_init(&tsud_mtx, "tonicsud", NULL, MTX_DEF);
		tsud_exit_tag = EVENTHANDLER_REGISTER(process_exit, tsud_on_exit, NULL, EVENTHANDLER_PRI_ANY);
		tsud_exec_tag = EVENTHANDLER_REGISTER(process_exec, tsud_on_exec, NULL, EVENTHANDLER_PRI_ANY);
		printf("tonicsud: loaded (system call %d)\n", tsud_offset);
		return (0);
	case MOD_UNLOAD:
		if (atomic_load_acq_int(&tsud_nprocs) != 0)
			return (EBUSY);  /* a tonic is running: quit it first */
		mtx_lock(&tsud_mtx);
		for (i = 0; i < tsud_nhooks; i++)
			tsud_hooks[i].sv->sv_fetch_syscall_args = tsud_hooks[i].orig;
		mtx_unlock(&tsud_mtx);
		/* Let calls already inside tsud_fetch leave it. */
		pause("tsudul", hz / 2);
		EVENTHANDLER_DEREGISTER(process_exit, tsud_exit_tag);
		EVENTHANDLER_DEREGISTER(process_exec, tsud_exec_tag);
		mtx_destroy(&tsud_mtx);
		return (0);
	}
	return (0);
}

SYSCALL_MODULE(tonicsud, &tsud_offset, &tsud_sysent, tsud_modevent, NULL);
