/*
 * Copyright (c) 2000-2019 Apple Inc. All rights reserved.
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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)subr_log.c	8.3 (Berkeley) 2/14/95
 */

/*
 * Error log buffer for kernel printf's.
 */

#include <machine/atomic.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc_internal.h>
#include <sys/vnode.h>
#include <stdbool.h>
#include <firehose/tracepoint_private.h>
#include <firehose/chunk_private.h>
#include <firehose/ioctl_private.h>
#include <os/firehose_buffer_private.h>

#include <os/log_private.h>
#include <sys/ioctl.h>
#include <sys/msgbuf.h>
#include <sys/file_internal.h>
#include <sys/errno.h>
#include <sys/select.h>
#include <sys/kernel.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/simple_lock.h>
#include <sys/lock.h>
#include <sys/signalvar.h>
#include <sys/conf.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>
#include <mach/mach_port.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <vm/vm_kern.h>
#include <kern/task.h>
#include <kern/locks.h>

extern void logwakeup(struct msgbuf *);
extern void oslogwakeup(void);
extern void oslog_streamwakeup(void);
extern bool os_log_disabled(void);

SECURITY_READ_ONLY_LATE(vm_offset_t) kernel_firehose_addr = 0;
SECURITY_READ_ONLY_LATE(uint8_t) __firehose_buffer_kernel_chunk_count =
    FIREHOSE_BUFFER_KERNEL_DEFAULT_CHUNK_COUNT;
SECURITY_READ_ONLY_LATE(uint8_t) __firehose_num_kernel_io_pages =
    FIREHOSE_BUFFER_KERNEL_DEFAULT_IO_PAGES;

uint32_t oslog_msgbuf_dropped_charcount = 0;

/* Counters for streaming mode */
SCALABLE_COUNTER_DEFINE(oslog_s_total_msgcount);
SCALABLE_COUNTER_DEFINE(oslog_s_metadata_msgcount);
SCALABLE_COUNTER_DEFINE(oslog_s_streamed_msgcount);
SCALABLE_COUNTER_DEFINE(oslog_s_dropped_msgcount);
SCALABLE_COUNTER_DEFINE(oslog_s_error_count);

#define LOG_RDPRI       (PZERO + 1)

#define LOG_NBIO        0x02
#define LOG_ASYNC       0x04
#define LOG_RDWAIT      0x08

/* All globals should be accessed under bsd_log_lock() or bsd_log_lock_safe() */

static char amsg_bufc[1024];
static struct msgbuf aslbuf = {.msg_magic = MSG_MAGIC, .msg_size = sizeof(amsg_bufc), .msg_bufx = 0, .msg_bufr = 0, .msg_bufc = amsg_bufc};
struct msgbuf *aslbufp __attribute__((used)) = &aslbuf;

/* logsoftc only valid while log_open=1 */
struct logsoftc {
	int     sc_state;               /* see above for possibilities */
	struct  selinfo sc_selp;        /* thread waiting for select */
	int     sc_pgid;                /* process/group for async I/O */
	struct msgbuf *sc_mbp;
} logsoftc;

static int log_open;
char smsg_bufc[CONFIG_MSG_BSIZE]; /* static buffer */
struct firehose_chunk_s oslog_boot_buf = {
	.fc_pos = {
		.fcp_next_entry_offs = offsetof(struct firehose_chunk_s, fc_data),
		.fcp_private_offs = FIREHOSE_CHUNK_SIZE,
		.fcp_refcnt = 1, // indicate that there is a writer to this chunk
		.fcp_stream = firehose_stream_persist,
		.fcp_flag_io = 1, // for now, lets assume this is coming from the io bank
	},
}; /* static buffer */
firehose_chunk_t firehose_boot_chunk = &oslog_boot_buf;
struct msgbuf msgbuf = {.msg_magic  = MSG_MAGIC, .msg_size = sizeof(smsg_bufc), .msg_bufx = 0, .msg_bufr = 0, .msg_bufc = smsg_bufc};
struct msgbuf oslog_stream_buf = {.msg_magic = MSG_MAGIC, .msg_size = 0, .msg_bufx = 0, .msg_bufr = 0, .msg_bufc = NULL};
struct msgbuf *msgbufp __attribute__((used)) = &msgbuf;
struct msgbuf *oslog_streambufp __attribute__((used)) = &oslog_stream_buf;

// List entries for keeping track of the streaming buffer
static oslog_stream_buf_entry_t oslog_stream_buf_entries;

#define OSLOG_NUM_STREAM_ENTRIES        64
#define OSLOG_STREAM_BUF_SIZE           4096
#define OSLOG_STREAM_MAX_BUF_SIZE       (1024 * 1024)

int     oslog_open = 0;
bool    os_log_wakeup = false;
bool    oslog_stream_open = false;
int     oslog_stream_buf_bytesavail = 0;
int     oslog_stream_buf_size = OSLOG_STREAM_BUF_SIZE;
int     oslog_stream_num_entries = OSLOG_NUM_STREAM_ENTRIES;

/* oslogsoftc only valid while oslog_open=1 */
struct oslogsoftc {
	int     sc_state;               /* see above for possibilities */
	struct  selinfo sc_selp;        /* thread waiting for select */
	int     sc_pgid;                /* process/group for async I/O */
} oslogsoftc;

struct oslog_streamsoftc {
	int     sc_state;               /* see above for possibilities */
	struct  selinfo sc_selp;        /* thread waiting for select */
	int     sc_pgid;                /* process/group for async I/O */
} oslog_streamsoftc;

STAILQ_HEAD(, oslog_stream_buf_entry_s) oslog_stream_free_head =
    STAILQ_HEAD_INITIALIZER(oslog_stream_free_head);
STAILQ_HEAD(, oslog_stream_buf_entry_s) oslog_stream_buf_head =
    STAILQ_HEAD_INITIALIZER(oslog_stream_buf_head);

/* defined in osfmk/kern/printf.c  */
extern bool bsd_log_lock(bool);
extern void bsd_log_lock_safe(void);
extern void bsd_log_unlock(void);

LCK_GRP_DECLARE(oslog_stream_lock_grp, "oslog streaming");
LCK_SPIN_DECLARE(oslog_stream_lock, &oslog_stream_lock_grp);
#define stream_lock() lck_spin_lock(&oslog_stream_lock)
#define stream_unlock() lck_spin_unlock(&oslog_stream_lock)

