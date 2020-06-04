//
//  File: %d-trace.c
//  Summary: "Tracing Debug Routines"
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
// TRACE is functionality that was in R3-Alpha for doing low-level tracing.
// It could be turned on with `trace on` and off with `trace off`.  While
// it was on, it would print out information about the current execution step.
//
// Revolt's goal is to have a fully-featured debugger that should allow a
// TRACE-like facility to be written and customized by the user.  They would
// be able to get access on each step to the call frame, and control the
// evaluator from within.
//
// A lower-level trace facility may still be interesting even then, for
// "debugging the debugger".  Either way, the feature is fully decoupled from
// %c-eval.c, and the system could be compiled without it (or it could be
// done as an extension).
//

#include "sys-core.h"


//
//  Dispatch_Internal: C
//
// !!! A concept used to start developing usermode debugging and tracing
// constructs was that they would be able to "hook" the central function
// calling mechanics by wrapping them.  This would be somewhat similar to
// an ENCLOSE, where the frame was available for each function call to
// inspect.  The idea of doing this hooking by replacing C functions and
// then calling C functions is intrinsically flawed in a stackless model,
// though the code for the old demos depending on the technique is not
// deleted yet...as it might be helpful in rethinking how the demos would
// work more generally in a stackless model.  Having a stub for this routine
// around keeps the broken code compiling.
//
REB_R Dispatch_Internal(REBFRM * const f) {
    UNUSED(f);
    panic("Dispatch_Internal() is deprecated");
}


enum {
    TRACE_FLAG_FUNCTION = 1 << 0
};


//
//  Eval_Depth: C
//
REBINT Eval_Depth(void)
{
    REBINT depth = 0;
    REBFRM *frame = FS_TOP;

    for (; frame != FS_BOTTOM; frame = FRM_PRIOR(frame), depth++)
        NOOP;

    return depth;
}


//
//  Frame_At_Depth: C
//
REBFRM *Frame_At_Depth(REBLEN n)
{
    REBFRM *frame = FS_TOP;

    while (frame) {
        if (n == 0) return frame;

        --n;
        frame = FRM_PRIOR(frame);
    }

    return NULL;
}


//
//  Trace_Value: C
//
void Trace_Value(
    const char* label, // currently "match" or "input"
    const RELVAL *value
){
    // !!! The way the parse code is currently organized, the value passed
    // in is a relative value.  It would take some changing to get a specific
    // value, but that's needed by the API.  Molding can be done on just a
    // relative value, however.

    DECLARE_MOLD (mo);
    Push_Mold(mo);
    Mold_Value(mo, value);

    DECLARE_LOCAL (molded);
    Init_Text(molded, Pop_Molded_String(mo));
    PUSH_GC_GUARD(molded);

    rebElide("print [",
        "{Parse}", rebT(label), "{:}", molded,
    "]", rebEND);

    DROP_GC_GUARD(molded);
}


//
//  Trace_Parse_Input: C
//
void Trace_Parse_Input(const REBVAL *str)
{
    if (IS_END(str)) {
        rebElide("print {Parse Input: ** END **}", rebEND);
        return;
    }

    rebElide("print [",
        "{Parse input:} mold/limit", str, "60"
    "]", rebEND);
}


REBVAL *Trace_Eval_Dangerous(REBFRM *f)
{
    int depth = Eval_Depth() - Trace_Depth;
    if (depth > 10)
        depth = 10; // don't indent so far it goes off the screen

    DECLARE_LOCAL (v);
    Derelativize(v, f->feed->value, f->feed->specifier);

    rebElide("loop 4 *", rebI(depth), "[write-stdout space]", rebEND);

    if (FRM_IS_VALIST(f)) {
        //
        // If you are doing a sequence of REBVAL* held in a C va_list,
        // it doesn't have an "index".  It could manufacture one if
        // you reified it (which will be necessary for any inspections
        // beyond the current element), but TRACE does not currently
        // output more than one unit of lookahead.
        //
        rebElide("write-stdout spaced [",
            "{va:} mold/limit", v, "50"
        "]", rebEND);
    }
    else {
        rebElide("write-stdout spaced [",
            rebI(FRM_INDEX(f)), "{:} mold/limit", v, "50",
        "]", rebEND);
    }

    if (IS_WORD(v) or IS_GET_WORD(v)) {
        //
        // Note: \\ -> \ in C, because backslashes are escaped
        //
        const REBVAL *var = Try_Lookup_Word(v, SPECIFIED);
        if (not var) {
            rebElide("write-stdout { : \\\\end\\\\}", rebEND);
        }
        else if (IS_NULLED(var)) {
            rebElide("write-stdout { : \\\\null\\\\}", rebEND);
        }
        else if (IS_ACTION(var)) {
            rebElide("write-stdout spaced [",
                "{ : ACTION!} mold/limit parameters of", var, "50",
            "]", rebEND);
        }
        else if (
            ANY_WORD(var)
            or ANY_STRING(var)
            or ANY_ARRAY(var)
            or ANY_SCALAR(var)
            or IS_DATE(var)
            or IS_TIME(var)
            or IS_BLANK(var)
        ){
            // These are things that are printed, abbreviated to 50
            // characters of molding.
            //
            rebElide("write-stdout spaced [",
                "{ :} mold/limit", var, "50",
            "]", rebEND);
        }
        else {
            // Just print the type if it's a context, GOB!, etc.
            //
            rebElide("write-stdout spaced [",
                "{ :} type of", var,
            "]", rebEND);
        }
    }
    rebElide("write-stdout newline", rebEND);
    return nullptr;
}


