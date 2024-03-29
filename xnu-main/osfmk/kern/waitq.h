#ifndef _WAITQ_H_
#define _WAITQ_H_
/*
 * Copyright (c) 2014-2015 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
#ifdef  KERNEL_PRIVATE

#include <mach/mach_types.h>
#include <mach/sync_policy.h>
#include <mach/kern_return.h>           /* for kern_return_t */

#include <kern/kern_types.h>            /* for wait_queue_t */
#include <kern/queue.h>
#include <kern/assert.h>

#include <sys/cdefs.h>

#ifdef XNU_KERNEL_PRIVATE
/* priority queue static asserts fail for __ARM64_ARCH_8_32__ kext builds */
#include <kern/priority_queue.h>
#endif /* XNU_KERNEL_PRIVATE */
#ifdef MACH_KERNEL_PRIVATE
#include <kern/spl.h>
#include <kern/ticket_lock.h>

#include <machine/cpu_number.h>
#include <machine/machine_routines.h> /* machine_timeout_suspended() */
#endif /* MACH_KERNEL_PRIVATE */

__BEGIN_DECLS

#pragma GCC visibility push(hidden)

/*
 * Constants and types used in the waitq APIs
 */
#define WAITQ_ALL_PRIORITIES   (-1)
#define WAITQ_PROMOTE_PRIORITY (-2)
#define WAITQ_PROMOTE_ON_WAKE  (-3)

typedef enum e_waitq_lock_state {
	WAITQ_KEEP_LOCKED    = 0x01,
	WAITQ_UNLOCK         = 0x02,
	WAITQ_SHOULD_LOCK    = 0x04,
	WAITQ_ALREADY_LOCKED = 0x08,
	WAITQ_DONT_LOCK      = 0x10,
} waitq_lock_state_t;

/* Opaque sizes and alignment used for struct verification */
#if __arm__ || __arm64__
	#define WQ_OPAQUE_ALIGN   __BIGGEST_ALIGNMENT__
	#define WQS_OPAQUE_ALIGN  __BIGGEST_ALIGNMENT__
	#if __arm__
		#define WQ_OPAQUE_SIZE   32
		#define WQS_OPAQUE_SIZE  48
	#else
		#define WQ_OPAQUE_SIZE   40
		#define WQS_OPAQUE_SIZE  56
	#endif
#elif __x86_64__
	#define WQ_OPAQUE_ALIGN   8
	#define WQS_OPAQUE_ALIGN  8
	#define WQ_OPAQUE_SIZE   48
	#define WQS_OPAQUE_SIZE  64
#else
	#error Unknown size requirement
#endif

typedef struct {
	uint64_t wqr_value __kernel_ptr_semantics;
} waitq_ref_t;

#define WAITQ_REF_NULL ((waitq_ref_t){ 0 })

#ifdef MACH_KERNEL_PRIVATE

enum waitq_type {
	WQT_INVALID = 0,
	WQT_TSPROXY = 0x1,
	WQT_QUEUE   = 0x2,
	WQT_SET     = 0x3,
};

__options_decl(waitq_options_t, uint32_t, {
	WQ_OPTION_NONE                 = 0,
	WQ_OPTION_HANDOFF              = 1,
});

#if CONFIG_WAITQ_STATS
#define NWAITQ_BTFRAMES 5
struct wq_stats {
	uint64_t waits;
	uint64_t wakeups;
	uint64_t clears;
	uint64_t failed_wakeups;

	uintptr_t last_wait[NWAITQ_BTFRAMES];
	uintptr_t last_wakeup[NWAITQ_BTFRAMES];
	uintptr_t last_failed_wakeup[NWAITQ_BTFRAMES];
};
#endif

/*
 * The waitq needs WAITQ_FLAGS_BITS, which leaves 25 or 57 bits
 * for the eventmask.
 */
#define WAITQ_FLAGS_BITS   6
#define _EVENT_MASK_BITS   (8 * sizeof(waitq_flags_t) - WAITQ_FLAGS_BITS)

#if __arm64__
typedef uint32_t       waitq_flags_t;
#else
typedef unsigned long  waitq_flags_t;
#endif

/* Make sure the port abuse of bits doesn't overflow the evntmask size */
#define WAITQ_FLAGS_OVERFLOWS(...) \
	(sizeof(struct { waitq_flags_t bits : WAITQ_FLAGS_BITS, __VA_ARGS__; }) \
	> sizeof(waitq_flags_t))

