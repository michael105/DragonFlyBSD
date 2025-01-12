/*
 * Copyright (C) 2005 Daniel M. Eischen <deischen@freebsd.org>
 * Copyright (c) 2005 David Xu <davidxu@freebsd.org>
 * Copyright (c) 1995-1998 John Birrell <jb@cimlogic.com.au>.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: head/lib/libthr/thread/thr_private.h 217706 2010-08-23 $
 */

/*
 * Private thread definitions for the uthread kernel.
 */

#ifndef _THR_PRIVATE_H
#define _THR_PRIVATE_H

/*
 * Include files.
 */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/cdefs.h>
#include <sys/queue.h>
#include <sys/rtprio.h>
#include <sys/mman.h>
#include <machine/atomic.h>
#include <machine/cpumask.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/sched.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <pthread_np.h>

#if defined(_PTHREADS_DEBUGGING) || defined(_PTHREADS_DEBUGGING2)
void	_thr_log(const char *buf, size_t bytes);
#endif

#include "pthread_md.h"
#include "thr_umtx.h"
#include "thread_db.h"

/* Signal to do cancellation */
#define	SIGCANCEL		32

/*
 * Kernel fatal error handler macro.
 */
#define PANIC(args...)		_thread_exitf(__FILE__, __LINE__, ##args)

/* Output debug messages like this: */
#define stdout_debug(args...)	_thread_printf(STDOUT_FILENO, ##args)
#define stderr_debug(args...)	_thread_printf(STDERR_FILENO, ##args)

#ifdef _PTHREADS_INVARIANTS
#define THR_ASSERT(cond, msg) do {	\
	if (__predict_false(!(cond)))	\
		PANIC(msg);		\
} while (0)
#else
#define THR_ASSERT(cond, msg)
#endif

#ifdef PIC
#define STATIC_LIB_REQUIRE(name)
#else
#define STATIC_LIB_REQUIRE(name)	__asm(".globl " #name)
#endif

TAILQ_HEAD(thread_head, pthread)	thread_head;
TAILQ_HEAD(atfork_head, pthread_atfork)	atfork_head;

#define	TIMESPEC_ADD(dst, src, val)				\
	do {							\
		(dst)->tv_sec = (src)->tv_sec + (val)->tv_sec;	\
		(dst)->tv_nsec = (src)->tv_nsec + (val)->tv_nsec; \
		if ((dst)->tv_nsec >= 1000000000) {		\
			(dst)->tv_sec++;			\
			(dst)->tv_nsec -= 1000000000;		\
		}						\
	} while (0)

#define	TIMESPEC_SUB(dst, src, val)				\
	do {							\
		(dst)->tv_sec = (src)->tv_sec - (val)->tv_sec;	\
		(dst)->tv_nsec = (src)->tv_nsec - (val)->tv_nsec; \
		if ((dst)->tv_nsec < 0) {			\
			(dst)->tv_sec--;			\
			(dst)->tv_nsec += 1000000000;		\
		}						\
	} while (0)

struct pthread_mutex {
	/*
	 * Lock for accesses to this structure.
	 */
	volatile umtx_t			m_lock;
#ifdef _PTHREADS_DEBUGGING2
	int				m_lastop[32];
#endif
	enum pthread_mutextype		m_type;
	int				m_protocol;
	TAILQ_HEAD(mutex_head, pthread)	m_queue;
	struct pthread			*m_owner;
	long				m_flags;
	int				m_count;
	int				m_refcount;

	/*
	 * Used for priority inheritance and protection.
	 *
	 *   m_prio       - For priority inheritance, the highest active
	 *		    priority (threads locking the mutex inherit
	 *		    this priority).  For priority protection, the
	 *		    ceiling priority of this mutex.
	 *   m_saved_prio - mutex owners inherited priority before
	 *		    taking the mutex, restored when the owner
	 *		    unlocks the mutex.
	 */
	int				m_prio;
	int				m_saved_prio;

	/*
	 * Link for list of all mutexes a thread currently owns.
	 */
	TAILQ_ENTRY(pthread_mutex)	m_qe;
};

#define TAILQ_INITIALIZER	{ NULL, NULL }

#define PTHREAD_MUTEX_STATIC_INITIALIZER   		\
	{	.m_lock = 0,				\
		.m_type = PTHREAD_MUTEX_DEFAULT,	\
		.m_protocol = PTHREAD_PRIO_NONE,	\
		.m_queue = TAILQ_INITIALIZER,		\
		.m_flags = MUTEX_FLAGS_PRIVATE		\
	}
