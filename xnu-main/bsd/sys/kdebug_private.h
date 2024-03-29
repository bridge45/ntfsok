/*
 * Copyright (c) 2000-2018 Apple Inc. All rights reserved.
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

#ifndef BSD_KDEBUG_PRIVATE_H
#define BSD_KDEBUG_PRIVATE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>
#include <sys/kdebug.h>

__BEGIN_DECLS

#if !KERNEL

#include <Availability.h>

#pragma mark - user space SPI

/*
 * OS components can use the full precision of the "code" field
 * (Class, SubClass, Code) to inject events using kdebug_trace() by
 * using:
 *
 * kdebug_trace(KDBG_CODE(DBG_XPC, 15, 1) | DBG_FUNC_NONE, 1, 2, 3, 4);
 *
 * These trace points can be included in production code, since they
 * use reserved, non-overlapping ranges.  The performance impact when
 * kernel tracing is not enabled is minimal.  However, when tracing is enabled,
 * each tracepoint becomes a syscall.  For this reason, os_signpost(3) is
 * recommended instead of kdebug_trace(2).
 *
 * Classes can be reserved by filing a Radar in xnu | ktrace.
 *
 * 64-bit arguments may be truncated if the system is using a 32-bit
 * kernel.
 *
 * On error, -1 will be returned and errno will indicate the error.
 */
int kdebug_trace(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3,
    uint64_t arg4)
__OSX_AVAILABLE(10.10) __IOS_AVAILABLE(8.2);

/*!
 * @function kdebug_trace_string
 *
 * @discussion
 * This function emits strings to kdebug trace along with an ID and allows
 * for previously-traced strings to be overwritten and invalidated.
 *
 * To start tracing a string and generate an ID to use to refer to it:
 *
 *      string_id = kdebug_trace_string(debugid, 0, "string");
 *
 * To replace a string previously traced:
 *
 *      string_id = kdebug_trace_string(debugid, string_id, "new string");
 *
 * To invalidate a string ID:
 *
 *      string_id = kdebug_trace_string(debugid, string_id, NULL);
 *
 * To check for errors:
 *
 *      if ((int64_t)string_id == -1) { perror("string error") }
 *
 * @param debugid
 * The `debugid` to check if its enabled before tracing and include as
 * an argument in the event containing the string.
 *
 * Some classes or subclasses are reserved for specific uses and are not
 * allowed to be used with this function.  No function qualifiers are
 * allowed on `debugid`.
 *
 * @param str_id
 * When 0, a new ID will be generated and returned if tracing is
 * enabled.
 *
 * Otherwise `str_id` must contain an ID that was previously generated
 * with this function.  Clents should pass NULL in `str` if `str_id`
 * is no longer in use.  Otherwise, the string previously mapped to
 * `str_id` will be overwritten with the contents of `str`.
 *
 * @param str
 * A NUL-terminated 'C' string containing the characters that should be
 * traced alongside `str_id`.
 *
 * If necessary, the string will be truncated at an
 * implementation-defined length.  The string must not be the empty
 * string, but can be NULL if a valid `str_id` is provided.
 *
 * @return
 * 0 if tracing is disabled or `debugid` is being filtered out of trace.
 * It can also return (int64_t)-1 if an error occured. Otherwise,
 * it returns the ID to use to refer to the string in future
 * kdebug_trace(2) calls.
 *
 * The errors that can occur are:
 *
 * EINVAL
 *      There are function qualifiers on `debugid`, `str` is empty, or
 *      `str_id` was not generated by this function.
 * EPERM
 *      The `debugid`'s class or subclass is reserved for internal use.
 * EFAULT
 *      `str` is an invalid address or NULL when `str_id` is 0.
 */
extern uint64_t kdebug_trace_string(uint32_t debugid, uint64_t str_id,
    const char *str)
__OSX_AVAILABLE(10.11) __IOS_AVAILABLE(9.0);

/*
 * Although the performance impact of kdebug_trace() when kernel
 * tracing is not enabled is minimal, it may require the caller to
 * perform an expensive calculation/summarization. This cost can be
 * skipped by checking the kdebug_is_enabled() predicate:
 *
 * if (kdebug_is_enabled(KDBG_CODE(DBG_XPC, 15, 1))) {
 *     uint64_t arg1 = ...;
 *     uint64_t arg2 = ...;
 *     kdebug_trace(KDBG_CODE(DBG_XPC, 15, 1) | DBG_FUNC_NONE, arg1, arg2, 0, 0);
 * }
 *
 * If tracing is enabled for the code at the time of the check, 1
 * will be returned. Otherwise, 0 will be returned.
 */