#define WAITQ_FLAGS(prefix, ...) \
	struct {                                                               \
	    waitq_flags_t /* flags */                                          \
	        prefix##_type:2,      /* only public field */                  \
	        prefix##_fifo:1,      /* fifo wakeup policy? */                \
	        prefix##_irq:1,       /* waitq requires interrupts disabled */ \
	        prefix##_turnstile:1, /* waitq is embedded in a turnstile */   \
	        prefix##_portset:1    /* waitq is embedded in port-set    */   \
	            - 2 * WAITQ_FLAGS_OVERFLOWS(__VA_ARGS__),                  \
	        __VA_ARGS__;                                                   \
	}

/*
 *	struct waitq
 *
 *	This is the definition of the common event wait queue
 *	that the scheduler APIs understand.  It is used
 *	internally by the gerneralized event waiting mechanism
 *	(assert_wait), and also for items that maintain their
 *	own wait queues (such as ports and semaphores).
 *
 *	It is not published to other kernel components.
 *
 *	NOTE:  Hardware locks are used to protect event wait
 *	queues since interrupt code is free to post events to
 *	them.
 */
struct waitq {
	/* waitq_eventmask: the wait queue set (set-of-sets) to which this queue belongs */
	WAITQ_FLAGS(waitq, waitq_eventmask:_EVENT_MASK_BITS);
	hw_lck_ticket_t   waitq_interlock;
	uint8_t           waitq_padding[sizeof(waitq_flags_t) - sizeof(hw_lck_ticket_t)];
	waitq_ref_t       waitq_set_id;
	uint64_t          waitq_prepost_id;
	union {
		/* queue of elements - used for waitq not embedded in turnstile or ports */
		queue_head_t            waitq_queue;

		/* priority ordered queue of elements - used for waitqs embedded in turnstiles */
		struct priority_queue_sched_max waitq_prio_queue;

		/*
		 * used for waitqs embedded in ports
		 *
		 * waitq_ts:       used to store receive turnstile of the port
		 *
		 * waitq_tspriv:   non special-reply port, used to store the
		 *                 watchport element for port used to store
		 *                 receive turnstile of the port
		 *
		 * waitq_priv_pid: special-reply port, used to store the pid
		 *                 that copies out the send once right of the
		 *                 special-reply port.
		 */
		struct {
			struct turnstile   *waitq_ts;
			union {
				void       *waitq_tspriv;
				int         waitq_priv_pid;
			};
		};
	};
};

static_assert(sizeof(struct waitq) == WQ_OPAQUE_SIZE, "waitq structure size mismatch");
static_assert(__alignof(struct waitq) == WQ_OPAQUE_ALIGN, "waitq structure alignment mismatch");

/*
 *	struct waitq_set
 *
 *	This is the common definition for a set wait queue.
 */
struct waitq_set {
	struct waitq wqset_q;
	uint64_t     wqset_id;
	uint64_t     wqset_prepost_id;
};

#define WQSET_PREPOSTED_ANON   ((uint64_t)(~0))
#define WQSET_NOT_LINKED       ((uint64_t)(~0))
static_assert(sizeof(struct waitq_set) == WQS_OPAQUE_SIZE, "waitq_set structure size mismatch");
static_assert(__alignof(struct waitq_set) == WQS_OPAQUE_ALIGN, "waitq_set structure alignment mismatch");

extern void waitq_bootstrap(void);

#define waitq_is_queue(wq) \
	((wq)->waitq_type == WQT_QUEUE)

#define waitq_is_turnstile_proxy(wq) \
	((wq)->waitq_type == WQT_TSPROXY)

#define waitq_is_turnstile_queue(wq) \
	(((wq)->waitq_irq) && (wq)->waitq_turnstile)

#define waitq_is_set(wq) \
	((wq)->waitq_type == WQT_SET && ((struct waitq_set *)(wq))->wqset_id != 0)

#define waitqs_is_set(wqs) \
	(((wqs)->wqset_q.waitq_type == WQT_SET) && ((wqs)->wqset_id != 0))

#define waitq_valid(wq) \
	((wq) != NULL && (wq)->waitq_interlock.lck_valid)

#define waitqs_is_linked(wqs) \
	(((wqs)->wqset_id != WQSET_NOT_LINKED) && ((wqs)->wqset_id != 0))

/* in ipc_pset.c */
extern void ipc_pset_prepost(struct waitq_set *wqset, struct waitq *waitq);

extern lck_grp_t waitq_lck_grp;

#define waitq_held(wq) \
	hw_lck_ticket_held(&(wq)->waitq_interlock)

#define waitq_lock_try(wq)    \
	hw_lck_ticket_lock_try(&(wq)->waitq_interlock, &waitq_lck_grp)

#define waitq_wait_possible(thread) \
	((thread)->waitq == NULL)

extern bool waitq_lock_allow_invalid(struct waitq *wq) __result_use_check;

