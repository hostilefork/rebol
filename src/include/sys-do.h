//
//  File: %sys-do.h
//  Summary: {DO-until-end (of block or variadic feed) evaluation API}
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Revolt Open Source Contributors
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
// The "DO" helpers have names like Do_XXX(), and are a convenience layer
// over making repeated calls into the Eval_XXX() routines.  DO-ing things
// always implies running to the end of an input.  It also implies returning
// a VOID! value if nothing can be synthesized, but letting the last null
// or value fall out otherwise:
//
//     >> type of do []
//     == void!
//
//     >> type of do [comment "hi"]
//     == void!
//
//     >> do [1 comment "hi"]
//     == 1
//
//    >> do [null comment "hi"]
//    ; null
//
// See %sys-eval.h for the lower level routines if this isn't enough control.
//


// This helper routine is able to take an arbitrary input cell to start with
// that may not be VOID!.  It is code that DO shares with GROUP! evaluation
// in Eval_Core()--where being able to know if a group "completely vaporized"
// is important as distinct from an expression evaluating to void.
//
inline static bool Do_Feed_To_End_Maybe_Stale_Throws(
    REBVAL *out,  // must be initialized, unchanged if all empty/invisible
    REBFED *feed,  // feed mechanics always call va_end() if va_list
    REBFLGS flags
){
    DECLARE_FRAME (f, feed, flags | EVAL_FLAG_ROOT_FRAME);

    bool threw;
    Push_Frame(out, f);
    do {
        f->executor = &New_Expression_Executor;
        threw = (*PG_Trampoline_Throws)(f);
    } while (not threw and NOT_END(feed->value));
    Drop_Frame(f);

    return threw;
}


inline static bool Do_Any_Array_At_Throws(
    REBVAL *out,
    const RELVAL *any_array,  // same as `out` is allowed
    REBSPC *specifier
){
    DECLARE_FEED_AT_CORE (feed, any_array, specifier);

    Init_Void(out);  // ^-- *after* feed initialization (if any_array == out)

    bool threw = Do_Feed_To_End_Maybe_Stale_Throws(
        out,
        feed,
        EVAL_MASK_DEFAULT | EVAL_FLAG_ALLOCATED_FEED
    );
    CLEAR_CELL_FLAG(out, OUT_MARKED_STALE);
    return threw;
}


// !!! When working with an array outside of the context of a REBVAL it was
// extracted from, then that means automatic determination of the CONST rules
// isn't possible.  This primitive is currently used in a few places where
// the desire is not to inherit any "wave of constness" from the parent's
// frame, or from a value.  The cases need review--in particular the use for
// the kind of shady frame translations used by HIJACK and ports.
//
inline static bool Do_At_Mutable_Maybe_Stale_Throws(
    REBVAL *out,
    const RELVAL *opt_first,  // optional element to inject *before* the array
    REBARR *array,
    REBLEN index,
    REBSPC *specifier  // must match array, but also opt_first if relative
){
    REBFED feed_struct;  // opt_first so can't use DECLARE_ARRAY_FEED
    REBFED *feed = &feed_struct;
    Prep_Array_Feed(
        feed,
        opt_first,
        array,
        index,
        specifier,
        FEED_MASK_DEFAULT  // different: does not 
    );

    return Do_Feed_To_End_Maybe_Stale_Throws(out, feed, EVAL_MASK_DEFAULT);
}

inline static bool Do_At_Mutable_Throws(
    REBVAL *out,
    REBARR *array,
    REBLEN index,
    REBSPC *specifier
){
    Init_Void(out);

    bool threw = Do_At_Mutable_Maybe_Stale_Throws(
        out,
        nullptr,
        array,
        index,
        specifier
    );
    CLEAR_CELL_FLAG(out, OUT_MARKED_STALE);
    return threw;
}


// Takes a list of arguments terminated by an end marker and will do something
// similar to R3-Alpha's "apply/only" with a value.  If that value is a
// function, it will be called...if it's a SET-WORD! it will be assigned, etc.
//
// This is equivalent to putting the value at the head of the input and
// then calling EVAL/ONLY on it.  If all the inputs are not consumed, an
// error will be thrown.
//
inline static bool RunQ_Throws(
    REBVAL *out,
    bool fully,
    const void *p,  // last param before ... mentioned in va_start()
    ...
){
    va_list va;
    va_start(va, p);

    bool threw = Eval_Step_In_Va_Throws_Core(
        SET_END(out),  // start at END to detect error if no eval product
        FEED_MASK_DEFAULT | FLAG_QUOTING_BYTE(1),
        p,  // first argument (C variadic protocol: at least 1 normal arg)
        &va,  // va_end() handled by Eval_Va_Core on success/fail/throw
        EVAL_MASK_DEFAULT
            | (fully ? EVAL_FLAG_NO_RESIDUE : 0)
    );

    if (IS_END(out))
        fail ("Run_Throws() empty or just COMMENTs/ELIDEs/BAR!s");

    return threw;
}


// Conditional constructs allow branches that are either BLOCK!s or ACTION!s.
// If an action, the triggering condition is passed to it as an argument:
// https://trello.com/c/ay9rnjIe
//
// Allowing other values was deemed to do more harm than good:
// https://forum.rebol.info/t/backpedaling-on-non-block-branches/476
//
inline static bool Do_Branch_Core_Throws(
    REBVAL *out,
    const REBVAL *branch,
    const REBVAL *condition  // can be END, but use nullptr vs. a NULLED cell!
){
    assert(branch != out and condition != out);

    // !!! In the interests of avoiding code duplication, this goes ahead and
    // invokes the trampoline directly with the same continuation code used
    // by stackless IF, CASE, DEFAULT, etc. on branches.
    //
    REBFRM *f = Push_Continuation_With_Core(
        out,
        FS_TOP,  // should not be touched
        0,  // no flags (hence passed in frame ignored)
        branch,
        SPECIFIED,
        condition
    );
    SET_EVAL_FLAG(f, ROOT_FRAME);
    bool threw = (*PG_Trampoline_Throws)(f);
    Drop_Frame(f);
    return threw;
}

#define Do_Branch_With_Throws(out,branch,condition) \
    Do_Branch_Core_Throws((out), (branch), NULLIFY_NULLED(condition))

#define Do_Branch_Throws(out,branch) \
    Do_Branch_Core_Throws((out), (branch), END_NODE)