extern bool kdebug_is_enabled(uint32_t code)
__API_AVAILABLE(macos(10.12), ios(10), watchos(3), tvos(10));

/*
 * Returns a pointer to the userspace typefilter, if one is available.
 * May return NULL.
 */
extern void *kdebug_typefilter(void)
__API_AVAILABLE(macos(10.12), ios(10), watchos(3), tvos(10));

/*
 * Returns true if kdebug is using continuous time for its events, and false
 * otherwise.
 */
extern bool kdebug_using_continuous_time(void)
__API_AVAILABLE(macos(10.15), ios(13), tvos(13), watchos(6));

/*
 * Convert an absolute time to a kdebug timestamp.
 */
extern uint64_t kdebug_timestamp_from_absolute(uint64_t abstime)
__API_AVAILABLE(macos(12), ios(15), tvos(15), watchos(8));

/*
 * Convert a continuous time to a kdebug timestamp.
 */
extern uint64_t kdebug_timestamp_from_continuous(uint64_t conttime)
__API_AVAILABLE(macos(12), ios(15), tvos(15), watchos(8));

/*
 * Capture a kdebug timestamp for the current time.
 */
extern uint64_t kdebug_timestamp(void)
__API_AVAILABLE(macos(12), ios(15), tvos(15), watchos(8));

#endif /* !KERNEL */

#pragma mark - private debugids

#define DBG_PPT         36
#define DBG_PERFCTRL    39
#define DBG_CLPC        50
#define DBG_MUSE        52

/* **** 128 to 139 are reserved for IOP tracing **** */
#define DBG_ANS         128
#define DBG_SIO         129
#define DBG_SEP         130
#define DBG_ISP         131
#define DBG_OSCAR       132
#define DBG_EMBEDDEDGFX 133
#define DBG_PMP         134
#define DBG_RTKIT       135

#define MACH_BRIDGE_RCV_TS      0x1     /* receive timestamp pair from interrupt handler */
#define MACH_BRIDGE_REMOTE_TIME 0x2     /* calculate remote timestamp */
#define MACH_BRIDGE_RESET_TS    0x3     /* reset timestamp conversion parameters */
#define MACH_BRIDGE_TS_PARAMS   0x4     /* recompute timestamp conversion parameters */
#define MACH_BRIDGE_SKIP_TS     0x5     /* skip timestamp */
#define MACH_BRIDGE_TS_MISMATCH 0x6     /* mismatch between predicted and received remote timestamp */
#define MACH_BRIDGE_OBSV_RATE   0x7     /* out of range observed rates */

/* DBG_SKYWALK has same toplevel code as DBG_DLIL, so don't reuse subcodes */
#define DBG_SKYWALK_ALWAYSON    0x10
#define DBG_SKYWALK_FLOWSWITCH  0x11
#define DBG_SKYWALK_NETIF       0x12
#define DBG_SKYWALK_CHANNEL     0x13
#define DBG_SKYWALK_PACKET      0x14

#define PPT_TEST            0x01
#define PPT_JETSAM_HIWAT    0x02
#define PPT_JETSAM_TOPPROC  0x03

#define SKYWALKDBG_CODE(SubClass, code) KDBG_CODE(DBG_DLIL, SubClass, code)
#define PPTDBG_CODE(SubClass, code) KDBG_CODE(DBG_PPT, SubClass, code)
#define PERFCTRL_CODE(SubClass, code) KDBG_CODE(DBG_PERFCTRL, SubClass, code)

#if !defined(DRIVERKIT)

extern unsigned int kdebug_enable;

/*
 * Bits used by kdebug_enable.  These used to control which events are traced at
 * runtime.
 */
#define KDEBUG_ENABLE_TRACE     0x001U
/*
 * If set, the timestamps in events are expected to be continuous times.
 * Otherwise, the timestamps are absolute times.  IOPs should observe this bit
 * in order to log events that can be merged cleanly with other event streams.
 */
#define KDEBUG_ENABLE_CONT_TIME 0x020U

#define KDEBUG_TRACE (KDEBUG_ENABLE_TRACE)

