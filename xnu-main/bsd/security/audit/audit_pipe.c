/*-
 * Copyright (c) 2006 Robert N. M. Watson
 * Copyright (c) 2008-2009 Apple, Inc.
 * All rights reserved.
 *
 * This software was developed by Robert Watson for the TrustedBSD Project.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdarg.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/fcntl.h>
#include <sys/conf.h>
#include <sys/poll.h>
#include <sys/user.h>
#include <sys/signalvar.h>
#include <miscfs/devfs/devfs.h>

#include <bsm/audit.h>
#include <security/audit/audit.h>
#include <security/audit/audit_ioctl.h>
#include <security/audit/audit_bsd.h>
#include <security/audit/audit_private.h>

#if CONFIG_AUDIT
/*
 * Implementation of a clonable special device providing a live stream of BSM
 * audit data.  Consumers receive a "tee" of the system audit trail by
 * default, but may also define alternative event selections using ioctls.
 * This interface provides unreliable but timely access to audit events.
 * Consumers should be very careful to avoid introducing event cycles.
 */

/*
 * Memory types.
 */
static MALLOC_DEFINE(M_AUDIT_PIPE, "audit_pipe", "Audit pipes");
static MALLOC_DEFINE(M_AUDIT_PIPE_ENTRY, "audit_pipeent",
    "Audit pipe entries and buffers");
static MALLOC_DEFINE(M_AUDIT_PIPE_PRESELECT, "audit_pipe_presel",
    "Audit pipe preselection structure");

/*
 * Audit pipe buffer parameters.
 */
#define AUDIT_PIPE_QLIMIT_DEFAULT       (128)
#define AUDIT_PIPE_QLIMIT_MIN           (1)
#define AUDIT_PIPE_QLIMIT_MAX           (1024)

/*
 * Description of an entry in an audit_pipe.
 */
struct audit_pipe_entry {
	void                            *ape_record;
	u_int                            ape_record_len;
	TAILQ_ENTRY(audit_pipe_entry)    ape_queue;
};

/*
 * Audit pipes allow processes to express "interest" in the set of records
 * that are delivered via the pipe.  They do this in a similar manner to the
 * mechanism for audit trail configuration, by expressing two global masks,
 * and optionally expressing per-auid masks.  The following data structure is
 * the per-auid mask description.  The global state is stored in the audit
 * pipe data structure.
 *
 * We may want to consider a more space/time-efficient data structure once
 * usage patterns for per-auid specifications are clear.
 */
struct audit_pipe_preselect {
	au_id_t                                  app_auid;
	au_mask_t                                app_mask;
	TAILQ_ENTRY(audit_pipe_preselect)        app_list;
};

/*
 * Description of an individual audit_pipe.  Consists largely of a bounded
 * length queue.
 */
#define AUDIT_PIPE_ASYNC        0x00000001
#define AUDIT_PIPE_NBIO         0x00000002
struct audit_pipe {
	int                              ap_open;       /* Device open? */
	u_int                            ap_flags;

	struct selinfo                   ap_selinfo;
	pid_t                            ap_sigio;

	/*
	 * Per-pipe mutex protecting most fields in this data structure.
	 */
	struct mtx                       ap_mtx;

	/*
	 * Per-pipe sleep lock serializing user-generated reads and flushes.
	 * uiomove() is called to copy out the current head record's data
	 * while the record remains in the queue, so we prevent other threads
	 * from removing it using this lock.
	 */
	struct slck                      ap_sx;

	/*
	 * Condition variable to signal when data has been delivered to a
	 * pipe.
	 */
	struct cv                        ap_cv;

	/*
	 * Various queue-related variables:  qlen and qlimit are a count of
	 * records in the queue; qbyteslen is the number of bytes of data
	 * across all records, and qoffset is the amount read so far of the
	 * first record in the queue.  The number of bytes available for
	 * reading in the queue is qbyteslen - qoffset.
	 */
	u_int                            ap_qlen;
	u_int                            ap_qlimit;
	u_int                            ap_qbyteslen;
	u_int                            ap_qoffset;

	/*
	 * Per-pipe operation statistics.
	 */
	u_int64_t                        ap_inserts;    /* Records added. */
	u_int64_t                        ap_reads;      /* Records read. */
	u_int64_t                        ap_drops;      /* Records dropped. */

	/*
	 * Fields relating to pipe interest: global masks for unmatched
	 * processes (attributable, non-attributable), and a list of specific
	 * interest specifications by auid.
	 */
	int                              ap_preselect_mode;
	au_mask_t                        ap_preselect_flags;
	au_mask_t                        ap_preselect_naflags;
	TAILQ_HEAD(, audit_pipe_preselect)      ap_preselect_list;