/* XXX wants a linker set so these can be static */
extern d_open_t         logopen;
extern d_close_t        logclose;
extern d_read_t         logread;
extern d_ioctl_t        logioctl;
extern d_select_t       logselect;

/* XXX wants a linker set so these can be static */
extern d_open_t         oslogopen;
extern d_close_t        oslogclose;
extern d_select_t       oslogselect;
extern d_ioctl_t        oslogioctl;

/* XXX wants a linker set so these can be static */
extern d_open_t         oslog_streamopen;
extern d_close_t        oslog_streamclose;
extern d_read_t         oslog_streamread;
extern d_ioctl_t        oslog_streamioctl;
extern d_select_t       oslog_streamselect;

void oslog_stream(bool is_metadata, firehose_tracepoint_id_u ftid, uint64_t stamp,
    const void *data, size_t datalen);

/*
 * Serialize log access.  Note that the log can be written at interrupt level,
 * so any log manipulations that can be done from, or affect, another processor
 * at interrupt level must be guarded with a spin lock.
 */

static int sysctl_kern_msgbuf(struct sysctl_oid *oidp,
    void *arg1, int arg2, struct sysctl_req *req);

/*ARGSUSED*/
int
logopen(__unused dev_t dev, __unused int flags, __unused int mode, struct proc *p)
{
	bsd_log_lock_safe();
	if (log_open) {
		bsd_log_unlock();
		return EBUSY;
	}
	if (atm_get_diagnostic_config() & ATM_ENABLE_LEGACY_LOGGING) {
		logsoftc.sc_mbp = msgbufp;
	} else {
		/*
		 * Support for messagetracer (kern_asl_msg())
		 * In this mode, /dev/klog exports only ASL-formatted messages
		 * written into aslbufp via vaddlog().
		 */
		logsoftc.sc_mbp = aslbufp;
	}
	logsoftc.sc_pgid = proc_getpid(p);            /* signal process only */
	log_open = 1;

	bsd_log_unlock();

	return 0;
}

/*ARGSUSED*/
int
logclose(__unused dev_t dev, __unused int flag, __unused int devtype, __unused struct proc *p)
{
	bsd_log_lock_safe();
	logsoftc.sc_state &= ~(LOG_NBIO | LOG_ASYNC);
	selthreadclear(&logsoftc.sc_selp);
	log_open = 0;
	bsd_log_unlock();
	return 0;
}


int
oslogopen(__unused dev_t dev, __unused int flags, __unused int mode, struct proc *p)
{
	bsd_log_lock_safe();
	if (oslog_open) {
		bsd_log_unlock();
		return EBUSY;
	}
	oslogsoftc.sc_pgid = proc_getpid(p);          /* signal process only */
	oslog_open = 1;

	bsd_log_unlock();
	return 0;
}

int
oslogclose(__unused dev_t dev, __unused int flag, __unused int devtype, __unused struct proc *p)
{
	bsd_log_lock_safe();
	oslogsoftc.sc_state &= ~(LOG_NBIO | LOG_ASYNC);
	selthreadclear(&oslogsoftc.sc_selp);
	oslog_open = 0;
	bsd_log_unlock();
	return 0;
}

static void
oslog_streamwakeup_locked(void)
{
	LCK_SPIN_ASSERT(&oslog_stream_lock, LCK_ASSERT_OWNED);
	if (!oslog_stream_open) {
		return;
	}
	selwakeup(&oslog_streamsoftc.sc_selp);
	if (oslog_streamsoftc.sc_state & LOG_RDWAIT) {
		wakeup((caddr_t)oslog_streambufp);
		oslog_streamsoftc.sc_state &= ~LOG_RDWAIT;
	}
}

int
oslog_streamopen(__unused dev_t dev, __unused int flags, __unused int mode, struct proc *p)
{
	char *oslog_stream_msg_bufc = NULL;
	oslog_stream_buf_entry_t entries = NULL;

	stream_lock();
	if (oslog_stream_open) {
		stream_unlock();
		return EBUSY;
	}
	stream_unlock();

	// Allocate the stream buffer
	oslog_stream_msg_bufc = kalloc_data(oslog_stream_buf_size,
	    Z_WAITOK | Z_ZERO);
	if (!oslog_stream_msg_bufc) {
		return ENOMEM;
	}

	/* entries to support kernel logging in stream mode */
	/* Zeroing to avoid copying uninitialized struct padding to userspace. */
	entries = kalloc_type(struct oslog_stream_buf_entry_s,
	    oslog_stream_num_entries, Z_WAITOK | Z_ZERO);
	if (!entries) {
		kfree_data(oslog_stream_msg_bufc, oslog_stream_buf_size);
		return ENOMEM;
	}

	stream_lock();
	if (oslog_stream_open) {
		stream_unlock();
		kfree_data(oslog_stream_msg_bufc, oslog_stream_buf_size);
		kfree_type(struct oslog_stream_buf_entry_s,
		    oslog_stream_num_entries, entries);
		return EBUSY;
	}

	assert(oslog_streambufp->msg_bufc == NULL);
	oslog_streambufp->msg_bufc = oslog_stream_msg_bufc;
	oslog_streambufp->msg_size = oslog_stream_buf_size;

	oslog_stream_buf_entries = entries;

	STAILQ_INIT(&oslog_stream_free_head);
	STAILQ_INIT(&oslog_stream_buf_head);

	for (int i = 0; i < oslog_stream_num_entries; i++) {
		oslog_stream_buf_entries[i].type = oslog_stream_link_type_log;
		STAILQ_INSERT_TAIL(&oslog_stream_free_head, &oslog_stream_buf_entries[i], buf_entries);
	}

	/* there should be no pending entries in the stream */
	assert(STAILQ_EMPTY(&oslog_stream_buf_head));
	assert(oslog_streambufp->msg_bufx == 0);
	assert(oslog_streambufp->msg_bufr == 0);

	oslog_streambufp->msg_bufx = 0;
	oslog_streambufp->msg_bufr = 0;
	oslog_streamsoftc.sc_pgid = proc_getpid(p); /* signal process only */
	oslog_stream_open = true;
	oslog_stream_buf_bytesavail = oslog_stream_buf_size;
	stream_unlock();

	return 0;
}