/*
 * Flags for mutexes.
 */
#define MUTEX_FLAGS_PRIVATE	0x01
#define MUTEX_FLAGS_INITED	0x02

struct pthread_mutex_attr {
	enum pthread_mutextype	m_type;
	int			m_protocol;
	int			m_ceiling;
	int			m_flags;
};

#define PTHREAD_MUTEXATTR_STATIC_INITIALIZER \
	{ PTHREAD_MUTEX_DEFAULT, PTHREAD_PRIO_NONE, 0, MUTEX_FLAGS_PRIVATE }

struct cond_cancel_info;

struct pthread_cond {
	/*
	 * Lock for accesses to this structure.
	 */
	volatile umtx_t	c_lock;
	volatile int	c_unused01;
	int		c_pshared;
	int		c_clockid;
	TAILQ_HEAD(, cond_cancel_info)	c_waitlist;
};

struct pthread_cond_attr {
	int		c_pshared;
	int		c_clockid;
};

/*
 * Flags for condition variables.
 */
#define COND_FLAGS_PRIVATE	0x01
#define COND_FLAGS_INITED	0x02

struct pthread_barrier {
	volatile umtx_t	b_lock;
	volatile umtx_t	b_cycle;
	volatile int	b_count;
	volatile int	b_waiters;
};

struct pthread_barrierattr {
	int		pshared;
};

struct pthread_spinlock {
	volatile umtx_t	s_lock;
};

/*
 * Cleanup definitions.
 */
struct pthread_cleanup {
	struct pthread_cleanup	*next;
	void			(*routine)(void *);
	void			*routine_arg;
	int			onstack;
};

#define	THR_CLEANUP_PUSH(td, func, arg) {		\
	struct pthread_cleanup __cup;			\
							\
	__cup.routine = func;				\
	__cup.routine_arg = arg;			\
	__cup.onstack = 1;				\
	__cup.next = (td)->cleanup;			\
	(td)->cleanup = &__cup;

#define	THR_CLEANUP_POP(td, exec)			\
	(td)->cleanup = __cup.next;			\
	if ((exec) != 0)				\
		__cup.routine(__cup.routine_arg);	\
}

struct pthread_atfork {
	TAILQ_ENTRY(pthread_atfork) qe;
	void (*prepare)(void);
	void (*parent)(void);
	void (*child)(void);
};

struct pthread_attr {
	int	sched_policy;
	int	sched_inherit;
	int	prio;
	int	suspend;
#define	THR_STACK_USER		0x100	/* 0xFF reserved for <pthread.h> */
#define THR_CPUMASK		0x200	/* cpumask is valid */
	int	flags;
	void	*stackaddr_attr;
	size_t	stacksize_attr;
	size_t	guardsize_attr;
	cpumask_t cpumask;
};

/*
 * Thread creation state attributes.
 */
#define THR_CREATE_RUNNING		0
#define THR_CREATE_SUSPENDED		1

/*
 * Miscellaneous definitions.
 */
#define THR_STACK_DEFAULT		(sizeof(void *) / 4 * 1024 * 1024)

/*
 * Maximum size of initial thread's stack.  This perhaps deserves to be larger
 * than the stacks of other threads, since many applications are likely to run
 * almost entirely on this stack.
 */
#define THR_STACK_INITIAL		(THR_STACK_DEFAULT * 2)

/*
 * Define the different priority ranges.  All applications have thread
 * priorities constrained within 0-31.  The threads library raises the
 * priority when delivering signals in order to ensure that signal
 * delivery happens (from the POSIX spec) "as soon as possible".
 * In the future, the threads library will also be able to map specific
 * threads into real-time (cooperating) processes or kernel threads.
 * The RT and SIGNAL priorities will be used internally and added to
 * thread base priorities so that the scheduling queue can handle both
 * normal and RT priority threads with and without signal handling.
 *
 * The approach taken is that, within each class, signal delivery
 * always has priority over thread execution.
 */
#define THR_DEFAULT_PRIORITY		0
#define THR_MUTEX_CEIL_PRIORITY		31	/* dummy */

/*
 * Time slice period in microseconds.
 */
#define TIMESLICE_USEC				20000

struct pthread_rwlockattr {
	int		pshared;
};