	/*
	 * Current pending record list.  Protected by a combination of ap_mtx
	 * and ap_sx.  Note particularly that *both* locks are required to
	 * remove a record from the head of the queue, as an in-progress read
	 * may sleep while copying and therefore cannot hold ap_mtx.
	 */
	TAILQ_HEAD(, audit_pipe_entry)   ap_queue;

	/*
	 * Global pipe list.
	 */
	TAILQ_ENTRY(audit_pipe)          ap_list;
};

#define AUDIT_PIPE_LOCK(ap)             mtx_lock(&(ap)->ap_mtx)
#define AUDIT_PIPE_LOCK_ASSERT(ap)      mtx_assert(&(ap)->ap_mtx, MA_OWNED)
#define AUDIT_PIPE_LOCK_DESTROY(ap)     mtx_destroy(&(ap)->ap_mtx)
#define AUDIT_PIPE_LOCK_INIT(ap)        mtx_init(&(ap)->ap_mtx, \
	                                    "audit_pipe_mtx", NULL, MTX_DEF)
#define AUDIT_PIPE_UNLOCK(ap)           mtx_unlock(&(ap)->ap_mtx)
#define AUDIT_PIPE_MTX(ap)              (&(ap)->ap_mtx)

#define AUDIT_PIPE_SX_LOCK_DESTROY(ap)  slck_destroy(&(ap)->ap_sx)
#define AUDIT_PIPE_SX_LOCK_INIT(ap)     slck_init(&(ap)->ap_sx, "audit_pipe_sx")
#define AUDIT_PIPE_SX_XLOCK_ASSERT(ap)  slck_assert(&(ap)->ap_sx, SA_XLOCKED)
#define AUDIT_PIPE_SX_XLOCK_SIG(ap)     slck_lock_sig(&(ap)->ap_sx)
#define AUDIT_PIPE_SX_XUNLOCK(ap)       slck_unlock(&(ap)->ap_sx)


/*
 * Global list of audit pipes, rwlock to protect it.  Individual record
 * queues on pipes are protected by per-pipe locks; these locks synchronize
 * between threads walking the list to deliver to individual pipes and add/
 * remove of pipes, and are mostly acquired for read.
 */
static TAILQ_HEAD(, audit_pipe)  audit_pipe_list;
static struct rwlock             audit_pipe_lock;

#define AUDIT_PIPE_LIST_LOCK_INIT()     rw_init(&audit_pipe_lock, \
	                                    "audit_pipe_list_lock")
#define AUDIT_PIPE_LIST_RLOCK()         rw_rlock(&audit_pipe_lock)
#define AUDIT_PIPE_LIST_RUNLOCK()       rw_runlock(&audit_pipe_lock)
#define AUDIT_PIPE_LIST_WLOCK()         rw_wlock(&audit_pipe_lock)
#define AUDIT_PIPE_LIST_WLOCK_ASSERT()  rw_assert(&audit_pipe_lock, \
	                                    RA_WLOCKED)
#define AUDIT_PIPE_LIST_WUNLOCK()       rw_wunlock(&audit_pipe_lock)

/*
 * Cloning related variables and constants.
 */
#define AUDIT_PIPE_NAME         "auditpipe"
#define MAX_AUDIT_PIPES         32
static int audit_pipe_major;

/*
 * dev_t doesn't have a pointer for "softc" data.  So we have to keep track of
 * it with the following global array (indexed by the minor number).
 *
 * XXX We may want to dynamically grow this as needed.
 */
static struct audit_pipe        *audit_pipe_dtab[MAX_AUDIT_PIPES];


/*
 * Special device methods and definition.
 */
static open_close_fcn_t         audit_pipe_open;
static open_close_fcn_t         audit_pipe_close;
static read_write_fcn_t         audit_pipe_read;
static ioctl_fcn_t              audit_pipe_ioctl;
static select_fcn_t             audit_pipe_poll;

static const struct cdevsw audit_pipe_cdevsw = {
	.d_open      =          audit_pipe_open,
	.d_close     =          audit_pipe_close,
	.d_read      =          audit_pipe_read,
	.d_write     =          eno_rdwrt,
	.d_ioctl     =          audit_pipe_ioctl,
	.d_stop      =          eno_stop,
	.d_reset     =          eno_reset,
	.d_ttys      =          NULL,
	.d_select    =          audit_pipe_poll,
	.d_mmap      =          eno_mmap,
	.d_strategy  =          eno_strat,
	.d_type      =          0
};

/*
 * Some global statistics on audit pipes.
 */
static int              audit_pipe_count;       /* Current number of pipes. */
static u_int64_t        audit_pipe_ever;        /* Pipes ever allocated. */
static u_int64_t        audit_pipe_records;     /* Records seen. */
static u_int64_t        audit_pipe_drops;       /* Global record drop count. */

/*
 * Free an audit pipe entry.
 */
