//
//  File: %c-trampoline.c
//  Summary: "Central Interpreter Loop for 'Stackless' Evaluation"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2020 Revolt Open Source Contributors
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
// This is the main loop of the interpreter.  We call this a "trampoline", in
// the spirit of the word as used in Lisp implementations.  That's because
// sub-expressions aren't evaluated with direct recursions of a C evaluator
// function, but using C's `return` to "bounce back" to a single loop,
// which invokes returned continuations.  Hence, there are no nested function
// calls and the stack won't grow:
//
// https://en.wikipedia.org/wiki/Trampoline_(computing)#High-level_programming
//

#include "sys-core.h"


#if defined(DEBUG_COUNT_TICKS)  // <-- THIS IS VERY USEFUL, SEE %sys-eval.h!
    //
    // This counter is incremented each time a function dispatcher is run
    // or a parse rule is executed.  See UPDATE_TICK_COUNT().
    //
    REBTCK TG_Tick;

    //      *** DON'T COMMIT THIS v-- KEEP IT AT ZERO! ***
    REBTCK TG_Break_At_Tick =      0;
    //      *** DON'T COMMIT THIS --^ KEEP IT AT ZERO! ***

#endif  // ^-- SERIOUSLY: READ ABOUT C-DEBUG-BREAK AND PLACES TICKS ARE STORED


//
//  Trampoline_Throws: C
//
// !!! The end goal is that this function is never found recursively on a
// standard evaluation stack.  The only way it should be found on the stack
// more than once would be to call out to non-Rebol code, which then turned
// around and made an API call back in...it would not be able to gracefully
// unwind across such C stack frames.  In the interim, not all natives have
// been rewritten as state machines.
//
// !!! There was an old concept that the way to write a stepwise debugger
// would be to replace this function in such a way that it would do some work
// related to examining the "pre" state of a frame... delegate to the "real"
// eval function... and then look at the end result after that call.  This
// meant hooking every recursion.  The new idea would be to make this
// "driver" easier to rewrite in its entirety, and examine the frame state
// as continuations are run.  This is radically different, and is requiring
// rethinking during the stackless transition.
//
bool Trampoline_Throws(void)
{
    REBFRM *start = FS_TOP;
    REBFRM *f = start;  // *usually* FS_TOP, unless requested to not drop

  loop:

    UPDATE_TICK_DEBUG(f, nullptr);

    // v-- This is the TG_Break_At_Tick or C-DEBUG-BREAK landing spot --v

    assert((f->executor == &Action_Executor) == (f->original != nullptr));

    REB_R r = (f->executor)(f);  // Note: f may not be FS_TOP at this moment
    f = FS_TOP;  // refresh to whatever topmost frame is after call

    if (r != R_THROWN)
        assert((f->executor == &Action_Executor) == (f->original != nullptr));

    if (f->executor == nullptr) {  // no further execution for frame, drop it
        assert(r == f->out);
        assert(NOT_CELL_FLAG(f->out, OUT_MARKED_STALE));

        // !!! Currently we do not drop the topmost frame, because some code
        // (e.g. MATCH) would ask for a frame to be filled, and then steal
        // its resulting varlist.  However, if MATCH is on the stack when it
        // makes the call, it's not stackless...e.g. it should be written
        // some other way.
        //
        if (f == start)
            return false;

        // Some natives and executors want to be able to leave a pushed frame
        // intact as the "top of stack" even when it has completed.  This
        // means that when those executors run, their frame parameter is
        // not the technical top of the stack.
        //
        REBFRM *prior = f->prior;
        if (GET_EVAL_FLAG(f, TRAMPOLINE_KEEPALIVE)) {
            f = prior;
            assert(f != FS_TOP);  // sanity check (*not* the top of stack)
        }
        else {
            Drop_Frame(f);
            f = prior;
            assert(f == FS_TOP);  // sanity check (is the top of the stack)
        }
        goto loop;
    }

    if (r == R_CONTINUATION) {
        assert(f->executor != nullptr);  // *topmost* frame needs callback
        goto loop;  // keep going
    }

  #if !defined(NDEBUG)
    if (not f->original)
        Eval_Core_Exit_Checks_Debug(f);   // called unless a fail() longjmps
  #endif

    if (r == R_THROWN) {
        while (f != start) {
            if (not f->original and f->out != f->prior->out) {
                //assert(f->out == FRM_SPARE(f->prior));
                Move_Value(f->prior->out, f->out);
            }
            Drop_Frame(f);
            f = FS_TOP;  // refresh
            if (f->original)  // function is running, assume only catchers ATM
                goto loop;
        }
    }
    else {
        // !!! This is going to be the right place to handle other variants of
        // return values consistently, e.g. API handles.  The return results
        // from native dispatchers may be specific to interactions.

        assert(r == f->out);

        // Want to keep this flag between an operation and an ensuing enfix in
        // the same frame, so can't clear in Drop_Action(), e.g. due to:
        //
        //     left-lit: enfix :lit
        //     o: make object! [f: does [1]]
        //     o/f left-lit  ; want error suggesting -> here, need flag for that
        //
        CLEAR_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH);
        assert(NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT));  // must be consumed

        #if !defined(NDEBUG)
        //assert(NOT_EVAL_FLAG(f, DOING_PICKUPS));
        //assert(f->flags.bits == f->initial_flags);  // changes should be restored
        #endif

        // We now are at the frame above the one that made the last
        // request.  What we need to know is if it wanted to do any
        // post-processing of the f->out that was resolved, or if it
        // is happy to take the result "as is"
        //
        // As it stands, the Action_Dispatcher always wants a chance to
        // follow up... even if just to Drop the action.  Whether it
        // calls the dispatcher or not again depends on DELEGATES_DISPATCH
        //
        goto loop;
    }

    assert(f == start);

    assert(r == R_THROWN);
    return true;  // thrown
}
