/*
 * Copyright (c) 2000-2020 Apple Inc. All rights reserved.
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
/* Copyright (c) 1995, 1997 Apple Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_fork.c	8.8 (Berkeley) 2/14/95
 */
/*
 * NOTICE: This file was modified by McAfee Research in 2004 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

#include <kern/assert.h>
#include <kern/bits.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/user.h>
#include <sys/reason.h>
#include <sys/resourcevar.h>
#include <sys/vnode_internal.h>
#include <sys/file_internal.h>
#include <sys/acct.h>
#include <sys/codesign.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>
#if CONFIG_PERSONAS
#include <sys/persona.h>
#endif
#include <sys/doc_tombstone.h>
#if CONFIG_DTRACE
/* Do not include dtrace.h, it redefines kmem_[alloc/free] */
extern void (*dtrace_proc_waitfor_exec_ptr)(proc_t);
extern void dtrace_proc_fork(proc_t, proc_t, int);

/*
 * Since dtrace_proc_waitfor_exec_ptr can be added/removed in dtrace_subr.c,
 * we will store its value before actually calling it.
 */
static void (*dtrace_proc_waitfor_hook)(proc_t) = NULL;

#include <sys/dtrace_ptss.h>
#endif

#include <security/audit/audit.h>

#include <mach/mach_types.h>
#include <kern/coalition.h>
#include <kern/kern_types.h>
#include <kern/kalloc.h>
#include <kern/mach_param.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/zalloc.h>

#include <os/log.h>

#if CONFIG_MACF
#include <security/mac_framework.h>
#include <security/mac_mach_internal.h>
#endif

#include <vm/vm_map.h>
#include <vm/vm_protos.h>
#include <vm/vm_shared_region.h>

#include <sys/shm_internal.h>   /* for shmfork() */
#include <mach/task.h>          /* for thread_create() */
#include <mach/thread_act.h>    /* for thread_resume() */

#include <sys/sdt.h>

#if CONFIG_MEMORYSTATUS
#include <sys/kern_memorystatus.h>
#endif

/* XXX routines which should have Mach prototypes, but don't */
void thread_set_parent(thread_t parent, int pid);
extern void act_thread_catt(void *ctx);
void thread_set_child(thread_t child, int pid);
void *act_thread_csave(void);
extern boolean_t task_is_exec_copy(task_t);
int nextpidversion = 0;


thread_t cloneproc(task_t, coalition_t *, proc_t, int, int);
proc_t forkproc(proc_t);
void forkproc_free(proc_t);
thread_t fork_create_child(task_t parent_task,
    coalition_t *parent_coalitions,
    proc_t child,
    int inherit_memory,
    int is_64bit_addr,
    int is_64bit_data,
    int in_exec);

static LCK_GRP_DECLARE(rethrottle_lock_grp, "rethrottle");
static ZONE_DECLARE(uthread_zone, "uthreads",
    sizeof(struct uthread), ZC_ZFREE_CLEARMEM);

os_refgrp_decl(, p_refgrp, "proc", NULL);
SECURITY_READ_ONLY_LATE(zone_t) proc_zone;
ZONE_INIT(&proc_zone, "proc", sizeof(struct proc),
    ZC_ZFREE_CLEARMEM | ZC_SEQUESTER, /* sequester is needed for proc_rele() */
    ZONE_ID_PROC, NULL);

KALLOC_TYPE_DEFINE(proc_stats_zone, struct pstats, KT_DEFAULT);

ZONE_DECLARE(proc_sigacts_zone, "sigacts", sizeof(struct sigacts), ZC_NONE);

static TUNABLE(bool, bootarg_panic_on_vfork, "-panic_on_vfork", false);

#if CONFIG_VFORK
/*
 * proc_vfork_begin
 *
 * Description:	start a vfork on a process
 *
 * Parameters:	parent_proc		process (re)entering vfork state
 *
 * Returns:	(void)
 *
 * Notes:	Although this function increments a count, a count in
 *		excess of 1 is not currently supported.  According to the
 *		POSIX standard, calling anything other than execve() or
 *		_exit() following a vfork(), including calling vfork()
 *		itself again, will result in undefined behaviour
 */
static void
proc_vfork_begin(proc_t parent_proc)
{
	proc_lock(parent_proc);
	parent_proc->p_lflag  |= P_LVFORK;
	parent_proc->p_vforkcnt++;
	proc_unlock(parent_proc);
}

/*
 * proc_vfork_end
 *
 * Description:	stop a vfork on a process
 *
 * Parameters:	parent_proc		process leaving vfork state
 *
 * Returns:	(void)
 *
 * Notes:	Decrements the count; currently, reentrancy of vfork()
 *		is unsupported on the current process
 */
static void
proc_vfork_end(proc_t parent_proc)
{
	proc_lock(parent_proc);
	parent_proc->p_vforkcnt--;
	if (parent_proc->p_vforkcnt < 0) {
		panic("vfork cnt is -ve");
	}
	if (parent_proc->p_vforkcnt == 0) {
		parent_proc->p_lflag  &= ~P_LVFORK;
	}
	proc_unlock(parent_proc);
}


/*
 * vfork
 *
 * Description:	vfork system call
 *
 * Parameters:	void			[no arguments]
 *
 * Retval:	0			(to child process)
 *		!0			pid of child (to parent process)
 *		-1			error (see "Returns:")
 *
 * Returns:	EAGAIN			Administrative limit reached
 *		EINVAL			vfork() called during vfork()
 *		ENOMEM			Failed to allocate new process
 *
 * Note:	After a successful call to this function, the parent process
 *		has its task, thread, and uthread lent to the child process,
 *		and control is returned to the caller; if this function is
 *		invoked as a system call, the return is to user space, and
 *		is effectively running on the child process.
 *
 *		Subsequent calls that operate on process state are permitted,
 *		though discouraged, and will operate on the child process; any
 *		operations on the task, thread, or uthread will result in
 *		changes in the parent state, and, if inheritable, the child
 *		state, when a task, thread, and uthread are realized for the
 *		child process at execve() time, will also be effected.  Given
 *		this, it's recemmended that people use the posix_spawn() call
 *		instead.
 *
 * BLOCK DIAGRAM OF VFORK
 *
 * Before:
 *
 *     ,----------------.         ,-------------.
 *     |                |   task  |             |
 *     | parent_thread  | ------> | parent_task |
 *     |                | <.list. |             |
 *     `----------------'         `-------------'
 *    uthread |  ^             bsd_info |  ^
 *            v  | vc_thread            v  | task
 *     ,----------------.         ,-------------.
 *     |                |         |             |
 *     | parent_uthread | <.list. | parent_proc | <-- current_proc()
 *     |                |         |             |
 *     `----------------'         `-------------'
 *    uu_proc |
 *            v
 *           NULL
 *
 * After:
 *
 *                 ,----------------.         ,-------------.
 *                 |                |   task  |             |
 *          ,----> | parent_thread  | ------> | parent_task |
 *          |      |                | <.list. |             |
 *          |      `----------------'         `-------------'
 *          |     uthread |  ^             bsd_info |  ^
 *          |             v  | vc_thread            v  | task
 *          |      ,----------------.         ,-------------.
 *          |      |                |         |             |
 *          |      | parent_uthread | <.list. | parent_proc |
 *          |      |                |         |             |
 *          |      `----------------'         `-------------'
 *          |     uu_proc |  . list
 *          |             v  v
 *          |      ,----------------.
 *          `----- |                |
 *      p_vforkact | child_proc     | <-- current_proc()
 *                 |                |
 *                 `----------------'
 */