static void
audit_pipe_entry_free(struct audit_pipe_entry *ape)
{
	kfree_data(ape->ape_record, ape->ape_record_len);
	kfree_type(struct audit_pipe_entry, ape);
}

/*
 * Find an audit pipe preselection specification for an auid, if any.
 */
static struct audit_pipe_preselect *
audit_pipe_preselect_find(struct audit_pipe *ap, au_id_t auid)
{
	struct audit_pipe_preselect *app;

	AUDIT_PIPE_LOCK_ASSERT(ap);

	TAILQ_FOREACH(app, &ap->ap_preselect_list, app_list) {
		if (app->app_auid == auid) {
			return app;
		}
	}
	return NULL;
}

/*
 * Query the per-pipe mask for a specific auid.
 */
static int
audit_pipe_preselect_get(struct audit_pipe *ap, au_id_t auid,
    au_mask_t *maskp)
{
	struct audit_pipe_preselect *app;
	int error;

	AUDIT_PIPE_LOCK(ap);
	app = audit_pipe_preselect_find(ap, auid);
	if (app != NULL) {
		*maskp = app->app_mask;
		error = 0;
	} else {
		error = ENOENT;
	}
	AUDIT_PIPE_UNLOCK(ap);
	return error;
}

/*
 * Set the per-pipe mask for a specific auid.  Add a new entry if needed;
 * otherwise, update the current entry.
 */
static void
audit_pipe_preselect_set(struct audit_pipe *ap, au_id_t auid, au_mask_t mask)
{
	struct audit_pipe_preselect *app, *app_new;

	/*
	 * Pessimistically assume that the auid doesn't already have a mask
	 * set, and allocate.  We will free it if it is unneeded.
	 */
	app_new = kalloc_type(struct audit_pipe_preselect, Z_WAITOK | Z_NOFAIL);
	AUDIT_PIPE_LOCK(ap);
	app = audit_pipe_preselect_find(ap, auid);
	if (app == NULL) {
		app = app_new;
		app_new = NULL;
		app->app_auid = auid;
		TAILQ_INSERT_TAIL(&ap->ap_preselect_list, app, app_list);
	}
	app->app_mask = mask;
	AUDIT_PIPE_UNLOCK(ap);
	kfree_type(struct audit_pipe_preselect, app_new);
}

/*
 * Delete a per-auid mask on an audit pipe.
 */
static int
audit_pipe_preselect_delete(struct audit_pipe *ap, au_id_t auid)
{
	struct audit_pipe_preselect *app;
	int error;

	AUDIT_PIPE_LOCK(ap);
	app = audit_pipe_preselect_find(ap, auid);
	if (app != NULL) {
		TAILQ_REMOVE(&ap->ap_preselect_list, app, app_list);
		error = 0;
	} else {
		error = ENOENT;
	}
	AUDIT_PIPE_UNLOCK(ap);
	kfree_type(struct audit_pipe_preselect, app);
	return error;
}

/*
 * Delete all per-auid masks on an audit pipe.
 */
static void
audit_pipe_preselect_flush_locked(struct audit_pipe *ap)
{
	struct audit_pipe_preselect *app;

	AUDIT_PIPE_LOCK_ASSERT(ap);

	while ((app = TAILQ_FIRST(&ap->ap_preselect_list)) != NULL) {
		TAILQ_REMOVE(&ap->ap_preselect_list, app, app_list);
		kfree_type(struct audit_pipe_preselect, app);
	}
}

static void
audit_pipe_preselect_flush(struct audit_pipe *ap)
{
	AUDIT_PIPE_LOCK(ap);
	audit_pipe_preselect_flush_locked(ap);
	AUDIT_PIPE_UNLOCK(ap);
}

/*-
 * Determine whether a specific audit pipe matches a record with these
 * properties.  Algorithm is as follows:
 *
 * - If the pipe is configured to track the default trail configuration, then
 *   use the results of global preselection matching.
 * - If not, search for a specifically configured auid entry matching the
 *   event.  If an entry is found, use that.
 * - Otherwise, use the default flags or naflags configured for the pipe.
 */
static int
audit_pipe_preselect_check(struct audit_pipe *ap, au_id_t auid,
    au_event_t event, au_class_t class, int sorf, int trail_preselect)
{
	struct audit_pipe_preselect *app;

	AUDIT_PIPE_LOCK_ASSERT(ap);

	switch (ap->ap_preselect_mode) {
	case AUDITPIPE_PRESELECT_MODE_TRAIL:
		return trail_preselect;

	case AUDITPIPE_PRESELECT_MODE_LOCAL:
		app = audit_pipe_preselect_find(ap, auid);
		if (app == NULL) {
			if (auid == (uid_t)AU_DEFAUDITID) {
				return au_preselect(event, class,
				           &ap->ap_preselect_naflags, sorf);
			} else {
				return au_preselect(event, class,
				           &ap->ap_preselect_flags, sorf);
			}
		} else {
			return au_preselect(event, class, &app->app_mask,
			           sorf);
		}

	default:
		panic("audit_pipe_preselect_check: mode %d",
		    ap->ap_preselect_mode);
	}

	return 0;
}

