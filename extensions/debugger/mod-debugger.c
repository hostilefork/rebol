//
//  File: %mod-debugger.c
//  Summary: "Native Functions for debugging"
//  Section: Extension
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2015-2019 Revolt Open Source Contributors
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
// One goal of Revolt's debugger is to have as much of it possible written in
// usermode code, and be easy to hack on and automate.
//
// This file contains interactive debugging support for breaking and
// resuming.  The instructions BREAKPOINT and PAUSE are natives which will
// invoke the CONSOLE function to start an interactive session.  During that
// time Revolt functions may continue to be called, though there is a sandbox
// which prevents the code from throwing or causing errors which will
// propagate past the breakpoint.  The only way to resume normal operation
// is with a "resume instruction".
//
// Hence RESUME and QUIT should be the only ways to get out of the breakpoint.
// Note that RESUME/DO provides a loophole, where it's possible to run code
// that performs a THROW or FAIL which is not trapped by the sandbox.
//

#include "sys-core.h"

#include "tmp-mod-debugger.h"


//
//  Do_Breakpoint_Throws: C
//
// A call to Do_Breakpoint_Throws will call the CONSOLE function.  The RESUME
// native cooperates with the CONSOLE by being able to give back a value (or
// give back code to run to produce a value) that the breakpoint returns.
//
// !!! RESUME had another feature, which is to be able to actually unwind and
// simulate a return /AT a function *further up the stack*.  For the moment
// this is not implemented.
//
bool Do_Breakpoint_Throws(
    REBVAL *out,
    bool interrupted,  // Ctrl-C (as opposed to a BREAKPOINT)
    const REBVAL *paused
){
    UNUSED(interrupted);  // !!! not passed to the REPL, should it be?
    UNUSED(paused);  // !!! feature TBD

    // !!! The unfinished SECURE extension would supposedly either be checked
    // here (or inject a check with HIJACK on BREAKPOINT) to make sure that
    // debugging was allowed.  Review doing that check here.

    REBVAL *inst = rebValue("debug-console", rebEND);

    if (IS_INTEGER(inst)) {
        Init_Thrown_With_Label(out, inst, NATIVE_VAL(quit));
        rebRelease(inst);
        return true;
    }

    // This is a request to install an evaluator hook.  For instance, the
    // STEP command wants to interject some monitoring to the evaluator, but
    // it does not want to do so until it is at the point of resuming the
    // code that was executing when the breakpoint hit.
    //
    if (IS_HANDLE(inst)) {
        CFUNC *cfunc = VAL_HANDLE_CFUNC(inst);
        rebRelease(inst);

        // !!! Evaluator hooking is a very experimental concept, and there's
        // no rigor yet for supporting more than one hook at a time.
        //
        assert(
            PG_Trampoline_Throws == &Trampoline_Throws
        );

        PG_Trampoline_Throws = cast(REBEVL*, cfunc);
        Init_Void(out);
        return false;  // no throw, run normally (but now, hooked)
    }

    // If we get an @( ) back, that's a request to run the code outside of
    // the console's sandbox and return its result.  It's possible to use
    // quoting to return simple values, like @('x)

    assert(IS_SYM_GROUP(inst));

    bool threw = Do_Any_Array_At_Throws(out, inst, SPECIFIED);

    rebRelease(inst);

    return threw;  // act as if the BREAKPOINT call itself threw
}


//
//  export breakpoint*: native [
//
//  "Signal breakpoint to the host, but do not participate in evaluation"
//
//      return: []
//          {Returns nothing, not even void ("invisible", like COMMENT)}
//  ]
//
REBNATIVE(breakpoint_p)
//
// !!! Need definition to test for N_DEBUGGER_breakpoint function
{
    if (Do_Breakpoint_Throws(
        D_OUT,
        false,  // not a Ctrl-C, it's an actual BREAKPOINT
        VOID_VALUE  // default result if RESUME does not override
    )){
        return R_THROWN;
    }

    // !!! Should use a more specific protocol (e.g. pass in END).  But also,
    // this provides a possible motivating case for functions to be able to
    // return *either* a value or no-value...if breakpoint were variadic, it
    // could splice in a value in place of what comes after it.
    //
    if (not IS_VOID(D_OUT))
        fail ("BREAKPOINT is invisible, can't RESUME/WITH code (use PAUSE)");

    return R_INVISIBLE;
}