int
vfork(proc_t parent_proc, __unused struct vfork_args *uap, int32_t *retval)
{
	thread_t child_thread;
	int err;

	os_log(OS_LOG_DEFAULT, "vfork called by %s[%d]",
	    parent_proc->p_comm, proc_getpid(parent_proc));
	if (bootarg_panic_on_vfork) {
		panic("vfork called by %s[%d] (bootarg -panic_on_vfork)",
		    parent_proc->p_comm, proc_getpid(parent_proc));
	}

	if ((err = fork1(parent_proc, &child_thread, PROC_CREATE_VFORK, NULL)) != 0) {
		retval[1] = 0;
	} else {
		uthread_t ut = get_bsdthread_info(current_thread());
		proc_t child_proc = ut->uu_proc;

		retval[0] = proc_getpid(child_proc);
		retval[1] = 1;          /* flag child return for user space */

		/*
		 * Drop the signal lock on the child which was taken on our
		 * behalf by forkproc()/cloneproc() to prevent signals being
		 * received by the child in a partially constructed state.
		 */
		proc_signalend(child_proc, 0);
		proc_transend(child_proc, 0);

		proc_knote(parent_proc, NOTE_FORK | proc_getpid(child_proc));
		DTRACE_PROC1(create, proc_t, child_proc);
		ut->uu_flag &= ~UT_VFORKING;
	}

	return err;
}
#endif /* CONFIG_VFORK */

/*
 * fork1
 *
 * Description:	common code used by all new process creation other than the
 *		bootstrap of the initial process on the system
 *
 * Parameters: parent_proc		parent process of the process being
 *		child_threadp		pointer to location to receive the
 *					Mach thread_t of the child process
 *					created
 *		kind			kind of creation being requested
 *		coalitions		if spawn, the set of coalitions the
 *					child process should join, or NULL to
 *					inherit the parent's. On non-spawns,
 *					this param is ignored and the child
 *					always inherits the parent's
 *					coalitions.
 *
 * Notes:	Permissable values for 'kind':
 *
 *		PROC_CREATE_FORK	Create a complete process which will
 *					return actively running in both the
 *					parent and the child; the child copies
 *					the parent address space.
 *		PROC_CREATE_SPAWN	Create a complete process which will
 *					return actively running in the parent
 *					only after returning actively running
 *					in the child; the child address space
 *					is newly created by an image activator,
 *					after which the child is run.
 *		PROC_CREATE_VFORK	Creates a partial process which will
 *					borrow the parent task, thread, and
 *					uthread to return running in the child;
 *					the child address space and other parts
 *					are lazily created at execve() time, or
 *					the child is terminated, and the parent
 *					does not actively run until that
 *					happens.
 *
 *		At first it may seem strange that we return the child thread
 *		address rather than process structure, since the process is
 *		the only part guaranteed to be "new"; however, since we do
 *		not actualy adjust other references between Mach and BSD (see
 *		the block diagram above the implementation of vfork()), this
 *		is the only method which guarantees us the ability to get
 *		back to the other information.
 */