struct pthread_rwlock {
	pthread_mutex_t	lock;	/* monitor lock */
	pthread_cond_t	read_signal;
	pthread_cond_t	write_signal;
	int		state;	/* 0 = idle  >0 = # of readers  -1 = writer */
	int		blocked_writers;
};

/*
 * Thread states.
 */
enum pthread_state {
	PS_RUNNING,
	PS_DEAD
};

struct pthread_specific_elem {
	const void	*data;
	int		seqno;
};

struct pthread_key {
	volatile int	allocated;
	volatile int	count;
	int		seqno;
	void		(*destructor)(void *);
};

/*
 * Thread structure.
 */
struct pthread {
	/*
	 * Magic value to help recognize a valid thread structure
	 * from an invalid one:
	 */
#define	THR_MAGIC		((u_int32_t) 0xd09ba115)
	u_int32_t		magic;
	char			*name;
	u_int64_t		uniqueid; /* for gdb */

	/*
	 * Lock for accesses to this thread structure.
	 */
	umtx_t			lock;

	/* Thread is terminated in kernel, written by kernel. */
	long			terminated;

	/* Kernel thread id. */
	lwpid_t			tid;

	/* Internal condition variable cycle number. */
	umtx_t			cycle;

	/* How many low level locks the thread held. */
	int			locklevel;

	/*
	 * Set to non-zero when this thread has entered a critical
	 * region.  We allow for recursive entries into critical regions.
	 */
	int			critical_count;

	/* Signal blocked counter. */
	int			sigblock;

	/* Queue entry for list of all threads. */
	TAILQ_ENTRY(pthread)	tle;	/* link for all threads in process */

	/* Queue entry for GC lists. */
	TAILQ_ENTRY(pthread)	gcle;

	/* Hash queue entry. */
	LIST_ENTRY(pthread)	hle;

	/* Threads reference count. */
	int			refcount;

	/*
	 * Thread start routine, argument, stack pointer and thread
	 * attributes.
	 */
	void			*(*start_routine)(void *);
	void			*arg;
	struct pthread_attr	attr;

	/*
	 * Cancelability flags
	 */
#define	THR_CANCEL_DISABLE		0x0001
#define	THR_CANCEL_EXITING		0x0002
#define THR_CANCEL_AT_POINT		0x0004
#define THR_CANCEL_NEEDED		0x0008
#define	SHOULD_CANCEL(val)					\
	(((val) & (THR_CANCEL_DISABLE | THR_CANCEL_EXITING |	\
		 THR_CANCEL_NEEDED)) == THR_CANCEL_NEEDED)

#define	SHOULD_ASYNC_CANCEL(val)				\
	(((val) & (THR_CANCEL_DISABLE | THR_CANCEL_EXITING |	\
		 THR_CANCEL_NEEDED | THR_CANCEL_AT_POINT)) ==	\
		 (THR_CANCEL_NEEDED | THR_CANCEL_AT_POINT))
	int			cancelflags;

	/* Thread temporary signal mask. */
	sigset_t		sigmask;

	/* Thread state: */
	umtx_t			state;

	/*
	 * Error variable used instead of errno, used for internal.
	 */
	int			error;

	/*
	 * The joiner is the thread that is joining to this thread.  The
	 * join status keeps track of a join operation to another thread.
	 */
	struct pthread		*joiner;

	/*
	 * The current thread can belong to a priority mutex queue.
	 * This is the synchronization queue link.
	 */
	TAILQ_ENTRY(pthread)	sqe;

	/* Miscellaneous flags; only set with scheduling lock held. */
	int			flags;
#define THR_FLAGS_PRIVATE	0x0001
#define	THR_FLAGS_NEED_SUSPEND	0x0002	/* thread should be suspended */
#define	THR_FLAGS_SUSPENDED	0x0004	/* thread is suspended */

	/* Thread list flags; only set with thread list lock held. */
	int			tlflags;
#define	TLFLAGS_GC_SAFE		0x0001	/* thread safe for cleaning */
#define	TLFLAGS_IN_TDLIST	0x0002	/* thread in all thread list */
#define	TLFLAGS_IN_GCLIST	0x0004	/* thread in gc list */
#define	TLFLAGS_DETACHED	0x0008	/* thread is detached */

	/*
	 * Base priority is the user setable and retrievable priority
	 * of the thread.  It is only affected by explicit calls to
	 * set thread priority and upon thread creation via a thread
	 * attribute or default priority.
	 */
	char			base_priority;