static oslog_stream_buf_entry_t
stream_metadata_create(firehose_tracepoint_id_u ftid, uint64_t stamp, const void *data, size_t data_len)
{
	const size_t ft_size = sizeof(struct firehose_tracepoint_s) + data_len;

	if (!data || data_len > UINT16_MAX || ft_size > UINT16_MAX) {
		return NULL;
	}

	oslog_stream_buf_entry_t e = kalloc_type(struct oslog_stream_buf_entry_s, Z_WAITOK);
	if (!e) {
		return NULL;
	}

	e->type         = oslog_stream_link_type_metadata;
	e->timestamp    = stamp;
	e->offset       = 0; // always zero for metadata
	e->size         = (uint16_t)ft_size;
	e->metadata     = kalloc_data(e->size, Z_WAITOK | Z_ZERO);

	if (!e->metadata) {
		kfree_type(struct oslog_stream_buf_entry_s, e);
		return NULL;
	}

	firehose_tracepoint_t ft    = e->metadata;
	ft->ft_id.ftid_value        = ftid.ftid_value;
	ft->ft_thread               = thread_tid(current_thread());
	ft->ft_length               = (uint16_t)data_len;
	memcpy(ft->ft_data, data, ft->ft_length);

	return e;
}

static void
stream_metadata_free(oslog_stream_buf_entry_t e)
{
	kfree_data(e->metadata, e->size);
	kfree_type(struct oslog_stream_buf_entry_s, e);
}

static void
oslog_streamwrite_metadata_locked(oslog_stream_buf_entry_t m_entry)
{
	LCK_SPIN_ASSERT(&oslog_stream_lock, LCK_ASSERT_OWNED);
	STAILQ_INSERT_TAIL(&oslog_stream_buf_head, m_entry, buf_entries);
}

static void
oslog_streamwrite_append_bytes(const char *buffer, int buflen)
{
	struct msgbuf *mbp;

	LCK_SPIN_ASSERT(&oslog_stream_lock, LCK_ASSERT_OWNED);

	assert(oslog_stream_buf_bytesavail >= buflen);
	oslog_stream_buf_bytesavail -= buflen;
	assert(oslog_stream_buf_bytesavail >= 0);

	mbp = oslog_streambufp;
	if (mbp->msg_bufx + buflen <= mbp->msg_size) {
		/*
		 * If this will fit without needing to be split across the end
		 * of the buffer, copy it directly in one go.
		 */
		memcpy((void *)(mbp->msg_bufc + mbp->msg_bufx), buffer, buflen);

		mbp->msg_bufx += buflen;
		if (mbp->msg_bufx == mbp->msg_size) {
			mbp->msg_bufx = 0;
		}
	} else {
		/*
		 * Copy up to the end of the stream buffer, and then put what remains
		 * at the beginning.
		 */
		int bytes_left = mbp->msg_size - mbp->msg_bufx;
		memcpy((void *)(mbp->msg_bufc + mbp->msg_bufx), buffer, bytes_left);

		buflen -= bytes_left;
		buffer += bytes_left;

		// Copy the remainder of the data from the beginning of stream
		memcpy((void *)mbp->msg_bufc, buffer, buflen);
		mbp->msg_bufx = buflen;
	}
	return;
}

static oslog_stream_buf_entry_t
oslog_stream_find_free_buf_entry_locked(void)
{
	struct msgbuf *mbp;
	oslog_stream_buf_entry_t buf_entry = NULL;

	LCK_SPIN_ASSERT(&oslog_stream_lock, LCK_ASSERT_OWNED);

	mbp = oslog_streambufp;

	buf_entry = STAILQ_FIRST(&oslog_stream_free_head);
	if (buf_entry) {
		STAILQ_REMOVE_HEAD(&oslog_stream_free_head, buf_entries);
	} else {
		// If no list elements are available in the free-list,
		// consume the next log line so we can free up its list element
		oslog_stream_buf_entry_t prev_entry = NULL;

		buf_entry = STAILQ_FIRST(&oslog_stream_buf_head);
		while (buf_entry->type == oslog_stream_link_type_metadata) {
			prev_entry = buf_entry;
			buf_entry = STAILQ_NEXT(buf_entry, buf_entries);
		}

		if (prev_entry == NULL) {
			STAILQ_REMOVE_HEAD(&oslog_stream_buf_head, buf_entries);
		} else {
			STAILQ_REMOVE_AFTER(&oslog_stream_buf_head, prev_entry, buf_entries);
		}

		mbp->msg_bufr += buf_entry->size;
		counter_inc(&oslog_s_dropped_msgcount);
		if (mbp->msg_bufr >= mbp->msg_size) {
			mbp->msg_bufr = (mbp->msg_bufr % mbp->msg_size);
		}
	}

	return buf_entry;
}