int
fork1(proc_t parent_proc, thread_t *child_threadp, int kind, coalition_t *coalitions)
{
#if CONFIG_VFORK
	thread_t parent_thread = (thread_t)current_thread();
	uthread_t parent_uthread = (uthread_t)get_bsdthread_info(parent_thread);
#endif /* CONFIG_VFORK */
	proc_t child_proc = NULL;       /* set in switch, but compiler... */
	thread_t child_thread = NULL;
	uid_t uid;
	size_t count;
	int err = 0;
	int spawn = 0;
	rlim_t rlimit_nproc_cur;

	/*
	 * Although process entries are dynamically created, we still keep
	 * a global limit on the maximum number we will create.  Don't allow
	 * a nonprivileged user to use the last process; don't let root
	 * exceed the limit. The variable nprocs is the current number of
	 * processes, maxproc is the limit.
	 */
	uid = kauth_getruid();
	proc_list_lock();
	if ((nprocs >= maxproc - 1 && uid != 0) || nprocs >= maxproc) {
#if (DEVELOPMENT || DEBUG) && !defined(XNU_TARGET_OS_OSX)
		/*
		 * On the development kernel, panic so that the fact that we hit
		 * the process limit is obvious, as this may very well wedge the
		 * system.
		 */
		panic("The process table is full; parent pid=%d", proc_getpid(parent_proc));
#endif
		proc_list_unlock();
		tablefull("proc");
		return EAGAIN;
	}
	proc_list_unlock();

	/*
	 * Increment the count of procs running with this uid. Don't allow
	 * a nonprivileged user to exceed their current limit, which is
	 * always less than what an rlim_t can hold.
	 * (locking protection is provided by list lock held in chgproccnt)
	 */
	count = chgproccnt(uid, 1);
	rlimit_nproc_cur = proc_limitgetcur(parent_proc, RLIMIT_NPROC);
	if (uid != 0 &&
	    (rlim_t)count > rlimit_nproc_cur) {
#if (DEVELOPMENT || DEBUG) && !defined(XNU_TARGET_OS_OSX)
		/*
		 * On the development kernel, panic so that the fact that we hit
		 * the per user process limit is obvious.  This may be less dire
		 * than hitting the global process limit, but we cannot rely on
		 * that.
		 */
		panic("The per-user process limit has been hit; parent pid=%d, uid=%d", proc_getpid(parent_proc), uid);
#endif
		err = EAGAIN;
		goto bad;
	}

#if CONFIG_MACF
	/*
	 * Determine if MAC policies applied to the process will allow
	 * it to fork.  This is an advisory-only check.
	 */
	err = mac_proc_check_fork(parent_proc);
	if (err != 0) {
		goto bad;
	}
#endif

	switch (kind) {
#if CONFIG_VFORK
	case PROC_CREATE_VFORK:
		/*
		 * Prevent a vfork while we are in vfork(); we should
		 * also likely preventing a fork here as well, and this
		 * check should then be outside the switch statement,
		 * since the proc struct contents will copy from the
		 * child and the tash/thread/uthread from the parent in
		 * that case.  We do not support vfork() in vfork()
		 * because we don't have to; the same non-requirement
		 * is true of both fork() and posix_spawn() and any
		 * call  other than execve() amd _exit(), but we've
		 * been historically lenient, so we continue to be so
		 * (for now).
		 *
		 * <rdar://6640521> Probably a source of random panics
		 */
		if (parent_uthread->uu_flag & UT_VFORK) {
			printf("fork1 called within vfork by %s\n", parent_proc->p_comm);
			err = EINVAL;
			goto bad;
		}

		/*
		 * Flag us in progress; if we chose to support vfork() in
		 * vfork(), we would chain our parent at this point (in
		 * effect, a stack push).  We don't, since we actually want
		 * to disallow everything not specified in the standard
		 */
		proc_vfork_begin(parent_proc);

		/* The newly created process comes with signal lock held */
		if ((child_proc = forkproc(parent_proc)) == NULL) {
			/* Failed to allocate new process */
			proc_vfork_end(parent_proc);
			err = ENOMEM;
			goto bad;
		}

// XXX BEGIN: wants to move to be common code (and safe)
#if CONFIG_MACF
		/*
		 * allow policies to associate the credential/label that
		 * we referenced from the parent ... with the child
		 * JMM - this really isn't safe, as we can drop that
		 *       association without informing the policy in other
		 *       situations (keep long enough to get policies changed)
		 */
		mac_cred_label_associate_fork(proc_ucred(child_proc), child_proc);
#endif

		/*
		 * Propogate change of PID - may get new cred if auditing.
		 *
		 * NOTE: This has no effect in the vfork case, since
		 *	child_proc->task != current_task(), but we duplicate it
		 *	because this is probably, ultimately, wrong, since we
		 *	will be running in the "child" which is the parent task
		 *	with the wrong token until we get to the execve() or
		 *	_exit() call; a lot of "undefined" can happen before
		 *	that.
		 *
		 * <rdar://6640530> disallow everything but exeve()/_exit()?
		 */
		set_security_token(child_proc);

		AUDIT_ARG(pid, proc_getpid(child_proc));

// XXX END: wants to move to be common code (and safe)

		/*
		 * BORROW PARENT TASK, THREAD, UTHREAD FOR CHILD
		 *
		 * Note: this is where we would "push" state instead of setting
		 * it for nested vfork() support (see proc_vfork_end() for
		 * description if issues here).
		 */
		proc_set_task(child_proc, proc_task(parent_proc));

		child_proc->p_lflag  |= P_LINVFORK;
		child_proc->p_vforkact = parent_thread;
		child_proc->p_stat = SRUN;

		/*
		 * Until UT_VFORKING is cleared at the end of the vfork
		 * syscall, the process identity of this thread is slightly
		 * murky.
		 *
		 * As long as UT_VFORK and it's associated field (uu_proc)
		 * is set, current_proc() will always return the child process.
		 *
		 * However dtrace_proc_selfpid() returns the parent pid to
		 * ensure that e.g. the proc:::create probe actions accrue
		 * to the parent.  (Otherwise the child magically seems to
		 * have created itself!)
		 */
		parent_uthread->uu_flag |= UT_VFORK | UT_VFORKING;
		parent_uthread->uu_proc = child_proc;
		parent_uthread->uu_userstate = (void *)act_thread_csave();
		parent_uthread->uu_vforkmask = parent_uthread->uu_sigmask;

		/* temporarily drop thread-set-id state */
		if (parent_uthread->uu_flag & UT_SETUID) {
			parent_uthread->uu_flag |= UT_WASSETUID;
			parent_uthread->uu_flag &= ~UT_SETUID;
		}

		/* blow thread state information */
		/* XXX is this actually necessary, given syscall return? */
		thread_set_child(parent_thread, proc_getpid(child_proc));

		child_proc->p_acflag = AFORK;   /* forked but not exec'ed */

		/*
		 * Preserve synchronization semantics of vfork.  If
		 * waiting for child to exec or exit, set P_PPWAIT
		 * on child, and sleep on our proc (in case of exit).
		 */
		child_proc->p_lflag |= P_LPPWAIT;
		pinsertchild(parent_proc, child_proc);  /* set visible */

		break;
#endif /* CONFIG_VFORK */

	case PROC_CREATE_SPAWN:
		/*
		 * A spawned process differs from a forked process in that
		 * the spawned process does not carry around the parents
		 * baggage with regard to address space copying, dtrace,
		 * and so on.
		 */
		spawn = 1;

		OS_FALLTHROUGH;

	case PROC_CREATE_FORK:
		/*
		 * When we clone the parent process, we are going to inherit
		 * its task attributes and memory, since when we fork, we
		 * will, in effect, create a duplicate of it, with only minor
		 * differences.  Contrarily, spawned processes do not inherit.
		 */
		if ((child_thread = cloneproc(parent_proc->task,
		    spawn ? coalitions : NULL,
		    parent_proc,
		    spawn ? FALSE : TRUE,
		    FALSE)) == NULL) {
			/* Failed to create thread */
			err = EAGAIN;
			goto bad;
		}

		/* copy current thread state into the child thread (only for fork) */
		if (!spawn) {
			thread_dup(child_thread);
		}

		/* child_proc = child_thread->task->proc; */
		child_proc = (proc_t)(get_bsdtask_info(get_threadtask(child_thread)));

// XXX BEGIN: wants to move to be common code (and safe)
#if CONFIG_MACF
		/*
		 * allow policies to associate the credential/label that
		 * we referenced from the parent ... with the child
		 * JMM - this really isn't safe, as we can drop that
		 *       association without informing the policy in other
		 *       situations (keep long enough to get policies changed)
		 */
		mac_cred_label_associate_fork(proc_ucred(child_proc), child_proc);
#endif

		/*
		 * Propogate change of PID - may get new cred if auditing.
		 *
		 * NOTE: This has no effect in the vfork case, since
		 *	child_proc->task != current_task(), but we duplicate it
		 *	because this is probably, ultimately, wrong, since we
		 *	will be running in the "child" which is the parent task
		 *	with the wrong token until we get to the execve() or
		 *	_exit() call; a lot of "undefined" can happen before
		 *	that.
		 *
		 * <rdar://6640530> disallow everything but exeve()/_exit()?
		 */
		set_security_token(child_proc);

		AUDIT_ARG(pid, proc_getpid(child_proc));

// XXX END: wants to move to be common code (and safe)

		/*
		 * Blow thread state information; this is what gives the child
		 * process its "return" value from a fork() call.
		 *
		 * Note: this should probably move to fork() proper, since it
		 * is not relevent to spawn, and the value won't matter
		 * until we resume the child there.  If you are in here
		 * refactoring code, consider doing this at the same time.
		 */
		thread_set_child(child_thread, proc_getpid(child_proc));

		child_proc->p_acflag = AFORK;   /* forked but not exec'ed */

#if CONFIG_DTRACE
		dtrace_proc_fork(parent_proc, child_proc, spawn);
#endif  /* CONFIG_DTRACE */
		if (!spawn) {
			/*
			 * Of note, we need to initialize the bank context behind
			 * the protection of the proc_trans lock to prevent a race with exit.
			 */
			task_bank_init(get_threadtask(child_thread));
		}

		break;

	default:
		panic("fork1 called with unknown kind %d", kind);
		break;
	}


	/* return the thread pointer to the caller */
	*child_threadp = child_thread;

bad:
	/*
	 * In the error case, we return a 0 value for the returned pid (but
	 * it is ignored in the trampoline due to the error return); this
	 * is probably not necessary.
	 */
	if (err) {
		(void)chgproccnt(uid, -1);
	}

	return err;
}


#if CONFIG_VFORK
/*
 * vfork_return
 *
 * Description:	"Return" to parent vfork thread() following execve/_exit;
 *		this is done by reassociating the parent process structure
 *		with the task, thread, and uthread.
 *
 *		Refer to the ASCII art above vfork() to figure out the
 *		state we're undoing.
 *
 * Parameters:	child_proc		Child process
 *		retval			System call return value array
 *		rval			Return value to present to parent
 *
 * Returns:	void
 *
 * Notes:	The caller resumes or exits the parent, as appropriate, after
 *		calling this function.
 */