	/*
	 * Inherited priority is the priority a thread inherits by
	 * taking a priority inheritance or protection mutex.  It
	 * is not affected by base priority changes.  Inherited
	 * priority defaults to and remains 0 until a mutex is taken
	 * that is being waited on by any other thread whose priority
	 * is non-zero.
	 */
	char			inherited_priority;

	/*
	 * Active priority is always the maximum of the threads base
	 * priority and inherited priority.  When there is a change
	 * in either the base or inherited priority, the active
	 * priority must be recalculated.
	 */
	char			active_priority;

	/* Number of priority ceiling or protection mutexes owned. */
	int			priority_mutex_count;

	/* Queue of currently owned simple type mutexes. */
	TAILQ_HEAD(, pthread_mutex)	mutexq;

	void				*ret;
	struct pthread_specific_elem	*specific;
	int				specific_data_count;

	/* Number rwlocks rdlocks held. */
	int			rdlock_count;

	/*
	 * Current locks bitmap for rtld. */
	int			rtld_bits;

	/* Thread control block */
	struct tls_tcb		*tcb;

	/* Cleanup handlers Link List */
	struct pthread_cleanup	*cleanup;

	/* Enable event reporting */
	int			report_events;

	/* Event mask */
	td_thr_events_t		event_mask;

	/* Event */
	td_event_msg_t		event_buf;
};

#define	THR_IN_CRITICAL(thrd)				\
	(((thrd)->locklevel > 0) ||			\
	((thrd)->critical_count > 0))

#define THR_UMTX_TRYLOCK(thrd, lck)			\
	_thr_umtx_trylock((lck), (thrd)->tid)

#define	THR_UMTX_LOCK(thrd, lck)			\
	_thr_umtx_lock((lck), (thrd)->tid)

#define	THR_UMTX_TIMEDLOCK(thrd, lck, timo)		\
	_thr_umtx_timedlock((lck), (thrd)->tid, (timo))

#define	THR_UMTX_UNLOCK(thrd, lck)			\
	_thr_umtx_unlock((lck), (thrd)->tid)

#define	THR_LOCK_ACQUIRE(thrd, lck)			\
do {							\
	(thrd)->locklevel++;				\
	_thr_umtx_lock((lck), (thrd)->tid);		\
} while (0)

#ifdef	_PTHREADS_INVARIANTS
#define	THR_ASSERT_LOCKLEVEL(thrd)			\
do {							\
	if (__predict_false((thrd)->locklevel <= 0))	\
		_thr_assert_lock_level();		\
} while (0)
#else
#define THR_ASSERT_LOCKLEVEL(thrd)
#endif

#define	THR_LOCK_RELEASE(thrd, lck)			\
do {							\
	THR_ASSERT_LOCKLEVEL(thrd);			\
	_thr_umtx_unlock((lck), (thrd)->tid);		\
	(thrd)->locklevel--;				\
	_thr_ast(thrd);					\
} while (0)

#define	THR_LOCK(curthrd)		THR_LOCK_ACQUIRE(curthrd, &(curthrd)->lock)
#define	THR_UNLOCK(curthrd)		THR_LOCK_RELEASE(curthrd, &(curthrd)->lock)
#define	THR_THREAD_LOCK(curthrd, thr)	THR_LOCK_ACQUIRE(curthrd, &(thr)->lock)
#define	THR_THREAD_UNLOCK(curthrd, thr)	THR_LOCK_RELEASE(curthrd, &(thr)->lock)

#define	THREAD_LIST_LOCK(curthrd)				\
do {								\
	THR_LOCK_ACQUIRE((curthrd), &_thr_list_lock);		\
} while (0)

#define	THREAD_LIST_UNLOCK(curthrd)				\
do {								\
	THR_LOCK_RELEASE((curthrd), &_thr_list_lock);		\
} while (0)

/*
 * Macros to insert/remove threads to the all thread list and
 * the gc list.
 */
