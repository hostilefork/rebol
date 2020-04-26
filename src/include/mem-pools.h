//
//  File: %mem-pools.h
//  Summary: "Memory allocation"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// In R3-Alpha, the memory pool details were not exported to most of the
// system.  However, Make_Node() takes a pool ID, so things that want to make
// nodes need to know about SER_POOL.  And in order to take advantage of
// inlining, the system has to put a lot of things in header files.  Not
// being able to do so leads to a lot of pushing and popping overhead for
// parameters to commonly called routines (e.g. Make_Node())
//
// Hence if there are rules on which file is supposed to be calling which,
// those should be implemented in %source-analysis.r.
//


// Linked list of used memory segments
//
typedef struct rebol_mem_segment {
    struct rebol_mem_segment *next;
    uintptr_t size;
} REBSEG;


// Specifies initial pool sizes
//
typedef struct rebol_mem_spec {
    REBLEN wide; // size of allocation unit
    REBLEN units; // units per segment allocation
} REBPOOLSPEC;


// Pools manage fixed sized blocks of memory
//
struct rebol_mem_pool {
    REBSEG *segs; // first memory segment
    REBNOD *first; // first free node in pool
    REBNOD *last; // last free node in pool
    REBLEN wide; // size of allocation unit
    REBLEN units; // units per segment allocation
    REBLEN free; // number of units remaining
    REBLEN  has; // total number of units
};

#define DEF_POOL(size, count) {size, count}
#define MOD_POOL(size, count) {size * MEM_MIN_SIZE, count}

#define MEM_MIN_SIZE sizeof(REBVAL)
#define MEM_BIG_SIZE 1024

#define MEM_BALLAST 3000000

enum Mem_Pool_Specs {
    MEM_TINY_POOL = 0,
    MEM_SMALL_POOLS = MEM_TINY_POOL + 16,
    MEM_MID_POOLS = MEM_SMALL_POOLS + 4,
    MEM_BIG_POOLS = MEM_MID_POOLS + 4, // larger pools
    SER_POOL = MEM_BIG_POOLS,
  #ifdef UNUSUAL_REBVAL_SIZE
    PAR_POOL,
  #else
    PAR_POOL = SER_POOL,
  #endif
    FRM_POOL,  // struct Reb_Frame => REBFRM
    FED_POOL,  // struct Reb_Feed => REBFED
    SYSTEM_POOL,
    MAX_POOLS
};