void
vfork_return(proc_t child_proc, int32_t *retval, int rval)
{
	task_t parent_task = get_threadtask(child_proc->p_vforkact);
	proc_t parent_proc = get_bsdtask_info(parent_task);
	thread_t th = current_thread();
	uthread_t uth = get_bsdthread_info(th);

	act_thread_catt(uth->uu_userstate);

	/* clear vfork state in parent proc structure */
	proc_vfork_end(parent_proc);

	/* REPATRIATE PARENT TASK, THREAD, UTHREAD */
	uth->uu_userstate = 0;
	uth->uu_flag &= ~UT_VFORK;
	/* restore thread-set-id state */
	if (uth->uu_flag & UT_WASSETUID) {
		uth->uu_flag |= UT_SETUID;
		uth->uu_flag &= ~UT_WASSETUID;
	}
	uth->uu_proc = 0;
	uth->uu_sigmask = uth->uu_vforkmask;

	proc_lock(child_proc);
	child_proc->p_lflag &= ~P_LINVFORK;
	child_proc->p_vforkact = 0;
	proc_unlock(child_proc);

	thread_set_parent(th, rval);

	if (retval) {
		retval[0] = rval;
		retval[1] = 0;                  /* mark parent */
	}
}
#endif /* CONFIG_VFORK */


/*
 * fork_create_child
 *
 * Description:	Common operations associated with the creation of a child
 *		process. Return with new task and first thread's control port movable
 *      and not pinned.
 *
 * Parameters:	parent_task		parent task
 *		parent_coalitions	parent's set of coalitions
 *		child_proc			child process
 *		inherit_memory		TRUE, if the parents address space is
 *							to be inherited by the child
 *		is_64bit_addr		TRUE, if the child being created will
 *							be associated with a 64 bit address space
 *		is_64bit_data		TRUE if the child being created will use a
 *                                                       64-bit register state
 *		in_exec				TRUE, if called from execve or posix spawn set exec
 *							FALSE, if called from fork or vfexec
 *
 * Note:	This code is called in the fork() case, from the execve() call
 *		graph, if implementing an execve() following a vfork(), from
 *		the posix_spawn() call graph (which implicitly includes a
 *		vfork() equivalent call, and in the system bootstrap case.
 *
 *		It creates a new task and thread (and as a side effect of the
 *		thread creation, a uthread) in the parent coalition set, which is
 *		then associated with the process 'child'.  If the parent
 *		process address space is to be inherited, then a flag
 *		indicates that the newly created task should inherit this from
 *		the child task.
 *
 *		As a special concession to bootstrapping the initial process
 *		in the system, it's possible for 'parent_task' to be TASK_NULL;
 *		in this case, 'inherit_memory' MUST be FALSE.
 */
thread_t
fork_create_child(task_t parent_task,
    coalition_t *parent_coalitions,
    proc_t child_proc,
    int inherit_memory,
    int is_64bit_addr,
    int is_64bit_data,
    int in_exec)
{
	thread_t        child_thread = NULL;
	task_t          child_task;
	kern_return_t   result;

	/* Create a new task for the child process */
	result = task_create_internal(parent_task,
	    parent_coalitions,
	    inherit_memory,
	    is_64bit_addr,
	    is_64bit_data,
	    TF_NONE,
	    in_exec ? TPF_EXEC_COPY : TPF_NONE,                        /* Mark the task exec copy if in execve */
	    (TRW_LRETURNWAIT | TRW_LRETURNWAITER),                     /* All created threads will wait in task_wait_to_return */
	    &child_task);
	if (result != KERN_SUCCESS) {
		printf("%s: task_create_internal failed.  Code: %d\n",
		    __func__, result);
		goto bad;
	}

	if (!in_exec) {
		/*
		 * Set the child process task to the new task if not in exec,
		 * will set the task for exec case in proc_exec_switch_task after image activation.
		 */
		proc_set_task(child_proc, child_task);
	}

	/* Set child task process to child proc */
	set_bsdtask_info(child_task, child_proc);

	/* Propagate CPU limit timer from parent */
	if (timerisset(&child_proc->p_rlim_cpu)) {
		task_vtimer_set(child_task, TASK_VTIMER_RLIM);
	}

	/*
	 * Set child process BSD visible scheduler priority if nice value
	 * inherited from parent
	 */
	if (child_proc->p_nice != 0) {
		resetpriority(child_proc);
	}

	/*
	 * Create main thread for the child process. Its control port is not immovable/pinned
	 * until main_thread_set_immovable_pinned().
	 *
	 * The new thread is waiting on the event triggered by 'task_clear_return_wait'
	 */
	result = thread_create_waiting(child_task,
	    (thread_continue_t)task_wait_to_return,
	    task_get_return_wait_event(child_task),
	    TH_CREATE_WAITING_OPTION_NONE,
	    &child_thread);

	if (result != KERN_SUCCESS) {
		printf("%s: thread_create failed. Code: %d\n",
		    __func__, result);
		task_deallocate(child_task);
		child_task = NULL;
	}

	/*
	 * Tag thread as being the first thread in its task.
	 */
	thread_set_tag(child_thread, THREAD_TAG_MAINTHREAD);

bad:
	thread_yield_internal(1);

	return child_thread;
}


/*
 * fork
 *
 * Description:	fork system call.
 *
 * Parameters:	parent			Parent process to fork
 *		uap (void)		[unused]
 *		retval			Return value
 *
 * Returns:	0			Success
 *		EAGAIN			Resource unavailable, try again
 *
 * Notes:	Attempts to create a new child process which inherits state
 *		from the parent process.  If successful, the call returns
 *		having created an initially suspended child process with an
 *		extra Mach task and thread reference, for which the thread
 *		is initially suspended.  Until we resume the child process,
 *		it is not yet running.
 *
 *		The return information to the child is contained in the
 *		thread state structure of the new child, and does not
 *		become visible to the child through a normal return process,
 *		since it never made the call into the kernel itself in the
 *		first place.
 *
 *		After resuming the thread, this function returns directly to
 *		the parent process which invoked the fork() system call.
 *
 * Important:	The child thread_resume occurs before the parent returns;
 *		depending on scheduling latency, this means that it is not
 *		deterministic as to whether the parent or child is scheduled
 *		to run first.  It is entirely possible that the child could
 *		run to completion prior to the parent running.
 */
int
fork(proc_t parent_proc, __unused struct fork_args *uap, int32_t *retval)
{
	thread_t child_thread;
	int err;

	retval[1] = 0;          /* flag parent return for user space */

	if ((err = fork1(parent_proc, &child_thread, PROC_CREATE_FORK, NULL)) == 0) {
		task_t child_task;
		proc_t child_proc;

		/* Return to the parent */
		child_proc = (proc_t)get_bsdthreadtask_info(child_thread);
		retval[0] = proc_getpid(child_proc);

		child_task = (task_t)get_threadtask(child_thread);
		assert(child_task != TASK_NULL);

		/* task_control_port_options has been inherited from parent, apply it */
		task_set_immovable_pinned(child_task);
		main_thread_set_immovable_pinned(child_thread);

		/*
		 * Drop the signal lock on the child which was taken on our
		 * behalf by forkproc()/cloneproc() to prevent signals being
		 * received by the child in a partially constructed state.
		 */
		proc_signalend(child_proc, 0);
		proc_transend(child_proc, 0);

		/* flag the fork has occurred */
		proc_knote(parent_proc, NOTE_FORK | proc_getpid(child_proc));
		DTRACE_PROC1(create, proc_t, child_proc);

#if CONFIG_DTRACE
		if ((dtrace_proc_waitfor_hook = dtrace_proc_waitfor_exec_ptr) != NULL) {
			(*dtrace_proc_waitfor_hook)(child_proc);
		}
#endif

		/* "Return" to the child */
		task_clear_return_wait(get_threadtask(child_thread), TCRW_CLEAR_ALL_WAIT);

		/* drop the extra references we got during the creation */
		task_deallocate(child_task);
		thread_deallocate(child_thread);
	}

	return err;
}


