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
bool Trampoline_Throws(REBFRM *f)
{
    // The instigating call to this function cannot be unwound across, as it
    // represents a "stackful" invocation of the evaluator.  YIELD must know
    // the passed-in frame is uncrossable, so that it can raise an error if
    // you try to unwind the Revolt stack across a top-level Trampoline call.
    //
    // (It's more efficient for the caller to set the bit in one assignment
    // with the other header bits it sets--so just have the debug build check
    // to make sure they did so.)
    //
    // !!! There could be a "promise" variant which didn't expect a concrete
    // result back, but was willing to accept a frame stack that would run
    // later to provide the result.  For now, we consider this a barrier.
    //
    assert(GET_EVAL_FLAG(f, ROOT_FRAME));

    // In theory, a caller could push several frames to be evaluated, and
    // the passed in `f` would just be where evaluation should *stop*.  No
    // cases of this exist yet, but the `f = FS_TOP` below would allow it.
    //
    assert(f == FS_TOP);

  push_again: ;

    // There is only one setjmp() point for each trampoline invocation.  Any
    // frame that is interrupted at an arbitrary moment by a fail() will be
    // "teleported" up to this point.  The running C stack variables will be
    // lost, but the Revolt frame stack will still be intact.
    //
    // Only the topmost frame may raise an error.  This means that if a frame
    // pushes another frame to do work with EVAL_FLAG_TRAMPOLINE_KEEPALIVE,
    // that must be dropped before failing.
    //
    // A *cooperative* failure is done by raising the error and returning it
    // like a throw.  This form of failure assumes balance in the frame was
    // achieved before returning, and the frame will be considered done.  If
    // EVAL_FLAG_TRAMPOLINE_KEEPALIVE wasn't used, it will be dropped.
    //
    // On the other hand, an *uncooperative* failure can happen at any moment,
    // even due to something like a failed memory allocation requested by
    // the executor itself.  As evidenced by fail()s in an Action_Executor()
    // which are caused by subdispatch to a native, the executor must get a
    // chance to clean up after fails that happen on its watch.

    struct Reb_Jump jump;
    PUSH_TRAP_SO_FAIL_CAN_JUMP_BACK_HERE(&jump);

    // The first time through the following code 'error' will be null, but...
    // `fail` can longjmp here, so 'error' won't be null *if* that happens!
    //
    if (jump.error) {

        // The mechanisms for THROW-ing and FAIL-ing are somewhat unified in
        // stackless...(a TRAPpable failure is just any "thrown" value with
        // a VAL_THROWN_LABEL() which is an ERROR!).  So the trampoline just
        // converts the longjmp into a throw.

        Init_Thrown_With_Label(
            FS_TOP->out,
            NULLED_CELL,  // no "thrown value"
            CTX_ARCHETYPE(jump.error)  // only the ERROR! as a label
        );

        goto push_again;
    }

    // This assignment is needed to avoid `f could be clobbered by longjmp`
    // warning (see also note about how it would facilitate a caller who
    // pushed more stack levels and didn't pass FS_TOP as initial parameter).
    //
    f = FS_TOP;

  loop:

    UPDATE_TICK_DEBUG(f, nullptr);

    // v-- This is the TG_Break_At_Tick or C-DEBUG-BREAK landing spot --v

    assert((f->executor == &Action_Executor) == (f->original != nullptr));

    // CALL THE EXECUTOR
    //
    // It is expected that all executors are able to handle the Is_Throwing()
    // state, even if just to pass it through.  The executor may push more
    // frames or change the executor of the frame it receives.
    //
    REB_R r = (f->executor)(f);  // Note: f may not be FS_TOP at this moment

    if (r == R_CONTINUATION) {
        //
        // The frame stack is singly-linked from lower stacks to higher.  Yet
        // the GC needs to find the bottom of stacks when sweeping, in order
        // to gracefully unwind suspended stacks (e.g. a GENERATOR' YIELD)
        // that have not been marked as "in use".
        //
        // A cheap concept which also helps a bit with error checking is to
        // say that all continuations have nonzero state bytes.  Then things
        // like YIELD will be at state byte zero: the root of an unwind.
        //
        assert(f == FS_TOP or STATE_BYTE(f) != 0);
    }

    f = FS_TOP;  // refresh to whatever topmost frame is after call

    if (r == R_THROWN) {
        //
        // When an executor does `return R_THROWN;` cooperatively, it is
        // expected that it has balanced all of its API handles and memory
        // allocations.  The executor is changed to a "trash" pointer to
        // indicate it did not end normally and should not be called again
        // (distinct from the 'nullptr' which signals normal execution done).
        // This is because the trashing is not necessary in release builds
        //
        assert(f->executor != nullptr);
        assert(not IS_POINTER_TRASH_DEBUG(f->executor));
        TRASH_POINTER_IF_DEBUG(f->executor);
    }
    else
        assert((f->executor == &Action_Executor) == (f->original != nullptr));

    assert(Eval_Count >= 0);
    if (--Eval_Count == 0) {
        //
        // Note that Do_Signals_Throws() may do a recycle step of the GC,
        // or may spawn an entire interactive debugging session via
        // breakpoint before it returns.  May also FAIL and longjmp out.
        //
        // We can't just test on the `nullptr` case of finishing an executor
        // result, because that would not provide termination in something
        // that was deeply tunneling with no resolution.
        //
        // The FRM_SPARE() is passed in to be used for the location to write
        // a throw, but shouldn't be written unless a throw happens...because
        // the spare cell is in use by the executor.
        //
        if (Do_Signals_Throws(FRM_SPARE(f))) {  // see note on FRM_SPARE()
            Move_Value(f->out, FRM_SPARE(f));
            r = R_THROWN;
            goto thrown;
        }
    }

    if (r == R_CONTINUATION) {
        assert(f->executor != nullptr);  // *topmost* frame needs callback
        goto loop;  // keep going
    }

    if (f->executor == nullptr) {  // no further execution for frame, drop it
        assert(r == f->out);

        // !!! Currently we do not drop the topmost frame, because some code
        // (e.g. MATCH) would ask for a frame to be filled, and then steal
        // its resulting varlist.  However, if MATCH is on the stack when it
        // makes the call, it's not stackless...e.g. it should be written
        // some other way.
        //
        if (GET_EVAL_FLAG(f, ROOT_FRAME)) {
            DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&jump);
            return false;
        }

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

  #if !defined(NDEBUG)
    if (not f->original)
        Eval_Core_Exit_Checks_Debug(f);   // called unless a fail() longjmps
  #endif

    if (r == R_THROWN) {
      thrown:
        if (GET_EVAL_FLAG(f, ROOT_FRAME)) {
            assert(NOT_EVAL_FLAG(f, TRAMPOLINE_KEEPALIVE));  // always kept
            DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&jump);
            return true;
        }

        if (GET_EVAL_FLAG(f, TRAMPOLINE_KEEPALIVE))
            f = f->prior;
        else {
            Abort_Frame(f);
            f = FS_TOP;  // refresh
        }
    }
    else {
        // !!! This is going to be the right place to handle other variants of
        // return values consistently, e.g. API handles.  The return results
        // from native dispatchers may be specific to interactions.

        assert(r == f->out);
        assert(IS_SPECIFIC(cast(RELVAL*, f->out)));

        // Want to keep this flag between an operation and an ensuing enfix in
        // the same frame, so can't clear in Drop_Action(), e.g. due to:
        //
        //     left-lit: enfix :lit
        //     o: make object! [f: does [1]]
        //     o/f left-lit  ; want error suggesting -> here, need flag for that
        //
        /* // !!! v-- Said this originally, but stackless seemed to not work
        CLEAR_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH);
        assert(NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT));  // must be consumed
        */
        if (GET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT)) {
            assert(GET_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH));
            fail (Error_Literal_Left_Path_Raw());
        }

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
    }

    goto loop;
}