static void
oslog_streamwrite_locked(firehose_tracepoint_id_u ftid,
    uint64_t stamp, const void *pubdata, size_t publen)
{
	struct msgbuf *mbp;
	oslog_stream_buf_entry_t buf_entry = NULL;
	oslog_stream_buf_entry_t next_entry = NULL;

	LCK_SPIN_ASSERT(&oslog_stream_lock, LCK_ASSERT_OWNED);

	assert(publen <= UINT16_MAX);
	const ssize_t ft_length = offsetof(struct firehose_tracepoint_s, ft_data) + publen;

	mbp = oslog_streambufp;
	if (ft_length > mbp->msg_size) {
		counter_inc(&oslog_s_error_count);
		counter_inc(&oslog_s_dropped_msgcount);
		return;
	}

	// Ensure that we have a list element for this record
	buf_entry = oslog_stream_find_free_buf_entry_locked();

	assert(buf_entry != NULL);

	while (ft_length > oslog_stream_buf_bytesavail) {
		oslog_stream_buf_entry_t prev_entry = NULL;

		next_entry = STAILQ_FIRST(&oslog_stream_buf_head);
		assert(next_entry != NULL);
		while (next_entry->type == oslog_stream_link_type_metadata) {
			prev_entry = next_entry;
			next_entry = STAILQ_NEXT(next_entry, buf_entries);
		}

		if (prev_entry == NULL) {
			STAILQ_REMOVE_HEAD(&oslog_stream_buf_head, buf_entries);
		} else {
			STAILQ_REMOVE_AFTER(&oslog_stream_buf_head, prev_entry, buf_entries);
		}

		mbp->msg_bufr += next_entry->size;
		if (mbp->msg_bufr >= mbp->msg_size) {
			mbp->msg_bufr = (mbp->msg_bufr % mbp->msg_size);
		}

		counter_inc(&oslog_s_dropped_msgcount);
		oslog_stream_buf_bytesavail += next_entry->size;
		assert(oslog_stream_buf_bytesavail <= oslog_stream_buf_size);

		STAILQ_INSERT_TAIL(&oslog_stream_free_head, next_entry, buf_entries);
	}

	assert(ft_length <= oslog_stream_buf_bytesavail);

	// Write the log line and update the list entry for this record
	buf_entry->offset = mbp->msg_bufx;
	buf_entry->size = (uint16_t)ft_length;
	buf_entry->timestamp = stamp;
	buf_entry->type = oslog_stream_link_type_log;

	// Construct a tracepoint
	struct firehose_tracepoint_s fs = {
		.ft_thread = thread_tid(current_thread()),
		.ft_id.ftid_value = ftid.ftid_value,
		.ft_length = publen
	};

	oslog_streamwrite_append_bytes((char *)&fs, sizeof(fs));
	oslog_streamwrite_append_bytes(pubdata, (int)publen);

	assert(mbp->msg_bufr < mbp->msg_size);
	// Insert the element to the buffer data list
	STAILQ_INSERT_TAIL(&oslog_stream_buf_head, buf_entry, buf_entries);

	return;
}

static void
oslog_stream_metadata(firehose_tracepoint_id_u ftid, uint64_t stamp, const void *data, size_t datalen)
{
	counter_inc(&oslog_s_metadata_msgcount);

	if (!oslog_stream_open) {
		counter_inc(&oslog_s_dropped_msgcount);
		return;
	}

	oslog_stream_buf_entry_t me = stream_metadata_create(ftid, stamp, data, datalen);
	if (!me) {
		counter_inc(&oslog_s_error_count);
		counter_inc(&oslog_s_dropped_msgcount);
		return;
	}

	stream_lock();
	if (!oslog_stream_open) {
		stream_unlock();
		stream_metadata_free(me);
		counter_inc(&oslog_s_dropped_msgcount);
		return;
	}
	oslog_streamwrite_metadata_locked(me);
	stream_unlock();

	oslog_streamwakeup();
}

static void
oslog_stream_data(firehose_tracepoint_id_u ftid, uint64_t stamp, const void *data, size_t datalen)
{
	counter_inc(&oslog_s_total_msgcount);

	if (!oslog_stream_open) {
		counter_inc(&oslog_s_dropped_msgcount);
		return;
	}

	stream_lock();
	if (!oslog_stream_open) {
		stream_unlock();
		counter_inc(&oslog_s_dropped_msgcount);
		return;
	}
	oslog_streamwrite_locked(ftid, stamp, data, datalen);
	stream_unlock();

	oslog_streamwakeup();
}

void
oslog_stream(bool is_metadata, firehose_tracepoint_id_u ftid, uint64_t stamp, const void *data, size_t datalen)
{
	if (is_metadata) {
		oslog_stream_metadata(ftid, stamp, data, datalen);
	} else {
		oslog_stream_data(ftid, stamp, data, datalen);
	}
}

int
oslog_streamclose(__unused dev_t dev, __unused int flag, __unused int devtype, __unused struct proc *p)
{
	stream_lock();

	if (!oslog_stream_open) {
		stream_unlock();
		return EBADF;
	}

	/*
	 * Discard all not consumed logs but free metadata explicitly since
	 * they are allocated by stream_metadata_create() from a heap.
	 */
	STAILQ_HEAD(, oslog_stream_buf_entry_s) metadata = STAILQ_HEAD_INITIALIZER(metadata);

	while (!STAILQ_EMPTY(&oslog_stream_buf_head)) {
		counter_inc(&oslog_s_dropped_msgcount);
		oslog_stream_buf_entry_t e = STAILQ_FIRST(&oslog_stream_buf_head);
		STAILQ_REMOVE_HEAD(&oslog_stream_buf_head, buf_entries);
		if (e->type == oslog_stream_link_type_metadata) {
			STAILQ_INSERT_TAIL(&metadata, e, buf_entries);
		}
	}

	oslog_streamwakeup_locked();
	oslog_streamsoftc.sc_state &= ~(LOG_NBIO | LOG_ASYNC);
	selthreadclear(&oslog_streamsoftc.sc_selp);

	oslog_stream_open = false;

	char *oslog_stream_msg_bufc = oslog_streambufp->msg_bufc;
	oslog_stream_buf_entry_t entries = oslog_stream_buf_entries;

	oslog_streambufp->msg_bufr = 0;
	oslog_streambufp->msg_bufx = 0;
	oslog_streambufp->msg_bufc = NULL;
	oslog_stream_buf_entries = NULL;
	oslog_streambufp->msg_size = 0;

	stream_unlock();

	// Free metadata
	while (!STAILQ_EMPTY(&metadata)) {
		oslog_stream_buf_entry_t e = STAILQ_FIRST(&metadata);
		STAILQ_REMOVE_HEAD(&metadata, buf_entries);
		stream_metadata_free(e);
	}

	// Free the stream buffer
	kheap_free(KHEAP_DATA_BUFFERS, oslog_stream_msg_bufc, oslog_stream_buf_size);
	// Free the list entries
	kfree_type(struct oslog_stream_buf_entry_s, oslog_stream_num_entries, entries);

	return 0;
}