//
//  Traced_Eval_Hook_Throws: C
//
// Ultimately there will be two trace codebases...one that will be low-level
// and printf()-based, only available in debug builds, and it will be able to
// trace all the way from the start.  Then there will be a trace that is in
// usermode with many features--but that uses functions like PRINT and would
// not be able to run during bootup.
//
// For the moment, this hook is neither.  It can't be run during boot, and
// it doesn't use printf, but relies on features not exposed to usermode.  As
// the debug and hooking API matures this should be split into the two forms.
//
bool Traced_Eval_Hook_Throws(REBFRM *f_stop)
{
    UNUSED(f_stop);

    REBFRM *f = FS_TOP;

    int depth = Eval_Depth() - Trace_Depth;
    if (depth < 0 || depth >= Trace_Level)
        return Trampoline_Throws(f);  // (REPL uses to hide)

    SHORTHAND (v, f->feed->value, NEVERNULL(const RELVAL*));

    if (depth > 10)
        depth = 10; // don't indent so far it goes off the screen

    // We're running, so while we're running we shouldn't hook again until
    // a dispatch says we're running the traced dispatcher.
    //
    assert(PG_Trampoline_Throws == &Traced_Eval_Hook_Throws);
    PG_Trampoline_Throws = &Trampoline_Throws;

    if (not (
        KIND_BYTE(*v) == REB_ACTION
        or (Trace_Flags & TRACE_FLAG_FUNCTION)
    )){
        REBVAL *err = rebRescue(cast(REBDNG*, &Trace_Eval_Dangerous), f);

      #if defined(DEBUG_HAS_PROBE)
        if (err) { PROBE(err); }
      #endif
        assert(not err);  // should not raise error!
        UNUSED(err);
    }

    // We put the Traced_Dispatcher() into effect.  It knows to turn the
    // eval hook back on when it dispatches, but it doesn't want to do
    // it until then (otherwise it would trace its own PRINTs!).
    //
    REBNAT saved_dispatch_hook = PG_Dispatch;
    PG_Dispatch = &Traced_Dispatch_Hook;

    bool threw = Trampoline_Throws(f);

    PG_Dispatch = saved_dispatch_hook;

    PG_Trampoline_Throws = &Traced_Eval_Hook_Throws;
    return threw;
}


REBVAL *Trace_Action_Dangerous(REBFRM *f)
{
    int depth = Eval_Depth() - Trace_Depth;
    if (depth > 10)
        depth = 10; // don't indent so far it goes off the screen

    rebElide("loop 4 *", rebI(depth), "[write-stdout space]", rebEND);
    rebElide("write-stdout spaced [",
        "{-->}", rebT(Frame_Label_Or_Anonymous_UTF8(f)),
    "]", rebEND);

    if (Trace_Flags & TRACE_FLAG_FUNCTION)
        rebElide("TBD Dump FRM_ARG(FS_TOP, 1), FRM_NUM_ARGS(FS_TOP)", rebEND);
    else
        rebElide("write-stdout newline", rebEND);

    return nullptr;
}


struct Reb_Return_Descriptor {
    REBFRM *f;
    const REBVAL *r;
};

