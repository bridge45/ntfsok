//
//  al_lock.h
//  ntfs
//
//  Created by work on 2023/8/9.
//
#ifndef al_lock_h
#define al_lock_h


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif


#include <stdint.h>

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif


typedef struct {
    unsigned long    opaque[10];
} al_lck_spin_t;



typedef struct {
    unsigned long       opaque[2];
} al_lck_mtx_t;

typedef struct {
    unsigned long       opaque[10];
} al_lck_mtx_ext_t;


#pragma pack(1)
typedef struct {
    uint32_t        opaque[3];
    uint32_t        opaque4;
} al_lck_rw_t;
#pragma pack()


#endif /* al_lock_h */
