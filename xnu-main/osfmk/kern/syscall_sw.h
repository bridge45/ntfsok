/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
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
/*
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */

#ifndef	_KERN_SYSCALL_SW_H_
#define	_KERN_SYSCALL_SW_H_

#include <mach_assert.h>

/*
 *	mach_trap_stack indicates the trap may discard
 *	its kernel stack.  Some architectures may need
 *	to save more state in the pcb for these traps.
 */
#if CONFIG_REQUIRES_U32_MUNGING
typedef	void	mach_munge_t(void *);
#elif __arm__ && (__BIGGEST_ALIGNMENT__ > 4)
typedef	int	mach_munge_t(const void *, void *);
#endif

typedef struct {
	int			mach_trap_arg_count; /* Number of trap arguments (Arch independant) */
	kern_return_t		(*mach_trap_function)(void *);
#if CONFIG_REQUIRES_U32_MUNGING || (__arm__ && (__BIGGEST_ALIGNMENT__ > 4))
	mach_munge_t		*mach_trap_arg_munge32; /* system call argument munger routine for 32-bit */
#endif
	int			mach_trap_u32_words; /* number of 32-bit words to copyin for U32 */
#if	MACH_ASSERT
	const char*		mach_trap_name;
#endif /* MACH_ASSERT */
} mach_trap_t;

#define MACH_TRAP_TABLE_COUNT   128


extern const mach_trap_t    mach_trap_table[];
extern const int                  mach_trap_count;
extern const char * const	mach_syscall_name_table[MACH_TRAP_TABLE_COUNT];

#if CONFIG_REQUIRES_U32_MUNGING || (__arm__ && (__BIGGEST_ALIGNMENT__ > 4))

#if	!MACH_ASSERT
#define	MACH_TRAP(name, arg_count, u32_arg_words, munge32)	\
	{ (arg_count), (kern_return_t (*)(void *)) (name), munge32, (u32_arg_words)  }
#else /* !MACH_ASSERT */
#define	MACH_TRAP(name, arg_count, u32_arg_words, munge32)	\
	{ (arg_count), (kern_return_t (*)(void *)) (name), munge32, (u32_arg_words), #name  }
#endif /* !MACH_ASSERT */


#else /* !CONFIG_REQUIRES_U32_MUNGING || (__arm__ && (__BIGGEST_ALIGNMENT__ > 4)) */

#if	!MACH_ASSERT
#define	MACH_TRAP(name, arg_count, u32_arg_words, munge32)	\
	{ (arg_count), (kern_return_t (*)(void *)) (name), (u32_arg_words)  }
#else /* !MACH_ASSERT */
#define	MACH_TRAP(name, arg_count, u32_arg_words, munge32)	\
	{ (arg_count), (kern_return_t (*)(void *)) (name), (u32_arg_words), #name  }
#endif /* !MACH_ASSERT */

#endif /* !CONFIG_REQUIRES_U32_MUNGING */

#endif	/* _KERN_SYSCALL_SW_H_ */
