//
//  File: %d-eval.c
//  Summary: "Debug-Build Checks for the Evaluator"
//  Section: debug
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Revolt Open Source Contributors
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
// Due to the length of %c-eval.c and debug checks it already has, some
// debug-only routines are separated out here.  (Note that these are in
// addition to the checks already done by Push_Frame() and Drop_Frame() time)
//
// * Eval_Core_Expression_Checks_Debug() runs before each full "expression"
//   is evaluated, e.g. before each EVALUATE step.  It makes sure the state
//   balanced completely--so no DS_PUSH() that wasn't balanced by a DS_DROP()
//   (for example).  It also trashes variables in the frame which might
//   accidentally carry over from one step to another, so that there will be
//   a crash instead of a casual reuse.
//
// * Eval_Core_Exit_Checks_Debug() runs only if the Eval_Core() call makes
//   it to the end without a fail() longjmping out from under it.  It also
//   checks to make sure the state has balanced, and that the return result is
//   consistent with the state being returned.
//
// Because none of these routines are in the release build, they cannot have
// any side-effects that affect the interpreter's ordinary operation.
//

#include "sys-core.h"

#if defined(DEBUG_COUNT_TICKS) && defined(DEBUG_HAS_PROBE)

//
//  Dump_Frame_Location: C
//
void Dump_Frame_Location(const RELVAL *v, REBFRM *f)
{
    SHORTHAND (next, f->feed->value, NEVERNULL(const RELVAL*));
    SHORTHAND (specifier, f->feed->specifier, REBSPC*);

    DECLARE_LOCAL (dump);

    if (v) {
        Derelativize(dump, v, *specifier);
        printf("Dump_Frame_Location() current\n");
        PROBE(dump);
    }

    if (IS_END(*next)) {
        printf("...then Dump_Frame_Location() is at end of array\n");
        if (not v and not *next) { // well, that wasn't informative
            if (not f->prior)
                printf("...and no parent frame, so you're out of luck\n");
            else {
                printf("...dumping parent in case that's more useful?\n");
                Dump_Frame_Location(nullptr, f->prior);
            }
        }
    }
    else {
        Derelativize(dump, *next, *specifier);
        printf("Dump_Frame_Location() next\n");
        PROBE(dump);

        printf("Dump_Frame_Location() rest\n");

        if (FRM_IS_VALIST(f)) {
            //
            // NOTE: This reifies the va_list in the frame, which should not
            // affect procssing.  But it is a side-effect and may need to be
            // avoided if the problem you are debugging was specifically
            // related to va_list frame processing.
            //
            const bool truncated = true;
            Reify_Va_To_Array_In_Frame(f, truncated);
        }

        Init_Any_Series_At_Core(
            dump,
            REB_BLOCK,
            SER(f->feed->array),
            cast(REBLEN, f->feed->index),
            *specifier
        );
        PROBE(dump);
    }
}

#endif


#if !defined(NDEBUG)

// These are checks common to Expression and Exit checks (hence also common
// to the "end of Start" checks, since that runs on the first expression)
//
static void Eval_Core_Shared_Checks_Debug(REBFRM *f) {
    //
    // The state isn't actually guaranteed to balance overall until a frame
    // is completely dropped.  This is because a frame may be reused over
    // multiple calls by something like REDUCE or FORM, accumulating items
    // on the data stack or mold stack/etc.  See Drop_Frame() for the actual
    // balance check.

    SHORTHAND (next, f->feed->value, NEVERNULL(const RELVAL*));
    SHORTHAND (next_gotten, f->feed->gotten, const REBVAL*);
    SHORTHAND (specifier, f->feed->specifier, REBSPC*);
    SHORTHAND (index, f->feed->index, REBLEN);

    // See notes on f->feed->gotten about the coherence issues in the face
    // of arbitrary function execution.
    //
    if (*next_gotten) {
        assert(IS_WORD(*next));
        assert(Try_Lookup_Word(*next, *specifier) == *next_gotten);
    }

    assert(f == FS_TOP);
    assert(DSP == f->dsp_orig);

    if (f->feed->array) {
        assert(not IS_POINTER_TRASH_DEBUG(f->feed->array));
        assert(*index != TRASHED_INDEX);
    }
    else
        assert(*index == TRASHED_INDEX);

    // If this fires, it means that Flip_Series_To_White was not called an
    // equal number of times after Flip_Series_To_Black, which means that
    // the custom marker on series accumulated.
    //
    assert(TG_Num_Black_Series == 0);

    // We only have a label if we are in the middle of running a function,
    // and if we're not running a function then f->original should be null.
    //
    assert(not f->original);
    assert(IS_POINTER_TRASH_DEBUG(f->opt_label));

    if (f->varlist) {
        assert(NOT_SERIES_FLAG(f->varlist, MANAGED));
        assert(NOT_SERIES_INFO(f->varlist, INACCESSIBLE));
    }

    //=//// ^-- ABOVE CHECKS *ALWAYS* APPLY ///////////////////////////////=//

    if (IS_END(*next))
        return;

    if (NOT_END(f->out) and Is_Throwing(f))
        return;

    //=//// v-- BELOW CHECKS ONLY APPLY IN EXITS CASE WITH MORE CODE //////=//

    ASSERT_NOT_END(*next);
    assert(*next != f->out);

    //=//// ^-- ADD CHECKS EARLIER THAN HERE IF THEY SHOULD ALWAYS RUN ////=//
}