#define	THR_LIST_ADD(thrd) do {					\
	if (((thrd)->tlflags & TLFLAGS_IN_TDLIST) == 0) {	\
		TAILQ_INSERT_HEAD(&_thread_list, thrd, tle);	\
		_thr_hash_add(thrd);				\
		(thrd)->tlflags |= TLFLAGS_IN_TDLIST;		\
	}							\
} while (0)
#define	THR_LIST_REMOVE(thrd) do {				\
	if (((thrd)->tlflags & TLFLAGS_IN_TDLIST) != 0) {	\
		TAILQ_REMOVE(&_thread_list, thrd, tle);		\
		_thr_hash_remove(thrd);				\
		(thrd)->tlflags &= ~TLFLAGS_IN_TDLIST;		\
	}							\
} while (0)
#define	THR_GCLIST_ADD(thrd) do {				\
	if (((thrd)->tlflags & TLFLAGS_IN_GCLIST) == 0) {	\
		TAILQ_INSERT_HEAD(&_thread_gc_list, thrd, gcle);\
		(thrd)->tlflags |= TLFLAGS_IN_GCLIST;		\
		_thr_gc_count++;					\
	}							\
} while (0)
#define	THR_GCLIST_REMOVE(thrd) do {				\
	if (((thrd)->tlflags & TLFLAGS_IN_GCLIST) != 0) {	\
		TAILQ_REMOVE(&_thread_gc_list, thrd, gcle);	\
		(thrd)->tlflags &= ~TLFLAGS_IN_GCLIST;		\
		_thr_gc_count--;					\
	}							\
} while (0)

#define GC_NEEDED()	(_thr_gc_count >= 5)

#define	THR_IN_SYNCQ(thrd)	(((thrd)->sflags & THR_FLAGS_IN_SYNCQ) != 0)

#define SHOULD_REPORT_EVENT(curthr, e)			\
	(curthr->report_events &&			\
	 (((curthr)->event_mask | _thread_event_mask ) & e) != 0)

#ifndef __LIBC_ISTHREADED_DECLARED
#define __LIBC_ISTHREADED_DECLARED
extern int __isthreaded;
#endif

/*
 * Global variables for the pthread library.
 */
extern char		*_usrstack;
extern struct pthread	*_thr_initial;

/* For debugger */
extern int		_libthread_xu_debug;
extern int		_thread_event_mask;
extern struct pthread	*_thread_last_event;

/* List of all threads */
extern struct thread_head	_thread_list;

/* List of threads needing GC */
extern struct thread_head	_thread_gc_list;

extern int	_thread_active_threads;

extern struct	atfork_head	_thr_atfork_list;
extern struct	atfork_head	_thr_atfork_kern_list;
extern umtx_t	_thr_atfork_lock;

/* Default thread attributes */
extern struct pthread_attr _pthread_attr_default;

/* Default mutex attributes */
extern struct pthread_mutex_attr _pthread_mutexattr_default;

/* Default condition variable attributes */
extern struct pthread_cond_attr _pthread_condattr_default;

extern pid_t	_thr_pid;
extern size_t	_thr_guard_default;
extern size_t	_thr_stack_default;
extern size_t	_thr_stack_initial;
extern int	_thr_page_size;
extern int	_thr_gc_count;

extern umtx_t	_mutex_static_lock;
extern umtx_t	_cond_static_lock;
extern umtx_t	_rwlock_static_lock;
extern umtx_t	_keytable_lock;
extern umtx_t	_thr_list_lock;
extern umtx_t	_thr_event_lock;

/*
 * Function prototype definitions.
 */
__BEGIN_DECLS
int	_thr_setthreaded(int);
int	_mutex_cv_lock(pthread_mutex_t *, int count);
int	_mutex_cv_unlock(pthread_mutex_t *, int *count);
void	_mutex_notify_priochange(struct pthread *, struct pthread *, int);
void	_mutex_fork(struct pthread *curthread);
void	_mutex_unlock_private(struct pthread *);

#if 0
int	_mutex_reinit(pthread_mutex_t *);
void	_cond_reinit(pthread_cond_t pcond);
void	_rwlock_reinit(pthread_rwlock_t prwlock);
#endif

void	_libpthread_init(struct pthread *);
struct pthread *_thr_alloc(struct pthread *);
void	_thread_exit(const char *, int, const char *) __dead2;
void	_thread_exitf(const char *, int, const char *, ...) __dead2
	    __printflike(3, 4);
void	_thr_exit_cleanup(void);
void	_thr_atfork_kern(void (*prepare)(void), void (*parent)(void),
			void (*child)(void));