/*
 * cloneproc
 *
 * Description: Create a new process from a specified process.
 *
 * Parameters:	parent_task		The parent task to be cloned, or
 *					TASK_NULL is task characteristics
 *					are not to be inherited
 *					be cloned, or TASK_NULL if the new
 *					task is not to inherit the VM
 *					characteristics of the parent
 *		parent_proc		The parent process to be cloned
 *		inherit_memory		True if the child is to inherit
 *					memory from the parent; if this is
 *					non-NULL, then the parent_task must
 *					also be non-NULL
 *		memstat_internal	Whether to track the process in the
 *					jetsam priority list (if configured)
 *
 * Returns:	!NULL			pointer to new child thread
 *		NULL			Failure (unspecified)
 *
 * Note:	On return newly created child process has signal lock held
 *		to block delivery of signal to it if called with lock set.
 *		fork() code needs to explicity remove this lock before
 *		signals can be delivered
 *
 *		In the case of bootstrap, this function can be called from
 *		bsd_utaskbootstrap() in order to bootstrap the first process;
 *		the net effect is to provide a uthread structure for the
 *		kernel process associated with the kernel task.
 *
 * XXX:		Tristating using the value parent_task as the major key
 *		and inherit_memory as the minor key is something we should
 *		refactor later; we owe the current semantics, ultimately,
 *		to the semantics of task_create_internal.  For now, we will
 *		live with this being somewhat awkward.
 */
thread_t
cloneproc(task_t parent_task, coalition_t *parent_coalitions, proc_t parent_proc, int inherit_memory, int memstat_internal)
{
#if !CONFIG_MEMORYSTATUS
#pragma unused(memstat_internal)
#endif
	task_t child_task;
	proc_t child_proc;
	thread_t child_thread = NULL;

	if ((child_proc = forkproc(parent_proc)) == NULL) {
		/* Failed to allocate new process */
		goto bad;
	}

	/*
	 * In the case where the parent_task is TASK_NULL (during the init path)
	 * we make the assumption that the register size will be the same as the
	 * address space size since there's no way to determine the possible
	 * register size until an image is exec'd.
	 *
	 * The only architecture that has different address space and register sizes
	 * (arm64_32) isn't being used within kernel-space, so the above assumption
	 * always holds true for the init path.
	 */
	const int parent_64bit_addr = parent_proc->p_flag & P_LP64;
	const int parent_64bit_data = (parent_task == TASK_NULL) ? parent_64bit_addr : task_get_64bit_data(parent_task);

	child_thread = fork_create_child(parent_task,
	    parent_coalitions,
	    child_proc,
	    inherit_memory,
	    parent_64bit_addr,
	    parent_64bit_data,
	    FALSE);

	if (child_thread == NULL) {
		/*
		 * Failed to create thread; now we must deconstruct the new
		 * process previously obtained from forkproc().
		 */
		forkproc_free(child_proc);
		goto bad;
	}

	child_task = get_threadtask(child_thread);
	if (parent_64bit_addr) {
		OSBitOrAtomic(P_LP64, (UInt32 *)&child_proc->p_flag);
	} else {
		OSBitAndAtomic(~((uint32_t)P_LP64), (UInt32 *)&child_proc->p_flag);
	}

#if CONFIG_MEMORYSTATUS
	if (memstat_internal) {
		proc_list_lock();
		child_proc->p_memstat_state |= P_MEMSTAT_INTERNAL;
		proc_list_unlock();
	}
#endif

	/* make child visible */
	pinsertchild(parent_proc, child_proc);

	/*
	 * Make child runnable, set start time.
	 */
	child_proc->p_stat = SRUN;
bad:
	return child_thread;
}

void
proc_set_sigact(proc_t p, int sig, user_addr_t sigact)
{
	if (sig < 0 || sig >= NSIG) {
		panic("%s: invalid sig\n", __func__);
	}

	p->p_sigacts->ps_sigact[sig] = sigact;
}

void
proc_set_trampact(proc_t p, int sig, user_addr_t trampact)
{
	if (sig < 0 || sig >= NSIG) {
		panic("%s: invalid sig\n", __func__);
	}

	p->p_sigacts->ps_trampact[sig] = trampact;
}

void
proc_set_sigact_trampact(proc_t p, int sig, user_addr_t sigact, user_addr_t trampact)
{
	if (sig < 0 || sig >= NSIG) {
		panic("%s: invalid sig\n", __func__);
	}

	p->p_sigacts->ps_sigact[sig] = sigact;
	p->p_sigacts->ps_trampact[sig] = trampact;
}

void
proc_reset_sigact(proc_t p, sigset_t sigs)
{
	int nc;

	while (sigs) {
		nc = ffs((unsigned int)sigs);
		p->p_sigacts->ps_sigact[nc] = SIG_DFL;
		sigs &= ~sigmask(nc);
	}
}

/*
 * Destroy a process structure that resulted from a call to forkproc(), but
 * which must be returned to the system because of a subsequent failure
 * preventing it from becoming active.
 *
 * Parameters:	p			The incomplete process from forkproc()
 *
 * Returns:	(void)
 *
 * Note:	This function should only be used in an error handler following
 *		a call to forkproc().
 *
 *		Operations occur in reverse order of those in forkproc().
 */
void
forkproc_free(proc_t p)
{
	struct pgrp *pg;

#if CONFIG_PERSONAS
	persona_proc_drop(p);
#endif /* CONFIG_PERSONAS */

#if PSYNCH
	pth_proc_hashdelete(p);
#endif /* PSYNCH */

	/* We held signal and a transition locks; drop them */
	proc_signalend(p, 0);
	proc_transend(p, 0);

	/*
	 * If we have our own copy of the resource limits structure, we
	 * need to free it.  If it's a shared copy, we need to drop our
	 * reference on it.
	 */
	proc_limitdrop(p);

#if SYSV_SHM
	/* Need to drop references to the shared memory segment(s), if any */
	if (p->vm_shm) {
		/*
		 * Use shmexec(): we have no address space, so no mappings
		 *
		 * XXX Yes, the routine is badly named.
		 */
		shmexec(p);
	}
#endif

	/* Need to undo the effects of the fdt_fork(), if any */
	fdt_invalidate(p);
	fdt_destroy(p);

	/*
	 * Drop the reference on a text vnode pointer, if any
	 * XXX This code is broken in forkproc(); see <rdar://4256419>;
	 * XXX if anyone ever uses this field, we will be extremely unhappy.
	 */
	if (p->p_textvp) {
		vnode_rele(p->p_textvp);
		p->p_textvp = NULL;
	}

	/* Update the audit session proc count */
	AUDIT_SESSION_PROCEXIT(p);

	lck_mtx_destroy(&p->p_mlock, &proc_mlock_grp);
	lck_mtx_destroy(&p->p_ucred_mlock, &proc_ucred_mlock_grp);
#if CONFIG_DTRACE
	lck_mtx_destroy(&p->p_dtrace_sprlock, &proc_lck_grp);
#endif
	lck_spin_destroy(&p->p_slock, &proc_slock_grp);

	/* Release the credential reference */
	kauth_cred_t tmp_ucred = proc_ucred(p);
	kauth_cred_unref(&tmp_ucred);
	proc_set_ucred(p, tmp_ucred);

	proc_list_lock();
	/* Decrement the count of processes in the system */
	nprocs--;

	/* quit the group */
	pg = pgrp_leave_locked(p);

	/* Take it out of process hash */
	assert(os_ref_get_raw_mask(&p->p_refcount) ==
	    ((1U << P_REF_BITS) | P_REF_NEW));
	os_atomic_xor(&p->p_refcount, P_REF_NEW | P_REF_DEAD, relaxed);
	phash_remove_locked(proc_getpid(p), p);

	proc_list_unlock();

	pgrp_rele(pg);

	thread_call_free(p->p_rcall);

	/* Free allocated memory */
	zfree(proc_sigacts_zone, p->p_sigacts);
	p->p_sigacts = NULL;
	zfree(proc_stats_zone, p->p_stats);
	p->p_stats = NULL;
	if (p->p_subsystem_root_path) {
		zfree(ZV_NAMEI, p->p_subsystem_root_path);
	}

	proc_checkdeadrefs(p);
	proc_wait_release(p);
}