/* obsolete kdebug_enable bits */
#define KDEBUG_ENABLE_ENTROPY   0x002U
#define KDEBUG_ENABLE_CHUD      0x004U
#define KDEBUG_ENABLE_PPT       0x008U
#define KDEBUG_ENABLE_SERIAL    0x010U
#define KDEBUG_PPT    (KDEBUG_ENABLE_PPT)
#define KDEBUG_COMMON (KDEBUG_ENABLE_TRACE | KDEBUG_ENABLE_PPT)

/*
 * The kernel debug configuration level.  These values control which events are
 * compiled in under different build configurations.
 *
 * Infer the supported kernel debug event level from config option.  Use
 * (KDEBUG_LEVEL >= KDEBUG_LEVEL_STANDARD) as a guard to protect unaudited debug
 * code.
 */
#define KDEBUG_LEVEL_NONE     0
#define KDEBUG_LEVEL_IST      1
#define KDEBUG_LEVEL_STANDARD 2
#define KDEBUG_LEVEL_FULL     3

#if NO_KDEBUG
#define KDEBUG_LEVEL KDEBUG_LEVEL_NONE
#elif IST_KDEBUG
#define KDEBUG_LEVEL KDEBUG_LEVEL_IST
#elif KDEBUG
#define KDEBUG_LEVEL KDEBUG_LEVEL_FULL
#else
#define KDEBUG_LEVEL KDEBUG_LEVEL_STANDARD
/*
 * Currently, all other kernel configurations (development, etc) build with
 * KDEBUG_LEVEL_STANDARD.
 */
#endif

/*
 * Some Apple internal clients try to use the kernel macros in user space.
 */
#ifndef KERNEL_DEBUG
#define KERNEL_DEBUG(...) do { } while (0)
#endif /* !defined(KERNEL_DEBUG) */

#pragma mark - private definitions

/*
 * Ensure that both LP32 and LP64 variants of arm64 use the same kd_buf
 * structure.
 */
#if defined(__arm64__)
typedef uint64_t kd_buf_argtype;
#else
typedef uintptr_t kd_buf_argtype;
#endif

typedef struct {
	uint64_t timestamp;
	kd_buf_argtype arg1;
	kd_buf_argtype arg2;
	kd_buf_argtype arg3;
	kd_buf_argtype arg4;
	kd_buf_argtype arg5; /* the thread ID */
	uint32_t debugid;
/*
 * Ensure that both LP32 and LP64 variants of arm64 use the same kd_buf
 * structure.
 */
#if defined(__LP64__) || defined(__arm64__)
	uint32_t cpuid;
	kd_buf_argtype unused;
#endif
} kd_buf;

#if defined(__LP64__) || defined(__arm64__)
#define KDBG_TIMESTAMP_MASK             0xffffffffffffffffULL
static inline void
kdbg_set_cpu(kd_buf *kp, int cpu)
{
	kp->cpuid = (unsigned int)cpu;
}
static inline int
kdbg_get_cpu(kd_buf *kp)
{
	return (int)kp->cpuid;
}
static inline void
kdbg_set_timestamp(kd_buf *kp, uint64_t thetime)
{
	kp->timestamp = thetime;
}
static inline uint64_t
kdbg_get_timestamp(kd_buf *kp)
{
	return kp->timestamp;
}
static inline void
kdbg_set_timestamp_and_cpu(kd_buf *kp, uint64_t thetime, int cpu)
{
	kdbg_set_timestamp(kp, thetime);
	kdbg_set_cpu(kp, cpu);
}
#else
#define KDBG_TIMESTAMP_MASK 0x00ffffffffffffffULL
#define KDBG_CPU_MASK       0xff00000000000000ULL
#define KDBG_CPU_SHIFT      56
static inline void
kdbg_set_cpu(kd_buf *kp, int cpu)
{
	kp->timestamp = (kp->timestamp & KDBG_TIMESTAMP_MASK) |
	    (((uint64_t) cpu) << KDBG_CPU_SHIFT);
}
static inline int
kdbg_get_cpu(kd_buf *kp)
{
	return (int) (((kp)->timestamp & KDBG_CPU_MASK) >> KDBG_CPU_SHIFT);
}
static inline void
kdbg_set_timestamp(kd_buf *kp, uint64_t thetime)
{
	kp->timestamp = thetime & KDBG_TIMESTAMP_MASK;
}
static inline uint64_t
kdbg_get_timestamp(kd_buf *kp)
{
	return kp->timestamp & KDBG_TIMESTAMP_MASK;
}
static inline void
kdbg_set_timestamp_and_cpu(kd_buf *kp, uint64_t thetime, int cpu)
{
	kp->timestamp = (thetime & KDBG_TIMESTAMP_MASK) |
	    (((uint64_t) cpu) << KDBG_CPU_SHIFT);
}
#endif