/*ARGSUSED*/
int
logread(__unused dev_t dev, struct uio *uio, int flag)
{
	int error = 0;
	struct msgbuf *mbp = logsoftc.sc_mbp;
	ssize_t resid;

	bsd_log_lock_safe();
	while (mbp->msg_bufr == mbp->msg_bufx) {
		if (flag & IO_NDELAY) {
			error = EWOULDBLOCK;
			goto out;
		}
		if (logsoftc.sc_state & LOG_NBIO) {
			error = EWOULDBLOCK;
			goto out;
		}
		logsoftc.sc_state |= LOG_RDWAIT;
		bsd_log_unlock();
		/*
		 * If the wakeup is missed
		 * then wait for 5 sec and reevaluate
		 */
		if ((error = tsleep((caddr_t)mbp, LOG_RDPRI | PCATCH,
		    "klog", 5 * hz)) != 0) {
			/* if it times out; ignore */
			if (error != EWOULDBLOCK) {
				return error;
			}
		}
		bsd_log_lock_safe();
	}
	logsoftc.sc_state &= ~LOG_RDWAIT;

	while ((resid = uio_resid(uio)) > 0) {
		size_t l;

		if (mbp->msg_bufx >= mbp->msg_bufr) {
			l = mbp->msg_bufx - mbp->msg_bufr;
		} else {
			l = mbp->msg_size - mbp->msg_bufr;
		}
		if ((l = MIN(l, (size_t)resid)) == 0) {
			break;
		}

		const size_t readpos = mbp->msg_bufr;

		bsd_log_unlock();
		error = uiomove((caddr_t)&mbp->msg_bufc[readpos], (int)l, uio);
		bsd_log_lock_safe();
		if (error) {
			break;
		}

		mbp->msg_bufr = (int)(readpos + l);
		if (mbp->msg_bufr >= mbp->msg_size) {
			mbp->msg_bufr = 0;
		}
	}
out:
	bsd_log_unlock();
	return error;
}

/*ARGSUSED*/
int
oslog_streamread(__unused dev_t dev, struct uio *uio, int flag)
{
	int error = 0;
	int copy_size = 0;
	static char logline[FIREHOSE_CHUNK_SIZE];

	stream_lock();

	if (!oslog_stream_open) {
		stream_unlock();
		return EBADF;
	}

	while (STAILQ_EMPTY(&oslog_stream_buf_head)) {
		assert(oslog_stream_buf_bytesavail == oslog_stream_buf_size);

		if (flag & IO_NDELAY || oslog_streamsoftc.sc_state & LOG_NBIO) {
			stream_unlock();
			return EWOULDBLOCK;
		}

		oslog_streamsoftc.sc_state |= LOG_RDWAIT;
		wait_result_t wr = assert_wait((event_t)oslog_streambufp,
		    THREAD_INTERRUPTIBLE);
		if (wr == THREAD_WAITING) {
			stream_unlock();
			wr = thread_block(THREAD_CONTINUE_NULL);
			stream_lock();
		}

		switch (wr) {
		case THREAD_AWAKENED:
		case THREAD_TIMED_OUT:
			break;
		default:
			stream_unlock();
			return EINTR;
		}
	}

	if (!oslog_stream_open) {
		stream_unlock();
		return EBADF;
	}

	int logpos = 0;
	oslog_stream_buf_entry_t read_entry = NULL;
	uint16_t rec_length;

	read_entry = STAILQ_FIRST(&oslog_stream_buf_head);
	assert(read_entry != NULL);
	STAILQ_REMOVE_HEAD(&oslog_stream_buf_head, buf_entries);

	// Copy the timestamp first
	memcpy(logline + logpos, &read_entry->timestamp, sizeof(uint64_t));
	logpos += sizeof(uint64_t);

	switch (read_entry->type) {
	/* Handle metadata messages */
	case oslog_stream_link_type_metadata:
	{
		memcpy(logline + logpos, read_entry->metadata, read_entry->size);
		logpos += read_entry->size;

		stream_unlock();

		stream_metadata_free(read_entry);
		break;
	}
	/* Handle log messages */
	case oslog_stream_link_type_log:
	{
		/* ensure that the correct read entry was dequeued */
		assert(read_entry->offset == oslog_streambufp->msg_bufr);
		rec_length = read_entry->size;

		// If the next log line is contiguous in the buffer, copy it out.
		if (read_entry->offset + rec_length <= oslog_streambufp->msg_size) {
			memcpy(logline + logpos,
			    oslog_streambufp->msg_bufc + read_entry->offset, rec_length);

			oslog_streambufp->msg_bufr += rec_length;
			if (oslog_streambufp->msg_bufr == oslog_streambufp->msg_size) {
				oslog_streambufp->msg_bufr = 0;
			}
			logpos += rec_length;
		} else {
			// Otherwise, copy until the end of the buffer, and
			// copy the remaining bytes starting at index 0.
			int bytes_left = oslog_streambufp->msg_size - read_entry->offset;
			memcpy(logline + logpos,
			    oslog_streambufp->msg_bufc + read_entry->offset, bytes_left);
			logpos += bytes_left;
			rec_length -= bytes_left;

			memcpy(logline + logpos, (const void *)oslog_streambufp->msg_bufc,
			    rec_length);
			oslog_streambufp->msg_bufr = rec_length;
			logpos += rec_length;
		}

		oslog_stream_buf_bytesavail += read_entry->size;
		assert(oslog_stream_buf_bytesavail <= oslog_stream_buf_size);

		assert(oslog_streambufp->msg_bufr < oslog_streambufp->msg_size);
		STAILQ_INSERT_TAIL(&oslog_stream_free_head, read_entry, buf_entries);

		stream_unlock();
		break;
	}
	default:
	{
		panic("Got unexpected log entry type: %hhu", read_entry->type);
	}
	}

	copy_size = min(logpos, (int) MIN(uio_resid(uio), INT_MAX));
	if (copy_size > 0) {
		error = uiomove((caddr_t)logline, copy_size, uio);
	}
	counter_inc(&oslog_s_streamed_msgcount);

	return error;
}

/*ARGSUSED*/
int
logselect(__unused dev_t dev, int rw, void * wql, struct proc *p)
{
	const struct msgbuf *mbp = logsoftc.sc_mbp;

	switch (rw) {
	case FREAD:
		bsd_log_lock_safe();
		if (mbp->msg_bufr != mbp->msg_bufx) {
			bsd_log_unlock();
			return 1;
		}
		selrecord(p, &logsoftc.sc_selp, wql);
		bsd_log_unlock();
		break;
	}
	return 0;
}