/*
 * forkproc
 *
 * Description:	Create a new process structure, given a parent process
 *		structure.
 *
 * Parameters:	parent_proc		The parent process
 *
 * Returns:	!NULL			The new process structure
 *		NULL			Error (insufficient free memory)
 *
 * Note:	When successful, the newly created process structure is
 *		partially initialized; if a caller needs to deconstruct the
 *		returned structure, they must call forkproc_free() to do so.
 */
proc_t
forkproc(proc_t parent_proc)
{
	static uint64_t nextuniqueid = 0;
	static pid_t lastpid = 0;

	proc_t child_proc;      /* Our new process */
	int error = 0;
	struct pgrp *pg;
	uthread_t parent_uthread = (uthread_t)get_bsdthread_info(current_thread());
	rlim_t rlimit_cpu_cur;
	pid_t pid;

	child_proc = zalloc_flags(proc_zone, Z_WAITOK | Z_ZERO);
	child_proc->p_stats = zalloc_flags(proc_stats_zone, Z_WAITOK | Z_ZERO);
	child_proc->p_sigacts = zalloc_flags(proc_sigacts_zone, Z_WAITOK);
	os_ref_init_mask(&child_proc->p_refcount, P_REF_BITS, &p_refgrp, P_REF_NEW);
	os_ref_init_raw(&child_proc->p_waitref, &p_refgrp);

	/* allocate a callout for use by interval timers */
	child_proc->p_rcall = thread_call_allocate((thread_call_func_t)realitexpire, child_proc);


	/*
	 * Find an unused PID.
	 */

	fdt_init(child_proc);

	proc_list_lock();

	pid = lastpid;
	do {
		/*
		 * If the process ID prototype has wrapped around,
		 * restart somewhat above 0, as the low-numbered procs
		 * tend to include daemons that don't exit.
		 */
		if (++pid >= PID_MAX) {
			pid = 100;
		}
		if (pid == lastpid) {
			panic("Unable to allocate a new pid");
		}

		/* if the pid stays in hash both for zombie and runniing state */
	} while (phash_find_locked(pid) != PROC_NULL ||
	    pghash_find_locked(pid) != PGRP_NULL ||
	    session_find_locked(pid) != SESSION_NULL);

	lastpid = pid;
	nprocs++;
	child_proc->p_pid = pid;
	child_proc->p_idversion = OSIncrementAtomic(&nextpidversion);
	/* kernel process is handcrafted and not from fork, so start from 1 */
	child_proc->p_uniqueid = ++nextuniqueid;

	/* Insert in the hash, and inherit our group (and session) */
	phash_insert_locked(pid, child_proc);
	pg = pgrp_enter_locked(parent_proc, child_proc);
	proc_list_unlock();

	if (child_proc->p_uniqueid == startup_serial_num_procs) {
		/*
		 * Turn off startup serial logging now that we have reached
		 * the defined number of startup processes.
		 */
		startup_serial_logging_active = false;
	}

	/*
	 * We've identified the PID we are going to use;
	 * initialize the new process structure.
	 */
	child_proc->p_stat = SIDL;

	/*
	 * The zero'ing of the proc was at the allocation time due to need
	 * for insertion to hash.  Copy the section that is to be copied
	 * directly from the parent.
	 */
	__nochk_bcopy(&parent_proc->p_startcopy, &child_proc->p_startcopy,
	    (unsigned) ((caddr_t)&child_proc->p_endcopy - (caddr_t)&child_proc->p_startcopy));

#if defined(HAS_APPLE_PAC)
	/*
	 * The p_textvp and p_pgrp pointers are address-diversified by PAC, so we must
	 * resign them here for the new proc
	 */
	if (parent_proc->p_textvp) {
		child_proc->p_textvp = parent_proc->p_textvp;
	}
#endif /* defined(HAS_APPLE_PAC) */

	/*
	 * Some flags are inherited from the parent.
	 * Duplicate sub-structures as needed.
	 * Increase reference counts on shared objects.
	 * The p_stats and p_sigacts substructs are set in vm_fork.
	 */
#if CONFIG_DELAY_IDLE_SLEEP
	child_proc->p_flag = (parent_proc->p_flag & (P_LP64 | P_TRANSLATED | P_DISABLE_ASLR | P_DELAYIDLESLEEP | P_SUGID | P_AFFINITY));
#else /* CONFIG_DELAY_IDLE_SLEEP */
	child_proc->p_flag = (parent_proc->p_flag & (P_LP64 | P_TRANSLATED | P_DISABLE_ASLR | P_SUGID));
#endif /* CONFIG_DELAY_IDLE_SLEEP */

	child_proc->p_vfs_iopolicy = (parent_proc->p_vfs_iopolicy & (P_VFS_IOPOLICY_VALID_MASK));

	child_proc->p_responsible_pid = parent_proc->p_responsible_pid;

	/*
	 * Note that if the current thread has an assumed identity, this
	 * credential will be granted to the new process.
	 */
	proc_set_ucred(child_proc, kauth_cred_get_with_ref());
	/* update cred on proc */
	proc_update_creds_onproc(child_proc);
	/* update audit session proc count */
	AUDIT_SESSION_PROCNEW(child_proc);

	lck_mtx_init(&child_proc->p_mlock, &proc_mlock_grp, &proc_lck_attr);
	lck_mtx_init(&child_proc->p_ucred_mlock, &proc_ucred_mlock_grp, &proc_lck_attr);
#if CONFIG_DTRACE
	lck_mtx_init(&child_proc->p_dtrace_sprlock, &proc_lck_grp, &proc_lck_attr);
#endif
	lck_spin_init(&child_proc->p_slock, &proc_slock_grp, &proc_lck_attr);

	klist_init(&child_proc->p_klist);

	if (child_proc->p_textvp != NULLVP) {
		/* bump references to the text vnode */
		/* Need to hold iocount across the ref call */
		if ((error = vnode_getwithref(child_proc->p_textvp)) == 0) {
			error = vnode_ref(child_proc->p_textvp);
			vnode_put(child_proc->p_textvp);
		}

		if (error != 0) {
			child_proc->p_textvp = NULLVP;
		}
	}

	/*
	 * Copy the parents per process open file table to the child; if
	 * there is a per-thread current working directory, set the childs
	 * per-process current working directory to that instead of the
	 * parents.
	 */
	if (fdt_fork(&child_proc->p_fd, parent_proc, parent_uthread->uu_cdir) != 0) {
		forkproc_free(child_proc);
		child_proc = NULL;
		goto bad;
	}

#if SYSV_SHM
	if (parent_proc->vm_shm) {
		/* XXX may fail to attach shm to child */
		(void)shmfork(parent_proc, child_proc);
	}
#endif

	/*
	 * Child inherits the parent's plimit
	 */
	proc_limitfork(parent_proc, child_proc);

	rlimit_cpu_cur = proc_limitgetcur(child_proc, RLIMIT_CPU);
	if (rlimit_cpu_cur != RLIM_INFINITY) {
		child_proc->p_rlim_cpu.tv_sec = (rlimit_cpu_cur > __INT_MAX__) ? __INT_MAX__ : rlimit_cpu_cur;
	}

	/* Intialize new process stats, including start time */
	/* <rdar://6640543> non-zeroed portion contains garbage AFAICT */
	microtime_with_abstime(&child_proc->p_start, &child_proc->p_stats->ps_start);

	if (parent_proc->p_sigacts != NULL) {
		(void)memcpy(child_proc->p_sigacts,
		    parent_proc->p_sigacts, sizeof *child_proc->p_sigacts);
	} else {
		(void)memset(child_proc->p_sigacts, 0, sizeof *child_proc->p_sigacts);
	}

	if (pg->pg_session->s_ttyvp != NULL && parent_proc->p_flag & P_CONTROLT) {
		os_atomic_or(&child_proc->p_flag, P_CONTROLT, relaxed);
	}

	/*
	 * block all signals to reach the process.
	 * no transition race should be occuring with the child yet,
	 * but indicate that the process is in (the creation) transition.
	 */
	proc_signalstart(child_proc, 0);
	proc_transstart(child_proc, 0, 0);

	child_proc->p_pcaction = 0;

	TAILQ_INIT(&child_proc->p_uthlist);
	TAILQ_INIT(&child_proc->p_aio_activeq);
	TAILQ_INIT(&child_proc->p_aio_doneq);

	/* Inherit the parent flags for code sign */
	child_proc->p_csflags = (parent_proc->p_csflags & ~CS_KILLED);

	/*
	 * Copy work queue information
	 *
	 * Note: This should probably only happen in the case where we are
	 *	creating a child that is a copy of the parent; since this
	 *	routine is called in the non-duplication case of vfork()
	 *	or posix_spawn(), then this information should likely not
	 *	be duplicated.
	 *
	 * <rdar://6640553> Work queue pointers that no longer point to code
	 */
	child_proc->p_wqthread = parent_proc->p_wqthread;
	child_proc->p_threadstart = parent_proc->p_threadstart;
	child_proc->p_pthsize = parent_proc->p_pthsize;
	if ((parent_proc->p_lflag & P_LREGISTER) != 0) {
		child_proc->p_lflag |= P_LREGISTER;
	}
	child_proc->p_dispatchqueue_offset = parent_proc->p_dispatchqueue_offset;
	child_proc->p_dispatchqueue_serialno_offset = parent_proc->p_dispatchqueue_serialno_offset;
	child_proc->p_dispatchqueue_label_offset = parent_proc->p_dispatchqueue_label_offset;
	child_proc->p_return_to_kernel_offset = parent_proc->p_return_to_kernel_offset;
	child_proc->p_mach_thread_self_offset = parent_proc->p_mach_thread_self_offset;
	child_proc->p_pth_tsd_offset = parent_proc->p_pth_tsd_offset;
	child_proc->p_pthread_wq_quantum_offset = parent_proc->p_pthread_wq_quantum_offset;
#if PSYNCH
	pth_proc_hashinit(child_proc);
#endif /* PSYNCH */

#if CONFIG_PERSONAS
	child_proc->p_persona = NULL;
	error = persona_proc_inherit(child_proc, parent_proc);
	if (error != 0) {
		printf("forkproc: persona_proc_inherit failed (persona %d being destroyed?)\n", persona_get_uid(parent_proc->p_persona));
		forkproc_free(child_proc);
		child_proc = NULL;
		goto bad;
	}
#endif

#if CONFIG_MEMORYSTATUS
	/* Memorystatus init */
	child_proc->p_memstat_state = 0;
	child_proc->p_memstat_effectivepriority = JETSAM_PRIORITY_DEFAULT;
	child_proc->p_memstat_requestedpriority = JETSAM_PRIORITY_DEFAULT;
	child_proc->p_memstat_assertionpriority = 0;
	child_proc->p_memstat_userdata          = 0;
	child_proc->p_memstat_idle_start        = 0;
	child_proc->p_memstat_idle_delta        = 0;
	child_proc->p_memstat_memlimit          = 0;
	child_proc->p_memstat_memlimit_active   = 0;
	child_proc->p_memstat_memlimit_inactive = 0;
	child_proc->p_memstat_relaunch_flags    = P_MEMSTAT_RELAUNCH_UNKNOWN;
#if CONFIG_FREEZE
	child_proc->p_memstat_freeze_sharedanon_pages = 0;
#endif
	child_proc->p_memstat_dirty = 0;
	child_proc->p_memstat_idledeadline = 0;
#endif /* CONFIG_MEMORYSTATUS */

	if (parent_proc->p_subsystem_root_path) {
		size_t parent_length = strlen(parent_proc->p_subsystem_root_path) + 1;
		assert(parent_length <= MAXPATHLEN);
		child_proc->p_subsystem_root_path = zalloc_flags(ZV_NAMEI,
		    Z_WAITOK | Z_ZERO);
		memcpy(child_proc->p_subsystem_root_path, parent_proc->p_subsystem_root_path, parent_length);
	}

bad:
	return child_proc;
}

