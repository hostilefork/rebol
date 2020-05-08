//
//  File: %c-trampoline.c
//  Summary: "Central Interpreter Dispatcher"
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

    REB_R r /* = (f->executor)(f) */ ;  // !!!soon...

    if (F->executor == &Eval_Just_Use_Out) {
        r = Eval_Just_Use_Out(F);
    }
    else if (F->executor == &Eval_Brancher) {
        r = Eval_Brancher(F);
    }
    else if (F->executor == &Eval_Frame_Workhorse) {  // was REEVALUATE_CELL
        F->executor = &Eval_New_Expression;  // !!! until further study
        r = Eval_Frame_Workhorse(F);
    }
    else if (F->executor == &Eval_Post_Switch) {  // was POST_SWITCH
        F->executor = &Eval_New_Expression;  // !!! until further study
        r = Eval_Post_Switch(F);
    }
    else if (F->executor == &Group_Executor) {
        r = Group_Executor(F);
    }
    else if (F->original) {  // sign for Is_Action_Frame()
        //
        // !!! This related to a check in Do_Process_Action_Checks_Debug(),
        // see notes there.
        //
        /* SET_CELL_FLAG(f->out, OUT_MARKED_STALE); */
        assert(F->executor == &Eval_New_Expression);
        r = Eval_Action(F);
    }
    else {
        assert(F->executor == &Eval_New_Expression);
        r = Eval_New_Expression(F);
    }

    if (r == R_CONTINUATION)
        goto loop;  // keep going

    if (r == R_THROWN) {
    return_thrown:
      #if !defined(NDEBUG)
        Eval_Core_Exit_Checks_Debug(F);   // called unless a fail() longjmps
        // don't care if f->flags has changes; thrown frame is not resumable
      #endif

        if (GET_EVAL_FLAG(F, CONTINUATION)) {
            if (GET_EVAL_FLAG(F, FULFILLING_ARG)) {  // *before* function runs
                assert(NOT_EVAL_FLAG(F->prior, DISPATCHER_CATCHES));  // no catch
                assert(F->prior->original);  // must be running function
                assert(F->prior->arg == F->out);  // must be fulfilling f->arg
                Move_Value(F->prior->out, F->out);  // throw must be in out
            }
            Drop_Frame(F);
            if (F->original) {  // function is in process of running, can catch
                goto loop;
            }
            goto return_thrown;  // no action to drop so this frame just ends
        }

        while (F != start and not F->original) {
            Drop_Frame(F);
        }
    }
    else {
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
        Eval_Core_Exit_Checks_Debug(F);  // called unless a fail() longjmps
        assert(NOT_EVAL_FLAG(F, DOING_PICKUPS));

        assert(F->flags.bits == F->initial_flags);  // changes should be restored
      #endif

        // If the evaluations are running to the end of a block or a group,
        // we don't want to drop the frame.  But we do want an opportunity
        // to hook the evaluation step here in the top level driver.
        //
        if (GET_EVAL_FLAG(F, TO_END)) {
            if (NOT_END(F->feed->value))
                goto loop;
        }

        while (GET_EVAL_FLAG(F, CONTINUATION)) {
            CLEAR_CELL_FLAG(F->out, OUT_MARKED_STALE);  // !!! review

            if (GET_EVAL_FLAG(F, FULFILLING_ARG)) {
                do {
                    Drop_Frame(F);
                } while (not F->original);
                SET_EVAL_FLAG(F, ARG_FINISHED);
                goto loop;
            }

            if (GET_EVAL_FLAG(F, DETACH_DONT_DROP)) {
                REBFRM* temp = F;
                TG_Top_Frame = temp->prior;
                temp->prior = nullptr;  // we don't "drop" it, but...
                // !!! leave flag or reset it?
            }
            else {
                Drop_Frame(F);  // frees feed
            }

            if (F->original) {
                //
                // !!! As written we call back the Eval_Action() code, with
                // or without an "ACTION_FOLLOWUP" flag.
                //
                if (NOT_EVAL_FLAG(F, DELEGATE_CONTROL))
                    SET_EVAL_FLAG(F, ACTION_FOLLOWUP);
                else
                    assert(NOT_EVAL_FLAG(F, ACTION_FOLLOWUP));
                goto loop;
            }
            else {
                if (NOT_EVAL_FLAG(F, DELEGATE_CONTROL))
                    goto loop;
            }
            Drop_Frame(F);
        }

        while (
            F != start
            and (NOT_EVAL_FLAG(F, CONTINUATION) or GET_EVAL_FLAG(F, TO_END))
        ){
            Drop_Frame(F);
        }
    }

    if (F != start)
        goto loop;

    if (r == F->out)
        return false;  // not thrown
    assert(r == R_THROWN);
    return true;  // thrown
}