/*
 * 2^16 bits (8 kilobytes), one for each possible class/subclass combination
 */
#define KDBG_TYPEFILTER_BITMAP_SIZE ((256 * 256) / 8)

/*
 * Bits for kd_ctrl_page.flags, KERN_KD{D,E}FLAGS.
 */
/* disable tracing when buffers are full */
#define KDBG_NOWRAP          0x0002
/* buffer has wrapped */
#define KDBG_WRAPPED         0x0008
/* only include processes with kdebug bit set in proc */
#define KDBG_PIDCHECK        0x0010
/* thread map is initialized */
#define KDBG_MAPINIT         0x0020
/* exclude processes based on kdebug bit in proc */
#define KDBG_PIDEXCLUDE      0x0040
/* whether the kdebug locks are intialized */
#define KDBG_LOCKINIT        0x0080
/* word size of the kernel */
#define KDBG_LP64            0x0100
/* whether timestamps are in continuous time */
#define KDBG_CONTINUOUS_TIME 0x0200
/* coprocessor tracing is disabled */
#define KDBG_DISABLE_COPROCS 0x0400

/* flags that are allowed to be set by user space */
#define KDBG_USERFLAGS  (KDBG_NOWRAP | KDBG_CONTINUOUS_TIME | KDBG_DISABLE_COPROCS)

/* obsolete flags */
#define KDBG_INIT              0x01
#define KDBG_FREERUN           0x04

/* bits for kd_ctrl_page.flags and kbufinfo_t.flags */

/* only trace events within a range */
#define KDBG_RANGECHECK       0x00100000U
/* only trace at most 4 types of events, at the code granularity */
#define KDBG_VALCHECK         0x00200000U
/* check class and subclass against the typefilter */
#define KDBG_TYPEFILTER_CHECK 0x00400000U
/* we are going to use 64-bit debugid in arg4 */
#define KDBG_DEBUGID_64       0x00800000U
/* kdebug trace buffers are initialized */
#define KDBG_BUFINIT          0x80000000U

/* bits for the type field of kd_regtype */
#define KDBG_CLASSTYPE  0x10000
#define KDBG_SUBCLSTYPE 0x20000
#define KDBG_RANGETYPE  0x40000
#define KDBG_TYPENONE   0x80000
#define KDBG_CKTYPES    0xF0000

typedef struct {
	unsigned int type;
	unsigned int value1;
	unsigned int value2;
	unsigned int value3;
	unsigned int value4;
} kd_regtype;

typedef struct {
	/* number of events that can fit in the buffers */
	int nkdbufs;
	/* set if trace is disabled */
	int nolog;
	/* kd_ctrl_page.flags */
	unsigned int flags;
	/* number of threads in thread map */
	int nkdthreads;
	/* the owning pid */
	int bufid;
} kbufinfo_t;

typedef struct {
	/* the thread ID */
#if defined(__arm64__)
	uint64_t thread;
#else
	uintptr_t thread __kernel_data_semantics;
#endif
	/* 0 for invalid, otherwise the PID (or 1 for kernel_task) */
	int valid;
	/* the name of the process owning the thread */
	char command[20];
} kd_threadmap;

typedef struct {
	uint32_t version_no;
	uint32_t cpu_count;
} kd_cpumap_header;

/* cpumap flags */
#define KDBG_CPUMAP_IS_IOP      0x1

typedef struct {
	uint32_t cpu_id;
	uint32_t flags;
	char name[8];
} kd_cpumap;

typedef struct {
	uint32_t cpu_id;
	uint32_t flags;
	char name[32];
} kd_cpumap_ext;

typedef struct {
	int             version_no;
	int             thread_count;
	uint64_t        TOD_secs;
	uint32_t        TOD_usecs;
} RAW_header;

#define RAW_VERSION0    0x55aa0000
#define RAW_VERSION1    0x55aa0101
#define RAW_VERSION2    0x55aa0200 /* Only used by kperf and Instruments */

/*
 * Bits set in the comm page for kdebug.
 */
#define KDEBUG_COMMPAGE_ENABLE_TRACE      0x1
#define KDEBUG_COMMPAGE_ENABLE_TYPEFILTER 0x2 /* Forced to false if ENABLE_TRACE is 0 */
#define KDEBUG_COMMPAGE_CONTINUOUS        0x4 /* Forced to false if ENABLE_TRACE is 0 */