void
proc_lock(proc_t p)
{
	LCK_MTX_ASSERT(&proc_list_mlock, LCK_MTX_ASSERT_NOTOWNED);
	lck_mtx_lock(&p->p_mlock);
}

void
proc_unlock(proc_t p)
{
	lck_mtx_unlock(&p->p_mlock);
}

void
proc_spinlock(proc_t p)
{
	lck_spin_lock_grp(&p->p_slock, &proc_slock_grp);
}

void
proc_spinunlock(proc_t p)
{
	lck_spin_unlock(&p->p_slock);
}

void
proc_list_lock(void)
{
	lck_mtx_lock(&proc_list_mlock);
}

void
proc_list_unlock(void)
{
	lck_mtx_unlock(&proc_list_mlock);
}

void
proc_ucred_lock(proc_t p)
{
	lck_mtx_lock(&p->p_ucred_mlock);
}

void
proc_ucred_unlock(proc_t p)
{
	lck_mtx_unlock(&p->p_ucred_mlock);
}

void
proc_update_creds_onproc(proc_t p)
{
	kauth_cred_t cred = proc_ucred(p);

	p->p_uid = kauth_cred_getuid(cred);
	p->p_gid = kauth_cred_getgid(cred);
	p->p_ruid = kauth_cred_getruid(cred);
	p->p_rgid = kauth_cred_getrgid(cred);
	p->p_svuid = kauth_cred_getsvuid(cred);
	p->p_svgid = kauth_cred_getsvgid(cred);
}