//
//  Eval_Core_Expression_Checks_Debug: C
//
// These fields are required upon initialization:
//
//     f->out
//     REBVAL pointer to which the evaluation's result should be written.
//     Should be to writable memory in a cell that lives above this call to
//     Eval_Core in stable memory that is not user-visible (e.g. DECLARE_LOCAL
//     or the parent's f->spare).  This can't point into an array whose memory
//     may move during arbitrary evaluation, and that includes cells on the
//     expandable data stack.  It also usually can't write a function argument
//     cell, because that could expose an unfinished calculation during this
//     Eval_Core() through its FRAME!...though a Eval_Core(f) must write f's
//     *own* arg slots to fulfill them.
//
//     f->feed
//     Contains the REBARR* or C va_list of subsequent values to fetch...as
//     well as the specifier.  The current value, its cached "gotten" value if
//     it is a WORD!, and other information is stored here through a level of
//     indirection so it may be shared and updated between recursions.
//
//     f->dsp_orig
//     Must be set to the base stack location of the operation (this may be
//     a deeper stack level than current DSP if this is an apply, and
//     refinements were preloaded onto the stack)
//
// This routine attempts to "trash" a lot of frame state variables to help
// make sure one evaluation does not leak data into the next.
//
void Eval_Core_Expression_Checks_Debug(REBFRM *f)
{
    assert(f == FS_TOP); // should be topmost frame, still

    Eval_Core_Shared_Checks_Debug(f);

    assert(not Is_Throwing(f));  // no evals between throws

    // Trash fields that GC won't be seeing unless Is_Action_Frame()
    //
    TRASH_POINTER_IF_DEBUG(f->param);
    TRASH_POINTER_IF_DEBUG(f->arg);
    TRASH_POINTER_IF_DEBUG(f->special);

    assert(not f->varlist or NOT_SERIES_INFO(f->varlist, INACCESSIBLE));

    // Mutate va_list sources into arrays at fairly random moments in the
    // debug build.  It should be able to handle it at any time.
    //
    if (FRM_IS_VALIST(f) and SPORADICALLY(50)) {
        const bool truncated = true;
        Reify_Va_To_Array_In_Frame(f, truncated);
    }
}


//
//  Do_Process_Action_Checks_Debug: C
//
void Do_Process_Action_Checks_Debug(REBFRM *f) {

    assert(IS_FRAME(f->rootvar));
    assert(f->arg == FRM_ARGS_HEAD(f));

    REBACT *phase = VAL_PHASE(f->rootvar);

    //=//// v-- BELOW CHECKS ONLY APPLY WHEN FRM_PHASE() is VALID ////////=//

    assert(GET_ARRAY_FLAG(ACT_PARAMLIST(phase), IS_PARAMLIST));

    // !!! This check related to a marking of outputs stale that was in the
    // PROCESS_ACTION branch of the pre-stackless work.  However that marking
    // causes problems with boot now.  Temporarily eliminating the assert
    // until it's understood what consequence it has.
    //
    /* if (NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT)) {
        if (NOT_CELL_FLAG(f->out, OUT_MARKED_STALE))
            assert(GET_ACTION_FLAG(phase, IS_INVISIBLE));
    } */
}


//
//  Do_After_Action_Checks_Debug: C
//
void Do_After_Action_Checks_Debug(REBFRM *f) {
    assert(NOT_END(f->out));
    assert(not Is_Throwing(f));

    if (GET_SERIES_INFO(f->varlist, INACCESSIBLE)) // e.g. ENCLOSE
        return;

    REBACT *phase = FRM_PHASE(f);

  #ifdef DEBUG_UTF8_EVERYWHERE
    if (ANY_STRING(f->out)) {
        REBLEN len = STR_LEN(VAL_STRING(f->out));
        UNUSED(len); // just one invariant for now, SER_LEN checks it
    }
  #endif

    // Usermode functions check the return type via Returner_Dispatcher(),
    // with everything else assumed to return the correct type.  But this
    // double checks any function marked with RETURN in the debug build,
    // so native return types are checked instead of just trusting the C.
    //
    // !!! PG_Dispatcher() should do this, so every phase gets checked.
    //
  #ifdef DEBUG_NATIVE_RETURNS
    if (GET_ACTION_FLAG(phase, HAS_RETURN)) {
        REBVAL *typeset = ACT_PARAMS_HEAD(phase);
        assert(VAL_PARAM_SYM(typeset) == SYM_RETURN);
        if (
            not Typecheck_Including_Quoteds(typeset, f->out)
            and not (
                GET_ACTION_FLAG(phase, IS_INVISIBLE)
                and IS_NULLED(f->out) // this happens with `do [return]`
            )
        ){
            printf("Native code violated return type contract!\n");
            panic (Error_Bad_Return_Type(f, VAL_TYPE(f->out)));
        }
    }
  #endif
}


//
//  Eval_Core_Exit_Checks_Debug: C
//
void Eval_Core_Exit_Checks_Debug(REBFRM *f) {
    Eval_Core_Shared_Checks_Debug(f);

    SHORTHAND (next, f->feed->value, NEVERNULL(const RELVAL*));

    if (NOT_END(*next) and not FRM_IS_VALIST(f)) {
        if (f->feed->index > ARR_LEN(f->feed->array)) {
            assert(
                (f->feed->pending and IS_END(f->feed->pending))
                or Is_Throwing(f)
            );
            assert(f->feed->index == ARR_LEN(f->feed->array) + 1);
        }
    }

    // We'd like `do [1 + comment "foo"]` to act identically to `do [1 +]`
    // Eval_Core() thus distinguishes an END for a fully "invisible"
    // evaluation, as opposed to void.  This distinction is internal and not
    // exposed to the user, at the moment.
    //
    if (NOT_END(f->out))
        assert(Is_Throwing(f) or VAL_TYPE(f->out) < REB_MAX);
}

#endif