//
//  export pause: native [
//
//  "Pause in the debugger before running the provided code"
//
//      return: [<opt> any-value!]
//          "Result of the code evaluation, or RESUME/WITH value if override"
//      :code [group!]  ; or LIT-WORD! name or BLOCK! for dialect
//          "Run the given code if breakpoint does not override"
//  ]
//
REBNATIVE(pause)
//
// !!! Need definition to test for N_DEBUGGER_pause function
{
    DEBUGGER_INCLUDE_PARAMS_OF_PAUSE;

    if (Do_Breakpoint_Throws(
        D_OUT,
        false,  // not a Ctrl-C, it's an actual BREAKPOINT
        ARG(code)  // default result if RESUME does not override
    )){
        return R_THROWN;
    }

    return D_OUT;
}


//
//  export resume: native [
//
//  {Resume after a breakpoint, can evaluate code in the breaking context.}
//
//      expression "Evalue the given code as return value from BREAKPOINT"
//          [<end> block!]
//  ]
//
REBNATIVE(resume)
//
// The CONSOLE makes a wall to prevent arbitrary THROWs and FAILs from ending
// a level of interactive inspection.  But RESUME is special, (with a throw
// /NAME of the RESUME native) to signal an end to the interactive session.
//
// When the BREAKPOINT native gets control back from CONSOLE, it evaluates
// a given expression.
//
// !!! Initially, this supported /AT:
//
//      /at
//          "Return from another call up stack besides the breakpoint"
//      level [frame! action! integer!]
//          "Stack level to target in unwinding (can be BACKTRACE #)"
//
// While an interesting feature, it's not currently a priority.  (It can be
// accomplished with something like `resume [unwind ...]`)
{
    DEBUGGER_INCLUDE_PARAMS_OF_RESUME;

    REBVAL *expr = ARG(expression);
    if (IS_NULLED(expr))  // e.g. <end> (actuall null not legal)
        Init_Any_Array(expr, REB_SYM_GROUP, EMPTY_ARRAY);
    else {
        assert(IS_BLOCK(expr));
        mutable_KIND_BYTE(expr) = mutable_MIRROR_BYTE(expr) = REB_SYM_GROUP;
    }

    // We throw with /NAME as identity of the RESUME function.  (Note: there
    // is no NATIVE_VAL() for extensions, yet...extract from current frame.)
    //
    DECLARE_LOCAL (resume);
    Init_Action_Maybe_Bound(resume, FRM_PHASE(frame_), FRM_BINDING(frame_));

    // We don't want to run the expression yet.  If we tried to run code from
    // this stack level--and it failed or threw--we'd stay stuck in the
    // breakpoint's sandbox.  We throw it as-is and it gets evaluated later.
    //
    return Init_Thrown_With_Label(D_OUT, expr, resume);
}


// !!! With the emergence of stackless, this was no longer used except in
// the (currently broken) debugger, which will need a complete overhaul.
//
static bool Reevaluate_In_Subframe_Maybe_Stale_Throws(
    REBVAL *out,
    REBFRM *f,
    const REBVAL *reval,
    REBFLGS flags
){
    DECLARE_FRAME (subframe, f->feed, flags);
    subframe->executor = &Reevaluation_Executor;
    subframe->u.reval.value = reval;

    Push_Frame(out, subframe);
    bool threw = (*PG_Trampoline_Throws)(subframe);
    Drop_Frame(subframe);

    return threw;
}


REBVAL *Spawn_Interrupt_Dangerous(void *opaque)
{
    REBFRM *f = cast(REBFRM*, opaque);

    REBVAL *interrupt = rebValue(":interrupt", rebEND);

    // In SHOVE it passes EVAL_FLAG_NEXT_ARG_FROM_OUT.  We don't have a reason
    // to do this if we pass interrupt via reevaluate.
    //
    REBFLGS flags = EVAL_MASK_DEFAULT;

    f->feed->gotten = nullptr;  // calling arbitrary code, may disrupt

    // This is calling an invisible, so it should not change f->out!
    //
    if (Reevaluate_In_Subframe_Maybe_Stale_Throws(
        f->out,
        f,
        interrupt,
        flags
    )){
        rebRelease(interrupt);  // ok if nullptr
        return R_THROWN;
    }

    rebRelease(interrupt);
    return nullptr;
}