int
oslogselect(__unused dev_t dev, int rw, void * wql, struct proc *p)
{
	switch (rw) {
	case FREAD:
		bsd_log_lock_safe();
		if (os_log_wakeup) {
			bsd_log_unlock();
			return 1;
		}
		selrecord(p, &oslogsoftc.sc_selp, wql);
		bsd_log_unlock();
		break;
	}
	return 0;
}

int
oslog_streamselect(__unused dev_t dev, int rw, void * wql, struct proc *p)
{
	int ret = 0;

	stream_lock();

	switch (rw) {
	case FREAD:
		if (STAILQ_EMPTY(&oslog_stream_buf_head)) {
			selrecord(p, &oslog_streamsoftc.sc_selp, wql);
		} else {
			ret = 1;
		}
		break;
	}

	stream_unlock();
	return ret;
}

void
logwakeup(struct msgbuf *mbp)
{
	/* cf. r24974766 & r25201228*/
	if (oslog_is_safe() == FALSE) {
		return;
	}

	bsd_log_lock_safe();
	if (!log_open) {
		bsd_log_unlock();
		return;
	}
	if (NULL == mbp) {
		mbp = logsoftc.sc_mbp;
	}
	if (mbp != logsoftc.sc_mbp) {
		goto out;
	}
	selwakeup(&logsoftc.sc_selp);
	if (logsoftc.sc_state & LOG_ASYNC) {
		int pgid = logsoftc.sc_pgid;
		bsd_log_unlock();
		if (pgid < 0) {
			gsignal(-pgid, SIGIO);
		} else {
			proc_signal(pgid, SIGIO);
		}
		bsd_log_lock_safe();
	}
	if (logsoftc.sc_state & LOG_RDWAIT) {
		wakeup((caddr_t)mbp);
		logsoftc.sc_state &= ~LOG_RDWAIT;
	}
out:
	bsd_log_unlock();
}

void
oslogwakeup(void)
{
	if (!oslog_is_safe()) {
		return;
	}

	bsd_log_lock_safe();
	if (!oslog_open) {
		bsd_log_unlock();
		return;
	}
	selwakeup(&oslogsoftc.sc_selp);
	os_log_wakeup = true;
	bsd_log_unlock();
}

void
oslog_streamwakeup(void)
{
	/* cf. r24974766 & r25201228*/
	if (oslog_is_safe() == FALSE) {
		return;
	}

	stream_lock();
	oslog_streamwakeup_locked();
	stream_unlock();
}

/*ARGSUSED*/
int
logioctl(__unused dev_t dev, u_long com, caddr_t data, __unused int flag, __unused struct proc *p)
{
	int l;
	const struct msgbuf *mbp = logsoftc.sc_mbp;

	bsd_log_lock_safe();
	switch (com) {
	/* return number of characters immediately available */
	case FIONREAD:
		l = mbp->msg_bufx - mbp->msg_bufr;
		if (l < 0) {
			l += mbp->msg_size;
		}
		*(off_t *)data = l;
		break;

	case FIONBIO:
		if (*(int *)data) {
			logsoftc.sc_state |= LOG_NBIO;
		} else {
			logsoftc.sc_state &= ~LOG_NBIO;
		}
		break;

	case FIOASYNC:
		if (*(int *)data) {
			logsoftc.sc_state |= LOG_ASYNC;
		} else {
			logsoftc.sc_state &= ~LOG_ASYNC;
		}
		break;

	case TIOCSPGRP:
		logsoftc.sc_pgid = *(int *)data;
		break;

	case TIOCGPGRP:
		*(int *)data = logsoftc.sc_pgid;
		break;

	default:
		bsd_log_unlock();
		return -1;
	}
	bsd_log_unlock();
	return 0;
}

/*ARGSUSED*/
int
oslogioctl(__unused dev_t dev, u_long com, caddr_t data, __unused int flag, __unused struct proc *p)
{
	int ret = 0;
	mach_vm_size_t buffer_size = (__firehose_buffer_kernel_chunk_count * FIREHOSE_CHUNK_SIZE);
	firehose_buffer_map_info_t map_info = {0, 0};
	firehose_buffer_t kernel_firehose_buffer = NULL;
	mach_vm_address_t user_addr = 0;
	mach_port_t mem_entry_ptr = MACH_PORT_NULL;
	bool has_more;

	switch (com) {
	/* return number of characters immediately available */

	case LOGBUFFERMAP:
		kernel_firehose_buffer = (firehose_buffer_t)kernel_firehose_addr;

		ret = mach_make_memory_entry_64(kernel_map,
		    &buffer_size,
		    (mach_vm_offset_t) kernel_firehose_buffer,
		    (MAP_MEM_VM_SHARE | VM_PROT_READ),
		    &mem_entry_ptr,
		    MACH_PORT_NULL);
		if (ret == KERN_SUCCESS) {
			ret = mach_vm_map_kernel(get_task_map(current_task()),
			    &user_addr,
			    buffer_size,
			    0,               /*  mask */
			    VM_FLAGS_ANYWHERE,
			    VM_MAP_KERNEL_FLAGS_NONE,
			    VM_KERN_MEMORY_NONE,
			    mem_entry_ptr,
			    0,               /* offset */
			    FALSE,               /* copy */
			    VM_PROT_READ,
			    VM_PROT_READ,
			    VM_INHERIT_SHARE);
		}

		if (ret == KERN_SUCCESS) {
			map_info.fbmi_addr = (uint64_t) (user_addr);
			map_info.fbmi_size = buffer_size;
			bcopy(&map_info, data, sizeof(firehose_buffer_map_info_t));
		}
		break;
	case LOGFLUSHED:
		has_more = __firehose_merge_updates(*(firehose_push_reply_t *)(data));
		bsd_log_lock_safe();
		os_log_wakeup = has_more;
		if (os_log_wakeup) {
			selwakeup(&oslogsoftc.sc_selp);
		}
		bsd_log_unlock();
		break;
	default:
		return -1;
	}
	return 0;
}

/*ARGSUSED*/
int
oslog_streamioctl(__unused dev_t dev, u_long com, caddr_t data, __unused int flag, __unused struct proc *p)
{
	int err = 0;

	stream_lock();

	switch (com) {
	case FIONBIO:
		if (data && *(int *)data) {
			oslog_streamsoftc.sc_state |= LOG_NBIO;
		} else {
			oslog_streamsoftc.sc_state &= ~LOG_NBIO;
		}
		break;
	case FIOASYNC:
		if (data && *(int *)data) {
			oslog_streamsoftc.sc_state |= LOG_ASYNC;
		} else {
			oslog_streamsoftc.sc_state &= ~LOG_ASYNC;
		}
		break;
	default:
		err = -1;
		break;
	}

	stream_unlock();
	return err;
}