#pragma mark - Tests

enum kdebug_test {
	KDTEST_KERNEL_MACROS = 1,
	KDTEST_OLD_TIMESTAMP = 2,
	KDTEST_FUTURE_TIMESTAMP = 3,
	KDTEST_SETUP_IOP = 4,
	KDTEST_SETUP_COPROCESSOR = 5,
	KDTEST_CONTINUOUS_TIMESTAMP = 6,
	KDTEST_ABSOLUTE_TIMESTAMP = 7,
};

#pragma mark - EnergyTracing

/* for EnergyTracing user space & clients */
#define kEnTrCompKernel     2

/*
 *   EnergyTracing opcodes
 *
 *   Activations use DBG_FUNC_START/END.
 *   Events are DBG_FUNC_NONE.
 */

/* Socket reads and writes are uniquely identified by the (sanitized)
 *  pointer to the socket struct in question.  To associate this address
 *  with the user space file descriptor, we have a socket activation with
 *  the FD as its identifier and the socket struct pointer as its value.
 */
#define kEnTrActKernSocket      1
#define kEnTrActKernSockRead    2
#define kEnTrActKernSockWrite   3

#define kEnTrActKernPoll        10
#define kEnTrActKernSelect      11
#define kEnTrActKernKQWait      12

// events
#define kEnTrEvUnblocked        256

// EnergyTracing flags (the low-order 16 bits of 'quality')
#define kEnTrFlagNonBlocking    1 << 0
#define kEnTrFlagNoWork         1 << 1

/*
 * EnergyTracing macros.
 */

#if (KDEBUG_LEVEL >= KDEBUG_LEVEL_IST)
// whether to bother calculating EnergyTracing inputs
// could change in future to see if DBG_ENERGYTRACE is active
#define ENTR_SHOULDTRACE kdebug_enable
// encode logical EnergyTracing into 32/64 KDebug trace
#define ENTR_KDTRACE(component, opcode, lifespan, id, quality, value)   \
do {                                                                    \
    uint32_t kdcode__;                                                  \
    uintptr_t highval__, lowval__, mask__ = 0xffffffff;                 \
    kdcode__ = KDBG_CODE(DBG_ENERGYTRACE,component,opcode)|(lifespan);  \
    highval__ = ((value) >> 32) & mask__;                               \
    lowval__ = (value) & mask__;                                        \
    ENTR_KDTRACEFUNC(kdcode__, id, quality, highval__, lowval__);       \
} while(0)

/*
 *   Trace the association of two existing activations.
 *
 *   An association is traced as a modification to the parent activation.
 *   In order to fit the sub-activation's component, activation code, and
 *   activation ID into a kdebug tracepoint, the arguments that would hold
 *   the value are left separate, and one stores the component and opcode
 *   of the sub-activation, while the other stores the pointer-sized
 *   activation ID.
 *
 *           arg2                   arg3               arg4
 +-----------------+  +~+----+----+--------+   +----------+
 |kEnTrModAssociate|  | |    |    |        |   |          |
 +-----------------+  +~+----+----+--------+   +----------+
 *                           8-bits unused       sub-activation ID
 *                                8-bit sub-component
 *                                     16-bit sub-opcode
 *
 */
#define kEnTrModAssociate (1 << 28)
#define ENTR_KDASSOCIATE(par_comp, par_opcode, par_act_id,              \
	    sub_comp, sub_opcode, sub_act_id)              \
do {                                                                    \
    unsigned sub_compcode = ((unsigned)sub_comp << 16) | sub_opcode;    \
    ENTR_KDTRACEFUNC(KDBG_CODE(DBG_ENERGYTRACE,par_comp,par_opcode),    \
	             par_act_id, kEnTrModAssociate, sub_compcode,       \
	             sub_act_id);                                       \
} while(0)

#else /* (KDEBUG_LEVEL >= KDEBUG_LEVEL_IST) */

#define ENTR_SHOULDTRACE FALSE
#define ENTR_KDTRACE(component, opcode, lifespan, id, quality, value)   \
	                            do {} while (0)
#define ENTR_KDASSOCIATE(par_comp, par_opcode, par_act_id,              \
	    sub_comp, sub_opcode, sub_act_id)              \
	                            do {} while (0)

#endif /* (KDEBUG_LEVEL >= KDEBUG_LEVEL_IST) */

#endif /* !defined(DRIVERKIT) */

__END_DECLS

#endif /* !defined(BSD_KDEBUG_PRIVATE_H) */