/*
 * Determine whether there exists a pipe interested in a record with specific
 * properties.
 */
int
audit_pipe_preselect(au_id_t auid, au_event_t event, au_class_t class,
    int sorf, int trail_preselect)
{
	struct audit_pipe *ap;

	/* Lockless read to avoid acquiring the global lock if not needed. */
	if (TAILQ_EMPTY(&audit_pipe_list)) {
		return 0;
	}

	AUDIT_PIPE_LIST_RLOCK();
	TAILQ_FOREACH(ap, &audit_pipe_list, ap_list) {
		AUDIT_PIPE_LOCK(ap);
		if (audit_pipe_preselect_check(ap, auid, event, class, sorf,
		    trail_preselect)) {
			AUDIT_PIPE_UNLOCK(ap);
			AUDIT_PIPE_LIST_RUNLOCK();
			return 1;
		}
		AUDIT_PIPE_UNLOCK(ap);
	}
	AUDIT_PIPE_LIST_RUNLOCK();
	return 0;
}

/*
 * Append individual record to a queue -- allocate queue-local buffer, and
 * add to the queue.  If the queue is full or we can't allocate memory, drop
 * the newest record.
 */
static void
audit_pipe_append(struct audit_pipe *ap, void *record, u_int record_len)
{
	struct audit_pipe_entry *ape;

	AUDIT_PIPE_LOCK_ASSERT(ap);

	if (ap->ap_qlen >= ap->ap_qlimit) {
		ap->ap_drops++;
		audit_pipe_drops++;
		return;
	}

	ape = kalloc_type(struct audit_pipe_entry, Z_NOWAIT | Z_ZERO);
	if (ape == NULL) {
		ap->ap_drops++;
		audit_pipe_drops++;
		return;
	}

	ape->ape_record = kalloc_data(record_len, Z_NOWAIT);
	if (ape->ape_record == NULL) {
		kfree_type(struct audit_pipe_entry, ape);
		ap->ap_drops++;
		audit_pipe_drops++;
		return;
	}

	bcopy(record, ape->ape_record, record_len);
	ape->ape_record_len = record_len;

	TAILQ_INSERT_TAIL(&ap->ap_queue, ape, ape_queue);
	ap->ap_inserts++;
	ap->ap_qlen++;
	ap->ap_qbyteslen += ape->ape_record_len;
	selwakeup(&ap->ap_selinfo);
	if (ap->ap_flags & AUDIT_PIPE_ASYNC) {
		pgsigio(ap->ap_sigio, SIGIO);
	}
#if 0  /* XXX - fix select */
	selwakeuppri(&ap->ap_selinfo, PSOCK);
	KNOTE_LOCKED(&ap->ap_selinfo.si_note, 0);
	if (ap->ap_flags & AUDIT_PIPE_ASYNC) {
		pgsigio(&ap->ap_sigio, SIGIO, 0);
	}
#endif
	cv_broadcast(&ap->ap_cv);
}

/*
 * audit_pipe_submit(): audit_worker submits audit records via this
 * interface, which arranges for them to be delivered to pipe queues.
 */
void
audit_pipe_submit(au_id_t auid, au_event_t event, au_class_t class, int sorf,
    int trail_select, void *record, u_int record_len)
{
	struct audit_pipe *ap;

	/*
	 * Lockless read to avoid lock overhead if pipes are not in use.
	 */
	if (TAILQ_FIRST(&audit_pipe_list) == NULL) {
		return;
	}

	AUDIT_PIPE_LIST_RLOCK();
	TAILQ_FOREACH(ap, &audit_pipe_list, ap_list) {
		AUDIT_PIPE_LOCK(ap);
		if (audit_pipe_preselect_check(ap, auid, event, class, sorf,
		    trail_select)) {
			audit_pipe_append(ap, record, record_len);
		}
		AUDIT_PIPE_UNLOCK(ap);
	}
	AUDIT_PIPE_LIST_RUNLOCK();

	/* Unlocked increment. */
	audit_pipe_records++;
}

/*
 * audit_pipe_submit_user(): the same as audit_pipe_submit(), except that
 * since we don't currently have selection information available, it is
 * delivered to the pipe unconditionally.
 *
 * XXXRW: This is a bug.  The BSM check routine for submitting a user record
 * should parse that information and return it.
 */
