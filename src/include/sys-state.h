//
//  File: %sys-state.h
//  Summary: "Interpreter State"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Structure holding the information about the last point in the stack that
// wanted to set up an opportunity to intercept a `fail (Error_XXX())`
//
// For operations using this structure, see %sys-trap.h
//

struct Reb_State {
    //
    // We put the jmp_buf first, since it has alignment specifiers on Windows
    //
  #ifdef HAS_POSIX_SIGNAL
    sigjmp_buf cpu_state;
  #else
    jmp_buf cpu_state;
  #endif

    struct Reb_State *last_state;

    REBDSP dsp;
    struct Reb_Chunk *top_chunk;
    REBFRM *frame;
    REBLEN guarded_len;
    REBCTX *error;

    REBLEN manuals_len; // Where GC_Manuals was when state started
    REBLEN mold_buf_len;
    REBSIZ mold_buf_size;
    REBLEN mold_loop_tail;

    // Some operations disable the ability to halt, e.g. remove SIG_HALT
    // from Eval_Sigmask...and then restore it when they are done.  If one of
    // these operations is running and then there is a longjmp past the place
    // where the restore is going to happen, they'd have to pay the cost of
    // a PUSH_TRAP to put it back.  We save effort for that case by saving
    // the signal mask and restoring it at the trap states.
    //
    REBFLGS saved_sigmask;
};