#define waitq_set_lock(wqs)             waitq_lock(&(wqs)->wqset_q)
#define waitq_set_unlock(wqs)           waitq_unlock(&(wqs)->wqset_q)
#define waitq_set_lock_try(wqs)         waitq_lock_try(&(wqs)->wqset_q)

/* assert intent to wait on a locked wait queue */
extern wait_result_t waitq_assert_wait64_locked(struct waitq *waitq,
    event64_t wait_event,
    wait_interrupt_t interruptible,
    wait_timeout_urgency_t urgency,
    uint64_t deadline,
    uint64_t leeway,
    thread_t thread);

/* pull a thread from its wait queue */
extern bool waitq_pull_thread_locked(struct waitq *waitq, thread_t thread);

/* wakeup all threads waiting for a particular event on locked queue */
extern kern_return_t waitq_wakeup64_all_locked(struct waitq *waitq,
    event64_t wake_event,
    wait_result_t result,
    uint64_t *reserved_preposts,
    int priority,
    waitq_lock_state_t lock_state);

/* wakeup one thread waiting for a particular event on locked queue */
extern kern_return_t waitq_wakeup64_one_locked(struct waitq *waitq,
    event64_t wake_event,
    wait_result_t result,
    uint64_t *reserved_preposts,
    int priority,
    waitq_lock_state_t lock_state,
    waitq_options_t options);

/* return identity of a thread awakened for a particular <wait_queue,event> */
extern thread_t waitq_wakeup64_identify_locked(struct waitq *waitq,
    event64_t        wake_event,
    wait_result_t    result,
    spl_t            *spl,
    uint64_t         *reserved_preposts,
    int              priority,
    waitq_lock_state_t lock_state);

/* wakeup thread iff its still waiting for a particular event on locked queue */
extern kern_return_t waitq_wakeup64_thread_locked(struct waitq *waitq,
    event64_t wake_event,
    thread_t thread,
    wait_result_t result,
    waitq_lock_state_t lock_state);

/* clear all preposts generated by the given waitq */
extern int waitq_clear_prepost_locked(struct waitq *waitq);

/* unlink the given waitq from all sets - returns unlocked */
extern void waitq_unlink_all_unlock(struct waitq *waitq);

/* unlink the given waitq set from all waitqs and waitq sets - returns unlocked */
extern kern_return_t waitq_set_unlink_all_unlock(struct waitq_set *wqset);

/* unlink the given waitq from all sets and add it to give set - returns unlocked */
extern void waitq_unlink_all_relink_unlock(
	struct waitq *waitq,
	struct waitq_set *wqset);

/*
 * clear a thread's boosted priority
 * (given via WAITQ_PROMOTE_PRIORITY in the wakeup function)
 */
extern void waitq_clear_promotion_locked(struct waitq *waitq,
    thread_t thread);

/*
 * waitq iteration
 */

enum waitq_iteration_constant {
	WQ_ITERATE_DROPPED             = -4,
	WQ_ITERATE_INVALID             = -3,
	WQ_ITERATE_ABORTED             = -2,
	WQ_ITERATE_FAILURE             = -1,
	WQ_ITERATE_SUCCESS             =  0,
	WQ_ITERATE_CONTINUE            =  1,
	WQ_ITERATE_BREAK               =  2,
	WQ_ITERATE_BREAK_KEEP_LOCKED   =  3,
	WQ_ITERATE_INVALIDATE_CONTINUE =  4,
	WQ_ITERATE_RESTART             =  5,
	WQ_ITERATE_FOUND               =  6,
	WQ_ITERATE_UNLINKED            =  7,
};

/* iterator over all waitqs that have preposted to wqset */
typedef enum waitq_iteration_constant (^ waitq_iterator_t)(struct waitq *);
extern int waitq_set_iterate_preposts(struct waitq_set *wqset, waitq_iterator_t it);

/*
 * prepost reservation
 */
extern uint64_t waitq_prepost_reserve(struct waitq *waitq, int extra,
    waitq_lock_state_t lock_state);

extern void waitq_prepost_release_reserve(uint64_t id);

#else /* !MACH_KERNEL_PRIVATE */

/*
 * The opaque waitq structure is here mostly for AIO and selinfo,
 * but could potentially be used by other BSD subsystems.
 */
struct waitq { char opaque[WQ_OPAQUE_SIZE]; } __attribute__((aligned(WQ_OPAQUE_ALIGN)));
struct waitq_set { char opaque[WQS_OPAQUE_SIZE]; } __attribute__((aligned(WQS_OPAQUE_ALIGN)));

#endif  /* MACH_KERNEL_PRIVATE */

/*
 * waitq init
 */
extern void waitq_init(struct waitq *waitq, int policy);
extern void waitq_deinit(struct waitq *waitq);

/*
 * Invalidate a waitq.
 */