void
audit_pipe_submit_user(void *record, u_int record_len)
{
	struct audit_pipe *ap;

	/*
	 * Lockless read to avoid lock overhead if pipes are not in use.
	 */
	if (TAILQ_FIRST(&audit_pipe_list) == NULL) {
		return;
	}

	AUDIT_PIPE_LIST_RLOCK();
	TAILQ_FOREACH(ap, &audit_pipe_list, ap_list) {
		AUDIT_PIPE_LOCK(ap);
		audit_pipe_append(ap, record, record_len);
		AUDIT_PIPE_UNLOCK(ap);
	}
	AUDIT_PIPE_LIST_RUNLOCK();

	/* Unlocked increment. */
	audit_pipe_records++;
}

/*
 * Allocate a new audit pipe.  Connects the pipe, on success, to the global
 * list and updates statistics.
 */
static struct audit_pipe *
audit_pipe_alloc(void)
{
	struct audit_pipe *ap;

	AUDIT_PIPE_LIST_WLOCK_ASSERT();

	ap = kalloc_type(struct audit_pipe, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	ap->ap_qlimit = AUDIT_PIPE_QLIMIT_DEFAULT;
	TAILQ_INIT(&ap->ap_queue);
#ifndef  __APPLE__
	knlist_init(&ap->ap_selinfo.si_note, AUDIT_PIPE_MTX(ap), NULL, NULL,
	    NULL);
#endif
	AUDIT_PIPE_LOCK_INIT(ap);
	AUDIT_PIPE_SX_LOCK_INIT(ap);
	cv_init(&ap->ap_cv, "audit_pipe");

	/*
	 * Default flags, naflags, and auid-specific preselection settings to
	 * 0.  Initialize the mode to the global trail so that if praudit(1)
	 * is run on /dev/auditpipe, it sees events associated with the
	 * default trail.  Pipe-aware application can clear the flag, set
	 * custom masks, and flush the pipe as needed.
	 */
	bzero(&ap->ap_preselect_flags, sizeof(ap->ap_preselect_flags));
	bzero(&ap->ap_preselect_naflags, sizeof(ap->ap_preselect_naflags));
	TAILQ_INIT(&ap->ap_preselect_list);
	ap->ap_preselect_mode = AUDITPIPE_PRESELECT_MODE_TRAIL;

	/*
	 * Add to global list and update global statistics.
	 */
	TAILQ_INSERT_HEAD(&audit_pipe_list, ap, ap_list);
	audit_pipe_count++;
	audit_pipe_ever++;

	return ap;
}

/*
 * Flush all records currently present in an audit pipe; assume mutex is held.
 */
static void
audit_pipe_flush(struct audit_pipe *ap)
{
	struct audit_pipe_entry *ape;

	AUDIT_PIPE_LOCK_ASSERT(ap);

	while ((ape = TAILQ_FIRST(&ap->ap_queue)) != NULL) {
		TAILQ_REMOVE(&ap->ap_queue, ape, ape_queue);
		ap->ap_qbyteslen -= ape->ape_record_len;
		audit_pipe_entry_free(ape);
		ap->ap_qlen--;
	}
	ap->ap_qoffset = 0;

	KASSERT(ap->ap_qlen == 0, ("audit_pipe_free: ap_qbyteslen"));
	KASSERT(ap->ap_qbyteslen == 0, ("audit_pipe_flush: ap_qbyteslen"));
}

/*
 * Free an audit pipe; this means freeing all preselection state and all
 * records in the pipe.  Assumes global write lock and pipe mutex are held to
 * revent any new records from being inserted during the free, and that the
 * audit pipe is still on the global list.
 */
static void
audit_pipe_free(struct audit_pipe *ap)
{
	AUDIT_PIPE_LIST_WLOCK_ASSERT();
	AUDIT_PIPE_LOCK_ASSERT(ap);

	audit_pipe_preselect_flush_locked(ap);
	audit_pipe_flush(ap);
	cv_destroy(&ap->ap_cv);
	AUDIT_PIPE_SX_LOCK_DESTROY(ap);
	AUDIT_PIPE_UNLOCK(ap);
	AUDIT_PIPE_LOCK_DESTROY(ap);
#ifndef  __APPLE__
	knlist_destroy(&ap->ap_selinfo.si_note);
#endif
	TAILQ_REMOVE(&audit_pipe_list, ap, ap_list);
	kfree_type(struct audit_pipe, ap);
	audit_pipe_count--;
}

/*
 * Audit pipe clone routine -- provides a new minor number, or to return (-1),
 * if one can't be provided.  Called with DEVFS_LOCK held.
 */
static int
audit_pipe_clone(__unused dev_t dev, int action)
{
	int i;

	if (action == DEVFS_CLONE_ALLOC) {
		for (i = 0; i < MAX_AUDIT_PIPES; i++) {
			if (audit_pipe_dtab[i] == NULL) {
				return i;
			}
		}

		/*
		 * XXX Should really return -1 here but that seems to hang
		 * things in devfs.  Instead return 0 and let _open() tell
		 * userland the bad news.
		 */
		return 0;
	}

	return -1;
}

/*
 * Audit pipe open method.  Explicit privilege check isn't used as this
 * allows file permissions on the special device to be used to grant audit
 * review access.  Those file permissions should be managed carefully.
 */
static int
audit_pipe_open(dev_t dev, __unused int flags, __unused int devtype,
    __unused proc_t p)
{
	struct audit_pipe *ap;
	int u;

	u = minor(dev);
	if (u < 0 || u >= MAX_AUDIT_PIPES) {
		return ENXIO;
	}

	AUDIT_PIPE_LIST_WLOCK();
	ap = audit_pipe_dtab[u];
	if (ap == NULL) {
		ap = audit_pipe_alloc();
		if (ap == NULL) {
			AUDIT_PIPE_LIST_WUNLOCK();
			return ENOMEM;
		}
		audit_pipe_dtab[u] = ap;
	} else {
		KASSERT(ap->ap_open, ("audit_pipe_open: ap && !ap_open"));
		AUDIT_PIPE_LIST_WUNLOCK();
		return EBUSY;
	}
	ap->ap_open = 1;
	AUDIT_PIPE_LIST_WUNLOCK();
#ifndef __APPLE__
	proc_getpid(fsetown(td->td_proc), &ap->ap_sigio);
#endif
	return 0;
}

/*
 * Close audit pipe, tear down all records, etc.
 */
static int
audit_pipe_close(dev_t dev, __unused int flags, __unused int devtype,
    __unused proc_t p)
{
	struct audit_pipe *ap;
	int u;

	u = minor(dev);
	ap = audit_pipe_dtab[u];
	KASSERT(ap != NULL, ("audit_pipe_close: ap == NULL"));
	KASSERT(ap->ap_open, ("audit_pipe_close: !ap_open"));

#ifndef __APPLE__
	funsetown(&ap->ap_sigio);
#endif
	AUDIT_PIPE_LIST_WLOCK();
	AUDIT_PIPE_LOCK(ap);
	ap->ap_open = 0;
	audit_pipe_free(ap);
	audit_pipe_dtab[u] = NULL;
	AUDIT_PIPE_LIST_WUNLOCK();
	return 0;
}

/*
 * Audit pipe ioctl() routine.  Handle file descriptor and audit pipe layer
 * commands.
 */
static int
audit_pipe_ioctl(dev_t dev, u_long cmd, caddr_t data,
    __unused int flag, __unused proc_t p)
{
	struct auditpipe_ioctl_preselect *aip;
	struct audit_pipe *ap;
	au_mask_t *maskp;
	int error, mode;
	au_id_t auid;

	ap = audit_pipe_dtab[minor(dev)];
	KASSERT(ap != NULL, ("audit_pipe_ioctl: ap == NULL"));

	/*
	 * Audit pipe ioctls: first come standard device node ioctls, then
	 * manipulation of pipe settings, and finally, statistics query
	 * ioctls.
	 */
	switch (cmd) {
	case FIONBIO:
		AUDIT_PIPE_LOCK(ap);
		if (*(int *)data) {
			ap->ap_flags |= AUDIT_PIPE_NBIO;
		} else {
			ap->ap_flags &= ~AUDIT_PIPE_NBIO;
		}
		AUDIT_PIPE_UNLOCK(ap);
		error = 0;
		break;

	case FIONREAD:
		AUDIT_PIPE_LOCK(ap);
		*(int *)data = ap->ap_qbyteslen - ap->ap_qoffset;
		AUDIT_PIPE_UNLOCK(ap);
		error = 0;
		break;

	case FIOASYNC:
		AUDIT_PIPE_LOCK(ap);
		if (*(int *)data) {
			ap->ap_flags |= AUDIT_PIPE_ASYNC;
		} else {
			ap->ap_flags &= ~AUDIT_PIPE_ASYNC;
		}
		AUDIT_PIPE_UNLOCK(ap);
		error = 0;
		break;

#ifndef __APPLE__
	case FIOSETOWN:
		error = fsetown(*(int *)data, &ap->ap_sigio);
		break;

	case FIOGETOWN:
		*(int *)data = fgetown(&ap->ap_sigio);
		error = 0;
		break;
#endif /* !__APPLE__ */

	case AUDITPIPE_GET_QLEN:
		*(u_int *)data = ap->ap_qlen;
		error = 0;
		break;

	case AUDITPIPE_GET_QLIMIT:
		*(u_int *)data = ap->ap_qlimit;
		error = 0;
		break;

	case AUDITPIPE_SET_QLIMIT:
		/* Lockless integer write. */
		if (*(u_int *)data >= AUDIT_PIPE_QLIMIT_MIN ||
		    *(u_int *)data <= AUDIT_PIPE_QLIMIT_MAX) {
			ap->ap_qlimit = *(u_int *)data;
			error = 0;
		} else {
			error = EINVAL;
		}
		break;

	case AUDITPIPE_GET_QLIMIT_MIN:
		*(u_int *)data = AUDIT_PIPE_QLIMIT_MIN;
		error = 0;
		break;

	case AUDITPIPE_GET_QLIMIT_MAX:
		*(u_int *)data = AUDIT_PIPE_QLIMIT_MAX;
		error = 0;
		break;

	case AUDITPIPE_GET_PRESELECT_FLAGS:
		AUDIT_PIPE_LOCK(ap);
		maskp = (au_mask_t *)data;
		*maskp = ap->ap_preselect_flags;
		AUDIT_PIPE_UNLOCK(ap);
		error = 0;
		break;

	case AUDITPIPE_SET_PRESELECT_FLAGS:
		AUDIT_PIPE_LOCK(ap);
		maskp = (au_mask_t *)data;
		ap->ap_preselect_flags = *maskp;
		AUDIT_CHECK_IF_KEVENTS_MASK(ap->ap_preselect_flags);
		AUDIT_PIPE_UNLOCK(ap);
		error = 0;
		break;

	case AUDITPIPE_GET_PRESELECT_NAFLAGS:
		AUDIT_PIPE_LOCK(ap);
		maskp = (au_mask_t *)data;
		*maskp = ap->ap_preselect_naflags;
		AUDIT_PIPE_UNLOCK(ap);
		error = 0;
		break;

	case AUDITPIPE_SET_PRESELECT_NAFLAGS:
		AUDIT_PIPE_LOCK(ap);
		maskp = (au_mask_t *)data;
		ap->ap_preselect_naflags = *maskp;
		AUDIT_CHECK_IF_KEVENTS_MASK(ap->ap_preselect_naflags);
		AUDIT_PIPE_UNLOCK(ap);
		error = 0;
		break;

	case AUDITPIPE_GET_PRESELECT_AUID:
		aip = (struct auditpipe_ioctl_preselect *)data;
		error = audit_pipe_preselect_get(ap, aip->aip_auid,
		    &aip->aip_mask);
		break;

	case AUDITPIPE_SET_PRESELECT_AUID:
		aip = (struct auditpipe_ioctl_preselect *)data;
		audit_pipe_preselect_set(ap, aip->aip_auid, aip->aip_mask);
		error = 0;
		break;

	case AUDITPIPE_DELETE_PRESELECT_AUID:
		auid = *(au_id_t *)data;
		error = audit_pipe_preselect_delete(ap, auid);
		break;

	case AUDITPIPE_FLUSH_PRESELECT_AUID:
		audit_pipe_preselect_flush(ap);
		error = 0;
		break;

	case AUDITPIPE_GET_PRESELECT_MODE:
		AUDIT_PIPE_LOCK(ap);
		*(int *)data = ap->ap_preselect_mode;
		AUDIT_PIPE_UNLOCK(ap);
		error = 0;
		break;

	case AUDITPIPE_SET_PRESELECT_MODE:
		mode = *(int *)data;
		switch (mode) {
		case AUDITPIPE_PRESELECT_MODE_TRAIL:
		case AUDITPIPE_PRESELECT_MODE_LOCAL:
			AUDIT_PIPE_LOCK(ap);
			ap->ap_preselect_mode = mode;
			AUDIT_PIPE_UNLOCK(ap);
			error = 0;
			break;

		default:
			error = EINVAL;
		}
		break;

	case AUDITPIPE_FLUSH:
		if (AUDIT_PIPE_SX_XLOCK_SIG(ap) != 0) {
			return EINTR;
		}
		AUDIT_PIPE_LOCK(ap);
		audit_pipe_flush(ap);
		AUDIT_PIPE_UNLOCK(ap);
		AUDIT_PIPE_SX_XUNLOCK(ap);
		error = 0;
		break;

	case AUDITPIPE_GET_MAXAUDITDATA:
		*(u_int *)data = MAXAUDITDATA;
		error = 0;
		break;

	case AUDITPIPE_GET_INSERTS:
		*(u_int *)data = ap->ap_inserts;
		error = 0;
		break;

	case AUDITPIPE_GET_READS:
		*(u_int *)data = ap->ap_reads;
		error = 0;
		break;

	case AUDITPIPE_GET_DROPS:
		*(u_int *)data = ap->ap_drops;
		error = 0;
		break;

	case AUDITPIPE_GET_TRUNCATES:
		*(u_int *)data = 0;
		error = 0;
		break;

	default:
		error = ENOTTY;
	}
	return error;
}

/*
 * Audit pipe read.  Read one or more partial or complete records to user
 * memory.
 */
static int
audit_pipe_read(dev_t dev, struct uio *uio, __unused int flag)
{
	struct audit_pipe_entry *ape;
	struct audit_pipe *ap;
	u_int toread;
	int error;

	ap = audit_pipe_dtab[minor(dev)];
	KASSERT(ap != NULL, ("audit_pipe_read: ap == NULL"));

	/*
	 * We hold an sleep lock over read and flush because we rely on the
	 * stability of a record in the queue during uiomove(9).
	 */
	if (AUDIT_PIPE_SX_XLOCK_SIG(ap) != 0) {
		return EINTR;
	}
	AUDIT_PIPE_LOCK(ap);
	while (TAILQ_EMPTY(&ap->ap_queue)) {
		if (ap->ap_flags & AUDIT_PIPE_NBIO) {
			AUDIT_PIPE_UNLOCK(ap);
			AUDIT_PIPE_SX_XUNLOCK(ap);
			return EAGAIN;
		}
		error = cv_wait_sig(&ap->ap_cv, AUDIT_PIPE_MTX(ap));
		if (error) {
			AUDIT_PIPE_UNLOCK(ap);
			AUDIT_PIPE_SX_XUNLOCK(ap);
			return error;
		}
	}

	/*
	 * Copy as many remaining bytes from the current record to userspace
	 * as we can.  Keep processing records until we run out of records in
	 * the queue, or until the user buffer runs out of space.
	 *
	 * Note: we rely on the sleep lock to maintain ape's stability here.
	 */
	ap->ap_reads++;
	while ((ape = TAILQ_FIRST(&ap->ap_queue)) != NULL &&
	    uio_resid(uio) > 0) {
		AUDIT_PIPE_LOCK_ASSERT(ap);

		KASSERT(ape->ape_record_len > ap->ap_qoffset,
		    ("audit_pipe_read: record_len > qoffset (1)"));
		toread = MIN((int)(ape->ape_record_len - ap->ap_qoffset),
		    uio_resid(uio));
		AUDIT_PIPE_UNLOCK(ap);
		error = uiomove((char *)ape->ape_record + ap->ap_qoffset,
		    toread, uio);
		if (error) {
			AUDIT_PIPE_SX_XUNLOCK(ap);
			return error;
		}

		/*
		 * If the copy succeeded, update book-keeping, and if no
		 * bytes remain in the current record, free it.
		 */
		AUDIT_PIPE_LOCK(ap);
		KASSERT(TAILQ_FIRST(&ap->ap_queue) == ape,
		    ("audit_pipe_read: queue out of sync after uiomove"));
		ap->ap_qoffset += toread;
		KASSERT(ape->ape_record_len >= ap->ap_qoffset,
		    ("audit_pipe_read: record_len >= qoffset (2)"));
		if (ap->ap_qoffset == ape->ape_record_len) {
			TAILQ_REMOVE(&ap->ap_queue, ape, ape_queue);
			ap->ap_qbyteslen -= ape->ape_record_len;
			audit_pipe_entry_free(ape);
			ap->ap_qlen--;
			ap->ap_qoffset = 0;
		}
	}
	AUDIT_PIPE_UNLOCK(ap);
	AUDIT_PIPE_SX_XUNLOCK(ap);
	return 0;
}

/*
 * Audit pipe poll.
 */
static int
audit_pipe_poll(dev_t dev, int events, void *wql, struct proc *p)
{
	struct audit_pipe *ap;
	int revents;

	revents = 0;
	ap = audit_pipe_dtab[minor(dev)];
	KASSERT(ap != NULL, ("audit_pipe_poll: ap == NULL"));

	if (events & (POLLIN | POLLRDNORM)) {
		AUDIT_PIPE_LOCK(ap);
		if (TAILQ_FIRST(&ap->ap_queue) != NULL) {
			revents |= events & (POLLIN | POLLRDNORM);
		} else {
			selrecord(p, &ap->ap_selinfo, wql);
		}
		AUDIT_PIPE_UNLOCK(ap);
	}
	return revents;
}

static void *devnode;

int
audit_pipe_init(void)
{
	dev_t dev;

	TAILQ_INIT(&audit_pipe_list);
	AUDIT_PIPE_LIST_LOCK_INIT();

	audit_pipe_major = cdevsw_add(-1, &audit_pipe_cdevsw);
	if (audit_pipe_major < 0) {
		return KERN_FAILURE;
	}

	dev = makedev(audit_pipe_major, 0);
	devnode = devfs_make_node_clone(dev, DEVFS_CHAR, UID_ROOT, GID_WHEEL,
	    0600, audit_pipe_clone, "auditpipe", 0);

	if (devnode == NULL) {
		return KERN_FAILURE;
	}

	return KERN_SUCCESS;
}

int
audit_pipe_shutdown(void)
{
	/* unwind everything */
	devfs_remove(devnode);
	(void) cdevsw_remove(audit_pipe_major, &audit_pipe_cdevsw);

	return KERN_SUCCESS;
}

#endif /* CONFIG_AUDIT */