int	_thr_ref_add(struct pthread *, struct pthread *, int);
void	_thr_ref_delete(struct pthread *, struct pthread *);
void	_thr_ref_delete_unlocked(struct pthread *, struct pthread *);
int	_thr_find_thread(struct pthread *, struct pthread *, int);
void	_thr_malloc_init(void);
void	_rtld_setthreaded(int);
void	_thr_rtld_init(void);
void	_thr_rtld_fini(void);
int	_thr_stack_alloc(struct pthread_attr *);
void	_thr_stack_free(struct pthread_attr *);
void	_thr_stack_cleanup(void);
void	_thr_sem_init(void);
void	_thr_free(struct pthread *, struct pthread *);
void	_thr_gc(struct pthread *);
void	_thread_cleanupspecific(void);
void	_thread_dump_info(void);
void	_thread_printf(int, const char *, ...) __printflike(2, 3);
void	_thread_vprintf(int, const char *, va_list);
void	_thr_spinlock_init(void);
int	_thr_cancel_enter(struct pthread *);
void	_thr_cancel_leave(struct pthread *, int);
void	_thr_signal_block(struct pthread *);
void	_thr_signal_unblock(struct pthread *);
void	_thr_signal_init(void);
void	_thr_signal_deinit(void);
int	_thr_send_sig(struct pthread *, int sig);
void	_thr_list_init(void);
void	_thr_hash_add(struct pthread *);
void	_thr_hash_remove(struct pthread *);
struct pthread *_thr_hash_find(struct pthread *);
void	_thr_link(struct pthread *curthread, struct pthread *thread);
void	_thr_unlink(struct pthread *curthread, struct pthread *thread);
void	_thr_suspend_check(struct pthread *curthread);
void	_thr_assert_lock_level(void) __dead2;
void	_thr_ast(struct pthread *);
int	_thr_get_tid(void);
void	_thr_report_creation(struct pthread *curthread,
			   struct pthread *newthread);
void	_thr_report_death(struct pthread *curthread);
void	_thread_bp_create(void);
void	_thread_bp_death(void);
int	_thr_getscheduler(lwpid_t, int *, struct sched_param *);
int	_thr_setscheduler(lwpid_t, int, const struct sched_param *);
int	_thr_set_sched_other_prio(struct pthread *, int);
int	_rtp_to_schedparam(const struct rtprio *rtp, int *policy,
	    struct sched_param *param);
int	_schedparam_to_rtp(int policy, const struct sched_param *param,
	    struct rtprio *rtp);
int	_umtx_sleep_err(volatile const int *, int, int);
int	_umtx_wakeup_err(volatile const int *, int);

/* #include <fcntl.h> */
#ifdef  _SYS_FCNTL_H_
int     __sys_fcntl(int, int, ...);
int     __sys_open(const char *, int, ...);
int     __sys_openat(int, const char *, int, ...);
#endif

/* #include <sys/ioctl.h> */
#ifdef _SYS_IOCTL_H_
int	__sys_ioctl(int, unsigned long, ...);
#endif

/* #include <sched.h> */
#ifdef	_SCHED_H_
int	__sys_sched_yield(void);
#endif

/* #include <signal.h> */
#ifdef _SIGNAL_H_
int	__sys_kill(pid_t, int);
int     __sys_sigaction(int, const struct sigaction *, struct sigaction *);
int     __sys_sigpending(sigset_t *);
int     __sys_sigprocmask(int, const sigset_t *, sigset_t *);
int     __sys_sigsuspend(const sigset_t *);
int     __sys_sigreturn(ucontext_t *);
int     __sys_sigaltstack(const stack_t *, stack_t *);
#endif

/* #include <time.h> */
#ifdef	_TIME_H_
int	__sys_nanosleep(const struct timespec *, struct timespec *);
#endif

/* #include <unistd.h> */
#ifdef  _UNISTD_H_
int	__sys_close(int);
int	__sys_execve(const char *, char * const *, char * const *);
pid_t	__sys_getpid(void);
ssize_t __sys_read(int, void *, size_t);
ssize_t __sys_write(int, const void *, size_t);
void	__sys_exit(int);
int	__sys_sigwait(const sigset_t *, int *);
int	__sys_sigtimedwait(const sigset_t *, siginfo_t *,
		const struct timespec *);
int	__sys_sigwaitinfo(const sigset_t *set, siginfo_t *info);
#endif

static inline int
_thr_isthreaded(void)
{
	return (__isthreaded != 0);
}

static inline int
_thr_is_inited(void)
{
	return (_thr_initial != NULL);
}

static inline void
_thr_check_init(void)
{
	if (_thr_initial == NULL)
		_libpthread_init(NULL);
}

struct dl_phdr_info;
void __pthread_cxa_finalize(struct dl_phdr_info *phdr_info);

__END_DECLS

#endif  /* !_THR_PRIVATE_H */
