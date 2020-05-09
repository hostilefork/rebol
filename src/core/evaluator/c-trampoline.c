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


// To make things simpler in the main evaluator loop, we abbreviate the top
// frame just as "F"
//
#define F FS_TOP


//
//  Eval_Internal_Maybe_Stale_Throws: C
//
// See notes at top of file for general remarks on this central function's
// name, and that wrappers should nearly always be used to call it.
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
bool Eval_Internal_Maybe_Stale_Throws(void)
{
    REBFRM *start = F;

  loop:

    UPDATE_TICK_DEBUG(F, nullptr);

    // v-- This is the TG_Break_At_Tick or C-DEBUG-BREAK landing spot --v

    assert((F->executor == &Action_Executor) == (F->original != nullptr));

    REB_R r = (F->executor)(F);

    if (r != R_THROWN)
        assert((F->executor == &Action_Executor) == (F->original != nullptr));

    if (F->executor == nullptr) {  // no further execution for frame, drop it
        assert(r == F->out);
        CLEAR_CELL_FLAG(F->out, OUT_MARKED_STALE);  // !!! review

        // !!! Currently we do not drop the topmost frame, because some code
        // (e.g. MATCH) would ask for a frame to be filled, and then steal
        // its resulting varlist.  However, if MATCH is on the stack when it
        // makes the call, it's not stackless...e.g. it should be written
        // some other way.
        //
        if (F == start) {
            F->executor = &New_Expression_Executor;  // !!! Old invariant
            return false;
        }

        // !!! Detaching vs. dropping a frame is used by routines like REDUCE,
        // which wish to reuse a frame for successive evaluations.
        //
        if (GET_EVAL_FLAG(F, DETACH_DONT_DROP)) {
            REBFRM* temp = F;
            TG_Top_Frame = temp->prior;
            temp->prior = nullptr;  // we don't "drop" it, but...
            // !!! leave flag or reset it?
        }
        else
            Drop_Frame(F);  // frees feed if necessary

        r = F->out;
    }

    if (r == R_CONTINUATION) {
        assert(F->executor != nullptr);  // *topmost* frame needs callback
        goto loop;  // keep going
    }

  #if !defined(NDEBUG)
    if (not F->original)
        Eval_Core_Exit_Checks_Debug(F);   // called unless a fail() longjmps
  #endif

    if (r == R_THROWN) {
        while (F != start) {
            Drop_Frame(F);
            if (F->original)  // function is running, assume only catchers ATM
                goto loop;
        }
    }
    else {
        // !!! This is going to be the right place to handle other variants of
        // return values consistently, e.g. API handles.  The return results
        // from native dispatchers may be specific to interactions.

        assert(r == F->out);

        // Want to keep this flag between an operation and an ensuing enfix in
        // the same frame, so can't clear in Drop_Action(), e.g. due to:
        //
        //     left-lit: enfix :lit
        //     o: make object! [f: does [1]]
        //     o/f left-lit  ; want error suggesting -> here, need flag for that
        //
        CLEAR_EVAL_FLAG(F, DIDNT_LEFT_QUOTE_PATH);
        assert(NOT_FEED_FLAG(F->feed, NEXT_ARG_FROM_OUT));  // must be consumed

        #if !defined(NDEBUG)
        //assert(NOT_EVAL_FLAG(F, DOING_PICKUPS));
        //assert(F->flags.bits == F->initial_flags);  // changes should be restored
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

    assert(F == start);

    F->executor = &New_Expression_Executor;  // !!! Old invariant

    assert(r == R_THROWN);
    return true;  // thrown
}