extern void waitq_invalidate(struct waitq *wq);

/*
 * global waitqs
 */
extern struct waitq *_global_eventq(char *event, size_t event_length);
#define global_eventq(event) _global_eventq((char *)&(event), sizeof(event))

extern struct waitq *global_waitq(int index);

/*
 * set init/deinit
 */
extern void waitq_set_init(struct waitq_set *wqset, int policy);

extern void waitq_set_reset_anon_prepost(struct waitq_set *wqset);

extern void waitq_set_deinit(struct waitq_set *wqset);

extern void waitq_set_deinit_and_unlock(struct waitq_set *wqset);

#if DEVELOPMENT || DEBUG
extern int sysctl_helper_waitq_set_nelem(void);
#endif /* DEVELOPMENT || DEBUG */

/*
 * set membership
 */
extern waitq_ref_t waitq_link_reserve(void);
extern void waitq_set_lazy_init_link(struct waitq_set *wqset);

extern void waitq_link_release(waitq_ref_t ref);

extern bool waitq_member_locked(struct waitq *waitq, struct waitq_set *wqset);

/* on success, consumes an reserved_link reference */
extern kern_return_t waitq_link(struct waitq *waitq,
    struct waitq_set *wqset,
    waitq_lock_state_t lock_state,
    waitq_ref_t *reserved_link);

extern kern_return_t waitq_unlink(struct waitq *waitq, struct waitq_set *wqset);

extern kern_return_t waitq_unlink_locked(struct waitq *waitq, struct waitq_set *wqset);

/*
 * interfaces used primarily by the select/kqueue subsystems
 */
extern uint64_t waitq_get_prepost_id(struct waitq *waitq);
extern void     waitq_unlink_by_prepost_id(uint64_t wqp_id, struct waitq_set *wqset);
extern struct waitq *waitq_lock_by_prepost_id(uint64_t wqp_id);

/*
 * waitq attributes
 */
extern bool waitq_is_valid(struct waitq *waitq);

extern bool waitq_set_is_valid(struct waitq_set *wqset);

extern bool waitq_is_global(struct waitq *waitq);

extern bool waitq_irq_safe(struct waitq *waitq);

#if CONFIG_WAITQ_STATS
/*
 * waitq statistics
 */
#define WAITQ_STATS_VERSION 1
struct wq_table_stats {
	uint32_t version;
	uint32_t table_elements;
	uint32_t table_used_elems;
	uint32_t table_elem_sz;
	uint32_t table_slabs;
	uint32_t table_slab_sz;

	uint64_t table_num_allocs;
	uint64_t table_num_preposts;
	uint64_t table_num_reservations;

	uint64_t table_max_used;
	uint64_t table_avg_used;
	uint64_t table_max_reservations;
	uint64_t table_avg_reservations;
};

extern void waitq_link_stats(struct wq_table_stats *stats);
extern void waitq_prepost_stats(struct wq_table_stats *stats);
#endif /* CONFIG_WAITQ_STATS */

/*
 *
 * higher-level waiting APIs
 *
 */

/* assert intent to wait on <waitq,event64> pair */
extern wait_result_t waitq_assert_wait64(struct waitq *waitq,
    event64_t wait_event,
    wait_interrupt_t interruptible,
    uint64_t deadline);

extern wait_result_t waitq_assert_wait64_leeway(struct waitq *waitq,
    event64_t wait_event,
    wait_interrupt_t interruptible,
    wait_timeout_urgency_t urgency,
    uint64_t deadline,
    uint64_t leeway);

/* wakeup the most appropriate thread waiting on <waitq,event64> pair */
extern kern_return_t waitq_wakeup64_one(struct waitq *waitq,
    event64_t wake_event,
    wait_result_t result,
    int priority);

/* wakeup all the threads waiting on <waitq,event64> pair */
extern kern_return_t waitq_wakeup64_all(struct waitq *waitq,
    event64_t wake_event,
    wait_result_t result,
    int priority);

#ifdef  XNU_KERNEL_PRIVATE

/* wakeup a specified thread iff it's waiting on <waitq,event64> pair */
extern kern_return_t waitq_wakeup64_thread(struct waitq *waitq,
    event64_t wake_event,
    thread_t thread,
    wait_result_t result);

/* return a reference to the thread that was woken up */
extern thread_t waitq_wakeup64_identify(struct waitq *waitq,
    event64_t       wake_event,
    wait_result_t   result,
    int             priority);

extern void waitq_lock(struct waitq *wq);

extern void waitq_unlock(struct waitq *wq);

#endif /* XNU_KERNEL_PRIVATE */

#pragma GCC visibility pop

__END_DECLS

#endif  /* KERNEL_PRIVATE */
#endif  /* _WAITQ_H_ */