__startup_func
static void
oslog_init_firehose(void)
{
	if (os_log_disabled()) {
		printf("Firehose disabled: Logging disabled by ATM\n");
		return;
	}

	kern_return_t kr;
	if (!PE_parse_boot_argn("firehose_chunk_count", &__firehose_buffer_kernel_chunk_count, sizeof(__firehose_buffer_kernel_chunk_count))) {
		__firehose_buffer_kernel_chunk_count = FIREHOSE_BUFFER_KERNEL_DEFAULT_CHUNK_COUNT;
	}
	if (!PE_parse_boot_argn("firehose_io_pages", &__firehose_num_kernel_io_pages, sizeof(__firehose_num_kernel_io_pages))) {
		__firehose_num_kernel_io_pages = FIREHOSE_BUFFER_KERNEL_DEFAULT_IO_PAGES;
	}
	if (!__firehose_kernel_configuration_valid(__firehose_buffer_kernel_chunk_count, __firehose_num_kernel_io_pages)) {
		printf("illegal firehose configuration %u/%u, using defaults\n", __firehose_buffer_kernel_chunk_count, __firehose_num_kernel_io_pages);
		__firehose_buffer_kernel_chunk_count = FIREHOSE_BUFFER_KERNEL_DEFAULT_CHUNK_COUNT;
		__firehose_num_kernel_io_pages = FIREHOSE_BUFFER_KERNEL_DEFAULT_IO_PAGES;
	}
	vm_size_t size = __firehose_buffer_kernel_chunk_count * FIREHOSE_CHUNK_SIZE;

	kr = kmem_alloc_flags(kernel_map, &kernel_firehose_addr,
	    size + (2 * PAGE_SIZE), VM_KERN_MEMORY_LOG,
	    KMA_GUARD_FIRST | KMA_GUARD_LAST | KMA_ZERO);
	if (kr != KERN_SUCCESS) {
		panic("Failed to allocate memory for firehose logging buffer");
	}
	kernel_firehose_addr += PAGE_SIZE;
	/* register buffer with firehose */
	kernel_firehose_addr = (vm_offset_t)__firehose_buffer_create((size_t *) &size);

	printf("Firehose configured: %u chunks, %u io pages\n",
	    __firehose_buffer_kernel_chunk_count, __firehose_num_kernel_io_pages);
}
STARTUP(OSLOG, STARTUP_RANK_SECOND, oslog_init_firehose);

/*
 * log_putc_locked
 *
 * Decription:	Output a character to the log; assumes the bsd_log_lock() or
 *              bsd_log_lock_safe() is held by the caller.
 *
 * Parameters:	c				Character to output
 *
 * Returns:	(void)
 *
 * Notes:	This functions is used for multibyte output to the log; it
 *		should be used preferrentially where possible to ensure that
 *		log entries do not end up interspersed due to preemption or
 *		SMP reentrancy.
 */
void
log_putc_locked(struct msgbuf *mbp, char c)
{
	mbp->msg_bufc[mbp->msg_bufx++] = c;
	if (mbp->msg_bufx >= mbp->msg_size) {
		mbp->msg_bufx = 0;
	}
}

/*
 * log_putc
 *
 * Decription:	Output a character to the log; assumes the bsd_log_lock() or
 *              bsd_log_lock_safe() is NOT held by the caller.
 *
 * Parameters:	c				Character to output
 *
 * Returns:	(void)
 *
 * Notes:	This function is used for single byte output to the log.  It
 *		primarily exists to maintain binary backward compatibility.
 */
void
log_putc(char c)
{
	if (!bsd_log_lock(oslog_is_safe())) {
		os_atomic_inc(&oslog_msgbuf_dropped_charcount, relaxed);
		return;
	}

	log_putc_locked(msgbufp, c);
	int unread_count = msgbufp->msg_bufx - msgbufp->msg_bufr;

	bsd_log_unlock();

	if (unread_count < 0) {
		unread_count = 0 - unread_count;
	}
	if (c == '\n' || unread_count >= (msgbufp->msg_size / 2)) {
		logwakeup(msgbufp);
	}
}

/*
 * it is possible to increase the kernel log buffer size by adding
 *   msgbuf=n
 * to the kernel command line, and to read the current size using
 *   sysctl kern.msgbuf
 * If there is no parameter on the kernel command line, the buffer is
 * allocated statically and is CONFIG_MSG_BSIZE characters in size, otherwise
 * memory is dynamically allocated. Memory management must already be up.
 */
static int
log_setsize(size_t size)
{
	int i, count;
	char *p;

	if (size == 0 || size > MAX_MSG_BSIZE) {
		return EINVAL;
	}

	int new_logsize = (int)size;
	char *new_logdata = kalloc_data(size, Z_WAITOK | Z_ZERO);
	if (!new_logdata) {
		printf("Cannot resize system message buffer: Not enough memory\n");
		return ENOMEM;
	}

	bsd_log_lock_safe();

	char *old_logdata = msgbufp->msg_bufc;
	int old_logsize = msgbufp->msg_size;
	int old_bufr = msgbufp->msg_bufr;
	int old_bufx = msgbufp->msg_bufx;

	/* start "new_logsize" bytes before the write pointer */
	if (new_logsize <= old_bufx) {
		count = new_logsize;
		p = old_logdata + old_bufx - count;
	} else {
		/*
		 * if new buffer is bigger, copy what we have and let the
		 * bzero above handle the difference
		 */
		count = MIN(new_logsize, old_logsize);
		p = old_logdata + old_logsize - (count - old_bufx);
	}
	for (i = 0; i < count; i++) {
		if (p >= old_logdata + old_logsize) {
			p = old_logdata;
		}
		new_logdata[i] = *p++;
	}

	int new_bufx = i;
	if (new_bufx >= new_logsize) {
		new_bufx = 0;
	}
	msgbufp->msg_bufx = new_bufx;

	int new_bufr = old_bufx - old_bufr; /* how much were we trailing bufx by? */
	if (new_bufr < 0) {
		new_bufr += old_logsize;
	}
	new_bufr = new_bufx - new_bufr; /* now relative to oldest data in new buffer */
	if (new_bufr < 0) {
		new_bufr += new_logsize;
	}
	msgbufp->msg_bufr = new_bufr;

	msgbufp->msg_size = new_logsize;
	msgbufp->msg_bufc = new_logdata;

	bsd_log_unlock();

	/*
	 * This memory is now dead - clear it so that it compresses better
	 * in case of suspend to disk etc.
	 */
	bzero(old_logdata, old_logsize);
	if (old_logdata != smsg_bufc) {
		/* dynamic memory that must be freed */
		kfree_data(old_logdata, old_logsize);
	}

	printf("System message buffer configured: %lu bytes\n", size);

	return 0;
}