REBVAL *Trace_Return_Dangerous(struct Reb_Return_Descriptor *d)
{
    REBFRM *f = d->f;
    const REBVAL *r = d->r;

    int depth = Eval_Depth() - Trace_Depth;
    if (depth > 10)
        depth = 10; // don't indent so far it goes off the screen

    rebElide("loop 4 *", rebI(depth), "[write-stdout space]", rebEND);
    rebElide("write-stdout spaced [",
        "{<--}", rebT(Frame_Label_Or_Anonymous_UTF8(f)), "{==} space",
    "]", rebEND);

    if (r == f->out) {

      process_out:;

        if (r != R_THROWN) {
            rebElide(
                "write-stdout mold/limit", f->out, "50",
                "write-stdout newline",
            rebEND);
            return nullptr;
        }

        // The system guards against the molding or forming of thrown
        // values, which are actually a pairing of label + value.
        // "Catch" it temporarily, long enough to output it, then
        // re-throw it.
        //
        DECLARE_LOCAL (arg);
        CATCH_THROWN(arg, f->out);

        if (IS_NULLED(f->out)) {
            rebElide("print ["
                "{throw} mold/limit", arg, "50",
            "]", rebEND);
        }
        else {
            rebElide("print [",
                "{throw} mold/limit", arg, "30 {,}",
                "{label} mold/limit", f->out, "20"
            "]", rebEND);
        }

        Init_Thrown_With_Label(f->out, arg, f->arg);
    }
    else if (not r) { // -> "\null\", backslash escaped
        rebElide("print {\\null\\}", rebEND);
    }
    else if (GET_CELL_FLAG(r, ROOT)) {
        Handle_Api_Dispatcher_Result(f, r);
        goto process_out;
    }
    else switch (KIND_BYTE(r)) {
      case REB_R_INVISIBLE:  // -> "\invisible\", backslash escaped
        rebElide("print {\\invisible\\}", rebEND);
        break;

      case REB_R_REFERENCE:
      case REB_R_IMMEDIATE:
        assert(false); // internal use only, shouldn't be returned
        break;

      default:
        assert(false);
    }

    return nullptr;
}


//
//  Traced_Dispatch_Hook: C
//
// This is the function which is swapped in for Dispatch_Internal() when
// tracing is enabled.
//
REB_R Traced_Dispatch_Hook(REBFRM * const f)
{
    int depth = Eval_Depth() - Trace_Depth;
    if (depth < 0 || depth >= Trace_Level)
        return Dispatch_Internal(f);

    PG_Dispatch = &Dispatch_Internal;  // don't trace the trace!

    REBACT *phase = FRM_PHASE(f);

    if (phase == f->original) {
        //
        // Only show the label if this phase is the first phase.

        REBVAL *err = rebRescue(cast(REBDNG*, Trace_Action_Dangerous), f);
        assert(not err);
        UNUSED(err);
    }

    // We can only tell if it's the last phase *before* the apply, because if
    // we check *after* it may change to become the last and need R_REDO_XXX.
    //
    bool last_phase = (ACT_UNDERLYING(phase) == phase);

    REBEVL *saved_eval = PG_Trampoline_Throws;
    PG_Trampoline_Throws = &Traced_Eval_Hook_Throws;

    REB_R r = Dispatch_Internal(f);

    PG_Trampoline_Throws = saved_eval;

/*    if (PG_Dispatch != Traced_Dispatch_Hook)
        return r; // TRACE OFF during the traced code, don't print any more
        */

    // When you HIJACK a function with an incompatible frame, it can REDO
    // even on what looks like the "last phase" because it is wiring in a new
    // function.  Review ramifications of this, and whether it should be
    // exposed vs. skipped as "not the last phase" (e.g. the function with
    // this frame's label will still be running, not running under a new name)
    //
    if (KIND_BYTE(r) == REB_R_REDO) {
        const bool checked = EXTRA(Any, r).flag;
        if (not checked)
            last_phase = false;
    }

    if (last_phase) {
        //
        // Only show the return result if this is the last phase.

        struct Reb_Return_Descriptor d;
        d.f = f;
        d.r = r;

        REBVAL *err = rebRescue(cast(REBDNG*, Trace_Return_Dangerous), &d);
        assert(not err);
        UNUSED(err);
    }

    PG_Dispatch = &Traced_Dispatch_Hook;

    return r;
}


//
//  trace: native [
//
//  {Enables and disables evaluation tracing and backtrace.}
//
//      return: [<opt>]
//      mode [integer! logic!]
//      /function
//          "Traces functions only (less output)"
//  ]
//
REBNATIVE(trace)
//
// !!! R3-Alpha had a kind of interesting concept of storing the backtrace in
// a buffer, up to a certain number of lines.  So it wouldn't be visible and
// interfering with your interactive typing, but you could ask for lines out
// of it after the fact.  This makes more sense as a usermode feature, where
// the backtrace is stored structurally, vs trying to implement in C.
//
{
    INCLUDE_PARAMS_OF_TRACE;

    REBVAL *mode = ARG(mode);

    Check_Security_Placeholder(Canon(SYM_DEBUG), SYM_READ, 0);

    // Set the trace level:
    if (IS_LOGIC(mode))
        Trace_Level = VAL_LOGIC(mode) ? 100000 : 0;
    else
        Trace_Level = Int32(mode);

    if (Trace_Level) {
        PG_Trampoline_Throws = &Traced_Eval_Hook_Throws;

        if (REF(function))
            Trace_Flags |= TRACE_FLAG_FUNCTION;
        Trace_Depth = Eval_Depth() - 1; // subtract current TRACE frame
    }
    else
        PG_Trampoline_Throws = &Trampoline_Throws;

    return nullptr;
}