void *
uthread_alloc(task_t task, thread_t thread, int noinherit)
{
	proc_t p;
	uthread_t uth;
	uthread_t uth_parent;
	void *ut;

	ut = zalloc_flags(uthread_zone, Z_WAITOK | Z_ZERO);

	p = (proc_t) get_bsdtask_info(task);
	uth = (uthread_t)ut;
	uth->uu_thread = thread;

	lck_spin_init(&uth->uu_rethrottle_lock, &rethrottle_lock_grp,
	    LCK_ATTR_NULL);

	/*
	 * Thread inherits credential from the creating thread, if both
	 * are in the same task.
	 *
	 * If the creating thread has no credential or is from another
	 * task we can leave the new thread credential NULL.  If it needs
	 * one later, it will be lazily assigned from the task's process.
	 */
	uth_parent = (uthread_t)get_bsdthread_info(current_thread());
	if ((noinherit == 0) && task == current_task() &&
	    uth_parent != NULL &&
	    IS_VALID_CRED(uth_parent->uu_ucred)) {
		/*
		 * XXX The new thread is, in theory, being created in context
		 * XXX of parent thread, so a direct reference to the parent
		 * XXX is OK.
		 */
		kauth_cred_ref(uth_parent->uu_ucred);
		uth->uu_ucred = uth_parent->uu_ucred;
		/* the credential we just inherited is an assumed credential */
		if (uth_parent->uu_flag & UT_SETUID) {
			uth->uu_flag |= UT_SETUID;
		}
	} else {
		/* sometimes workqueue threads are created out task context */
		if ((task != kernel_task) && (p != PROC_NULL)) {
			uth->uu_ucred = kauth_cred_proc_ref(p);
		} else {
			uth->uu_ucred = NOCRED;
		}
	}


	if ((task != kernel_task) && p) {
		proc_lock(p);
		if (noinherit != 0) {
			/* workq threads will not inherit masks */
			uth->uu_sigmask = ~workq_threadmask;
		} else if (uth_parent) {
			if (uth_parent->uu_flag & UT_SAS_OLDMASK) {
				uth->uu_sigmask = uth_parent->uu_oldmask;
			} else {
				uth->uu_sigmask = uth_parent->uu_sigmask;
			}
		}
		uth->uu_context.vc_thread = thread;
		/*
		 * Do not add the uthread to proc uthlist for exec copy task,
		 * since they do not hold a ref on proc.
		 */
		if (!task_is_exec_copy(task)) {
			TAILQ_INSERT_TAIL(&p->p_uthlist, uth, uu_list);
		}
		proc_unlock(p);

#if CONFIG_DTRACE
		if (p->p_dtrace_ptss_pages != NULL && !task_is_exec_copy(task)) {
			uth->t_dtrace_scratch = dtrace_ptss_claim_entry(p);
		}
#endif
	}

	return ut;
}

/*
 * This routine frees the thread name field of the uthread_t structure. Split out of
 * uthread_cleanup() so thread name does not get deallocated while generating a corpse fork.
 */
void
uthread_cleanup_name(void *uthread)
{
	uthread_t uth = (uthread_t)uthread;

	/*
	 * <rdar://17834538>
	 * Set pth_name to NULL before calling free().
	 * Previously there was a race condition in the
	 * case this code was executing during a stackshot
	 * where the stackshot could try and copy pth_name
	 * after it had been freed and before if was marked
	 * as null.
	 */
	if (uth->pth_name != NULL) {
		void *pth_name = uth->pth_name;
		uth->pth_name = NULL;
		kfree_data(pth_name, MAXTHREADNAMESIZE);
	}
	return;
}

/*
 * This routine frees all the BSD context in uthread except the credential.
 * It does not free the uthread structure as well
 */
void
uthread_cleanup(task_t task, void *uthread, void * bsd_info)
{
	uthread_t uth = (uthread_t)uthread;
	proc_t p = (proc_t)bsd_info;

	uthread_assert_zero_proc_refcount(uthread);

	if (uth->uu_lowpri_window || uth->uu_throttle_info) {
		/*
		 * task is marked as a low priority I/O type
		 * and we've somehow managed to not dismiss the throttle
		 * through the normal exit paths back to user space...
		 * no need to throttle this thread since its going away
		 * but we do need to update our bookeeping w/r to throttled threads
		 *
		 * Calling this routine will clean up any throttle info reference
		 * still inuse by the thread.
		 */
		throttle_lowpri_io(0);
	}
	/*
	 * Per-thread audit state should never last beyond system
	 * call return.  Since we don't audit the thread creation/
	 * removal, the thread state pointer should never be
	 * non-NULL when we get here.
	 */
	assert(uth->uu_ar == NULL);

	if (uth->uu_kqr_bound) {
		kqueue_threadreq_unbind(p, uth->uu_kqr_bound);
	}

	if (uth->uu_select.nbytes) {
		select_cleanup_uthread(&uth->uu_select);
	}

	if (uth->uu_cdir) {
		vnode_rele(uth->uu_cdir);
		uth->uu_cdir = NULLVP;
	}

	if (uth->uu_wqset) {
		if (waitq_set_is_valid(uth->uu_wqset)) {
			waitq_set_deinit(uth->uu_wqset);
		}
		kheap_free(KHEAP_DEFAULT, uth->uu_wqset, uth->uu_wqstate_sz);
		uth->uu_wqset = NULL;
		uth->uu_wqstate_sz = 0;
	}

	os_reason_free(uth->uu_exit_reason);

	if ((task != kernel_task) && p) {
#if CONFIG_VFORK
		if (((uth->uu_flag & UT_VFORK) == UT_VFORK) && (uth->uu_proc != PROC_NULL)) {
			vfork_exit_internal(uth->uu_proc, 0, 1);
		}
#endif /* CONFIG_VFORK */
		/*
		 * Remove the thread from the process list and
		 * transfer [appropriate] pending signals to the process.
		 * Do not remove the uthread from proc uthlist for exec
		 * copy task, since they does not have a ref on proc and
		 * would not have been added to the list.
		 */
		if (get_bsdtask_info(task) == p && !task_is_exec_copy(task)) {
			proc_lock(p);

			TAILQ_REMOVE(&p->p_uthlist, uth, uu_list);
			p->p_siglist |= (uth->uu_siglist & execmask & (~p->p_sigignore | sigcantmask));
			proc_unlock(p);
		}
#if CONFIG_DTRACE
		struct dtrace_ptss_page_entry *tmpptr = uth->t_dtrace_scratch;
		uth->t_dtrace_scratch = NULL;
		if (tmpptr != NULL && !task_is_exec_copy(task)) {
			dtrace_ptss_release_entry(p, tmpptr);
		}
#endif
	}
}

/* This routine releases the credential stored in uthread */
void
uthread_cred_free(void *uthread)
{
	uthread_t uth = (uthread_t)uthread;

	/* and free the uthread itself */
	if (IS_VALID_CRED(uth->uu_ucred)) {
		kauth_cred_t oldcred = uth->uu_ucred;
		uth->uu_ucred = NOCRED;
		kauth_cred_unref(&oldcred);
	}
}

/* This routine frees the uthread structure held in thread structure */
void
uthread_zone_free(void *uthread)
{
	uthread_t uth = (uthread_t)uthread;

	if (uth->t_tombstone) {
		kfree_type(struct doc_tombstone, uth->t_tombstone);
		uth->t_tombstone = NULL;
	}

#if CONFIG_DEBUG_SYSCALL_REJECTION
	size_t const bitstr_len = BITMAP_SIZE(mach_trap_count + nsysent);

	if (uth->syscall_rejection_mask) {
		kfree_data(uth->syscall_rejection_mask, bitstr_len);
		uth->syscall_rejection_mask = NULL;
	}
#endif /* CONFIG_DEBUG_SYSCALL_REJECTION */

	lck_spin_destroy(&uth->uu_rethrottle_lock, &rethrottle_lock_grp);

	uthread_cleanup_name(uthread);
	/* and free the uthread itself */
	zfree(uthread_zone, uthread);
}