/*
 * This function should have its own boot argument instead of being side
 * effect of 'msgbuf' boot argument.
 */
__startup_func
static void
oslog_setsize(size_t size)
{
	if (size <= OSLOG_STREAM_BUF_SIZE || size > OSLOG_STREAM_MAX_BUF_SIZE) {
		return;
	}

	size_t scale = (size / OSLOG_STREAM_BUF_SIZE);
	oslog_stream_buf_size = (int)size;
	oslog_stream_num_entries = (int)(scale * OSLOG_NUM_STREAM_ENTRIES);

	printf("OSLog stream buffer configured: %d bytes, %d entries\n",
	    oslog_stream_buf_size, oslog_stream_num_entries);
}

SYSCTL_PROC(_kern, OID_AUTO, msgbuf,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0,
    sysctl_kern_msgbuf, "I", "");

static int
sysctl_kern_msgbuf(struct sysctl_oid *oidp __unused,
    void *arg1 __unused, int arg2 __unused, struct sysctl_req *req)
{
	int old_bufsize, bufsize;
	int error;

	bsd_log_lock_safe();
	old_bufsize = bufsize = msgbufp->msg_size;
	bsd_log_unlock();

	error = sysctl_io_number(req, bufsize, sizeof(bufsize), &bufsize, NULL);
	if (error) {
		return error;
	}

	if (bufsize < 0) {
		return EINVAL;
	}

	if (bufsize != old_bufsize) {
		error = log_setsize(bufsize);
	}

	return error;
}

/*
 * This should be called by /sbin/dmesg only via libproc.
 * It returns as much data still in the buffer as possible.
 */
int
log_dmesg(user_addr_t buffer, uint32_t buffersize, int32_t *retval)
{
	uint32_t i;
	uint32_t localbuff_size;
	int error = 0, newl, skip;
	char *localbuff, *p, *copystart, ch;
	size_t copysize;

	bsd_log_lock_safe();
	localbuff_size = (msgbufp->msg_size + 2); /* + '\n' + '\0' */
	bsd_log_unlock();

	/* Allocate a temporary non-circular buffer for copyout */
	localbuff = kalloc_data(localbuff_size, Z_WAITOK);
	if (!localbuff) {
		printf("log_dmesg: unable to allocate memory\n");
		return ENOMEM;
	}

	/* in between here, the log could become bigger, but that's fine */
	bsd_log_lock_safe();

	/*
	 * The message buffer is circular; start at the write pointer, and
	 * make one loop up to write pointer - 1.
	 */
	p = msgbufp->msg_bufc + msgbufp->msg_bufx;
	for (i = newl = skip = 0; p != msgbufp->msg_bufc + msgbufp->msg_bufx - 1; ++p) {
		if (p >= msgbufp->msg_bufc + msgbufp->msg_size) {
			p = msgbufp->msg_bufc;
		}
		ch = *p;
		/* Skip "\n<.*>" syslog sequences. */
		if (skip) {
			if (ch == '>') {
				newl = skip = 0;
			}
			continue;
		}
		if (newl && ch == '<') {
			skip = 1;
			continue;
		}
		if (ch == '\0') {
			continue;
		}
		newl = (ch == '\n');
		localbuff[i++] = ch;
		/* The original version of this routine contained a buffer
		 * overflow. At the time, a "small" targeted fix was desired
		 * so the change below to check the buffer bounds was made.
		 * TODO: rewrite this needlessly convoluted routine.
		 */
		if (i == (localbuff_size - 2)) {
			break;
		}
	}
	if (!newl) {
		localbuff[i++] = '\n';
	}
	localbuff[i++] = 0;

	if (buffersize >= i) {
		copystart = localbuff;
		copysize = i;
	} else {
		copystart = localbuff + i - buffersize;
		copysize = buffersize;
	}

	bsd_log_unlock();

	error = copyout(copystart, buffer, copysize);
	if (!error) {
		*retval = (int32_t)copysize;
	}

	kfree_data(localbuff, localbuff_size);
	return error;
}

#ifdef CONFIG_XNUPOST

size_t find_pattern_in_buffer(const char *, size_t, size_t);

/*
 * returns count of pattern found in systemlog buffer.
 * stops searching further if count reaches expected_count.
 */
size_t
find_pattern_in_buffer(const char *pattern, size_t len, size_t expected_count)
{
	if (pattern == NULL || len == 0 || expected_count == 0) {
		return 0;
	}

	size_t msg_bufx = msgbufp->msg_bufx;
	size_t msg_size = msgbufp->msg_size;
	size_t match_count = 0;

	for (size_t i = 0; i < msg_size; i++) {
		boolean_t match = TRUE;
		for (size_t j = 0; j < len; j++) {
			size_t pos = (msg_bufx + i + j) % msg_size;
			if (msgbufp->msg_bufc[pos] != pattern[j]) {
				match = FALSE;
				break;
			}
		}
		if (match && ++match_count >= expected_count) {
			break;
		}
	}

	return match_count;
}

__startup_func
static void
oslog_init_msgbuf(void)
{
	size_t msgbuf_size = 0;

	if (PE_parse_boot_argn("msgbuf", &msgbuf_size, sizeof(msgbuf_size))) {
		(void) log_setsize(msgbuf_size);
		oslog_setsize(msgbuf_size);
	}
}
STARTUP(OSLOG, STARTUP_RANK_SECOND, oslog_init_msgbuf);

#endif