// It might seem that the "evaluator hook" could be a usermode function which
// took a FRAME! as an argument.  This is true, but it would be invasive...
// it would appear to be on the stack.  It would be a complex illusion to
// work past.
//
// A nicer way of doing this would involve freezing the evaluator thread and
// then passing control to a debugger thread, which had its own stack that
// would not interfere.  But in a single-threaded model, we make sure we
// don't add any stack levels in the hoook.
//
bool Stepper_Eval_Hook_Throws(REBFRM *f_stop)
{
    UNUSED(f_stop);

    REBFRM *f = FS_TOP;

    // At the moment, the only thing the stepper eval hook does is set a
    // signal for a breakpoint to happen on the *next* instruction.
    //
    // This could be done with SIG_INTERRUPT.  Though it's not clear if we
    // could just go ahead and run the breakpoint here (?)  The evaluator
    // has finished a step.

    // The stepper removes itself from evaluation because it wants to count
    // "whole steps".  So if you say `print 1 + 2`, right now that will break
    // after the whole expression is done.
    //
    PG_Trampoline_Throws = &Trampoline_Throws;

    bool threw = Trampoline_Throws(f);

    // !!! We cannot run more code while in a thrown state, hence we could not
    // invoke a nested console after a throw.  We have to either set a global
    // variable requesting to break after the throw's jump.  -or- we can save
    // the thrown state, spawn the console, and rethrow what we caught.  This
    // is experimental code and dealing with what may be a fool's errand in
    // the first place (a usermode debugger giving a coherent experience on
    // one stack--no separate thread/stack for the debugger).  But for now,
    // we freeze the thrown state and then rethrow.
    //
    DECLARE_LOCAL (thrown_label);
    DECLARE_LOCAL (thrown_value);
    if (threw) {
        Move_Value(thrown_label, VAL_THROWN_LABEL(f->out));
        CATCH_THROWN(thrown_value, f->out);
        PUSH_GC_GUARD(thrown_label);
        PUSH_GC_GUARD(thrown_value);
    }

    // !!! The API code (e.g. for Alloc_Value()) needs a reified frame in
    // order to get a REBCTX* to attach API handles to.  However, we may be
    // in the process of fulfilling a function frame...and forming a REBCTX*
    // out of a partial frame is illegal (not all cells are filled, they have
    // not even had their memory initialized).
    //
    // Hence we need to make a frame that isn't fulfilling to parent those
    // handles to.  rebRescue() already does that work, so reuse it.
    //
    REBVAL *r = rebRescue(&Spawn_Interrupt_Dangerous, f);
    if (threw) {
        DROP_GC_GUARD(thrown_value);
        DROP_GC_GUARD(thrown_label);
    }

    if (r == R_THROWN)
        return true;  // beats rethrowing whatever execution throw there was

    if (threw)
        Init_Thrown_With_Label(f->out, thrown_value, thrown_label);

    return threw;
}


//
//  export step: native [
//
//  "Perform a step in the debugger"
//
//      return: [<void>]
//      amount [<end> word! integer!]
//          "Number of steps to take (default is 1) or IN, OUT, OVER"
//  ]
//
REBNATIVE(step)
{
    DEBUGGER_INCLUDE_PARAMS_OF_STEP;

    REBVAL *amount = ARG(amount);
    if (IS_NULLED(amount))
        Init_Integer(amount, 1);

    if (not IS_INTEGER(amount) and VAL_INT32(amount) == 1)
        fail ("STEP is just getting started, can only STEP by 1");

    // !!! The way stepping is supposed to work is to be able to hook the
    // evaluator and check to see if the condition it's checking is met.
    // This means doing something like a RESUME, but as part of that resume
    // giving a hook to install.  The hook looks like the evaluator itself...
    // it takes a REBFRM* and has to call the evaluator at some point.
    //
    DECLARE_LOCAL(hook);
    Init_Handle_Cfunc(hook, cast(CFUNC*, &Stepper_Eval_Hook_Throws));

    // We throw with /NAME as identity of the RESUME function.  (There is no
    // NATIVE_VAL() for extensions at this time.)
    //
    REBVAL *resume = rebValue(":resume", rebEND);

    REBVAL *thrown = Init_Thrown_With_Label(D_OUT, hook, resume);
    rebRelease(resume);

    // !!! It would be nice to be able to have a step over or step out return
    // the value evaluated to.  This value would have to be passed to the
    // spawned console loop when it restarted, however...as this needs to
    // throw the hook we're going to install.
    //
    return thrown;
}
