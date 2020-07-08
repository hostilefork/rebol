//
//  File: %sys-eval.h
//  Summary: {Low-Level Internal Evaluator API}
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2018 Revolt Open Source Contributors
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
// The routine that powers a single EVAL or EVALUATE step is Eval_Core().
// It takes one parameter which holds the running state of the evaluator.
// This state may be allocated on the C variable stack...and fail() is
// written such that a longjmp up to a failure handler above it can run
// safely and clean up even though intermediate stacks have vanished.
//
// Revolt can run the evaluator across a REBARR-style series of input based on
// index.  It can also enumerate through C's `va_list`, providing the ability
// to pass pointers as REBVAL* to comma-separated input at the source level.
//
// To provide even greater flexibility, it allows the very first element's
// pointer in an evaluation to come from an arbitrary source.  It doesn't
// have to be resident in the same sequence from which ensuing values are
// pulled, allowing a free head value (such as an ACTION! REBVAL in a local
// C variable) to be evaluated in combination from another source (like a
// va_list or series representing the arguments.)  This avoids the cost and
// complexity of allocating a series to combine the values together.
//
//=//// NOTES ////////////////////////////////////////////////////////////=//
//
// * The usermode EVALUATE action is able to avoid overwriting the previous
//   value if the final evaluation step has nothing in it.  That's based on
//   the ability exposed here through the "Maybe_Stale" variations of the
//   Eval_XXX() routines.  Care should be taken not to allow OUT_MARKED_STALE
//   to leak and clear it on the cell (it is NODE_FLAG_MARKED and could be
//   misinterpreted--very easily so as ARG_MARKED_CHECKED!)
//
// * The usermode REEVAL function chooses to make `reeval comment "hi"` VOID!
//   rather than to raise an error.  However, the non-"Maybe_Stale" versions
//   of code here have another option...which is to give the result as END.
//   Currently this is what all the Eval_Step() routines which aren't stale
//   preserving do--but Eval_Value_Throws() will error.
//


#if defined(DEBUG_COUNT_TICKS)  // <-- THIS IS VERY USEFUL, READ THIS SECTION!
    //
    // The `REBFRM.tick` is updated in each frame on the last time it was
    // evaluated.  So if a fail() happens at a deterministic moment in a run,
    // capture the number from the level of interest and recompile for a
    // breakpoint at that tick.
    //
    // If the tick is AFTER command line processing is done, you can request
    // a tick breakpoint that way with `--breakpoint NNN`
    //
    // The debug build carries ticks many other places.  Series contain the
    // `REBSER.tick`, and the DEBUG_TRACK_EXTEND_CELLS switch will double the
    // size of cells so they can carry the tick, file, and line where they
    // were initialized.  (Even without TRACK_EXTEND, cells that don't have
    // their EXTRA() field in use carry the tick--it's in end cells, nulls,
    // blanks, and trash.)
    //
    // For custom updating of stored ticks to help debugging some scenarios,
    // see TOUCH_SERIES() and TOUCH_CELL().  Note also that BREAK_NOW() can be
    // called to pause and dump state at any moment.

    #define UPDATE_TICK_DEBUG(f,v) \
        do { \
            if (TG_Tick < INTPTR_MAX)  /* avoid rollover (may be 32-bit!) */ \
                (f)->tick = ++TG_Tick; \
            else \
                (f)->tick = INTPTR_MAX;  /* see tick for why signed! */ \
            if ( \
                (TG_Break_At_Tick != 0 and f->tick >= TG_Break_At_Tick) \
            ){ \
                printf("BREAK AT TICK %u\n", cast(unsigned int, (f)->tick)); \
                Dump_Frame_Location((v), (f)); \
                debug_break();  /* see %debug_break.h */ \
                TG_Break_At_Tick = 0; \
            } \
        } while (false)  // macro so that breakpoint is at right stack level!
#else
    #define UPDATE_TICK_DEBUG(f,v) NOOP
#endif


// Helper for calling Trampoline_Throws(), which acts through a function
// pointer...so that if there is a hooked trampoline it won't be skipped.
//
inline static bool Eval_Throws(REBFRM *f) {
    assert(f == FS_TOP);
    return (*PG_Trampoline_Throws)(f);
}


// If you're sure the evaluator isn't hooked, it seems no point in asking to
// "evaluate" a 1 (if there's nothing enfix after it).  You can take an inert
// optimization.  But if the evaluator is hooked with a trace or stepwise
// debugging, you can't skip out on the real call--it would skip the hook.
//
// Note that some optimizations can be dangerous beyond skipping tracing or
// debugging.  e.g. if `cycle []` runs, even if it's not hooked to see the
// trace of "I'm running an empty block", the tight loop without calling the
// evaluator could miss the check for if a halt via Ctrl-C/etc. was requested:
//
// https://github.com/rebol/rebol-issues/issues/2229
//
// It would be possible to ask routines like CYCLE to do explicit checks for
// the halt signal.  But the choice made is simply not to optimize a DO of an
// empty block--considering the case to be too rare to be worth optimizing,
// when weighed against predictable evaluator behavior.  So code using this
// should be very rare instances.
//
// !!! In the spirit of simplification, it may be that this test should not
// exist at all.  It is included for the sake of experimentation, but may
// be removed if there appears to be no point in maintaining the complexity.
//
#define OPTIMIZATIONS_OK \
    (PG_Trampoline_Throws == &Trampoline_Throws)


// Even though ANY_INERT() is a quick test, you can't skip the cost of frame
// processing--due to enfix.  But a feed only looks ahead one unit at a time,
// so advancing the frame past an inert item to find an enfix function means
// you have to enter the frame specially with executor Lookahead_Executor().
//
inline static bool Did_Init_Inert_Optimize_Complete(
    REBVAL *out,
    REBFED *feed,
    REBFLGS *flags,
    REBNAT *executor_out
){
    assert(not IS_END(feed->value));  // would be wasting time to call

    if (not ANY_INERT(feed->value) or not OPTIMIZATIONS_OK) {
        SET_END(out);  // Have to Init() `out` one way or another...
        *executor_out = &New_Expression_Executor;
        return false;  // general case evaluation requires a frame
    }

    Literal_Next_In_Feed(out, feed);

    if (KIND_BYTE_UNCHECKED(feed->value) == REB_WORD) {
        feed->gotten = Try_Lookup_Word(feed->value, feed->specifier);
        if (
            not feed->gotten
            or not IS_ACTION(feed->gotten)
            or NOT_ACTION_FLAG(VAL_ACTION(feed->gotten), ENFIXED)
        ){
            CLEAR_FEED_FLAG(feed, NO_LOOKAHEAD);
            return true;  // not enfixed
        }

        REBACT *action = VAL_ACTION(feed->gotten);
        if (GET_ACTION_FLAG(action, QUOTES_FIRST)) {
            //
            // Quoting defeats NO_LOOKAHEAD but only on soft quotes.
            //
            if (NOT_FEED_FLAG(feed, NO_LOOKAHEAD)) {
                *flags |= EVAL_FLAG_INERT_OPTIMIZATION;
                *executor_out = &Lookahead_Executor;
                return false;
            }

            CLEAR_FEED_FLAG(feed, NO_LOOKAHEAD);

            REBVAL *first = First_Unspecialized_Param(action);  // cache test?
            if (VAL_PARAM_CLASS(first) == REB_P_SOFT_QUOTE)
                return true;  // don't look back, yield the lookahead

            *flags |= EVAL_FLAG_INERT_OPTIMIZATION;
            *executor_out = &Lookahead_Executor;
            return false;
        }

        if (GET_FEED_FLAG(feed, NO_LOOKAHEAD)) {
            CLEAR_FEED_FLAG(feed, NO_LOOKAHEAD);
            return true;   // we're done!
        }

        // EVAL_FLAG_POST_SWITCH assumes that if the first arg were quoted and
        // skippable, that the skip check has already been done.  So we have
        // to do that check here.
        //
        if (GET_ACTION_FLAG(action, SKIPPABLE_FIRST)) {
            REBVAL *first = First_Unspecialized_Param(action);
            if (not TYPE_CHECK(first, KIND_BYTE(out)))
                return true;  // didn't actually want this parameter type
        }

        *flags |= EVAL_FLAG_INERT_OPTIMIZATION;
        *executor_out = &Lookahead_Executor;
        return false;  // do normal enfix handling
    }

    if (GET_FEED_FLAG(feed, NO_LOOKAHEAD)) {
        CLEAR_FEED_FLAG(feed, NO_LOOKAHEAD);
        return true;   // we're done!
    }

    if (KIND_BYTE_UNCHECKED(feed->value) != REB_PATH)
        return true;  // paths do enfix processing if '/'

    if (
        KIND_BYTE(ARR_AT(VAL_ARRAY(feed->value), 0)) == REB_BLANK
        and KIND_BYTE(ARR_AT(VAL_ARRAY(feed->value), 1)) == REB_BLANK
    ){
        *flags |= EVAL_FLAG_INERT_OPTIMIZATION;
        *executor_out = &Lookahead_Executor;
        return false;  // Let evaluator handle `/`
    }

    return true;
}


// This is a very light wrapper over Eval_Core(), which is used with
// operations like ANY or REDUCE that wish to perform several successive
// operations on an array, without creating a new frame each time.
//
inline static bool Eval_Step_Maybe_Stale_Throws(
    REBVAL *out,
    REBFRM *f
){
    assert(f == FS_TOP);
    assert(NOT_FEED_FLAG(f->feed, NO_LOOKAHEAD));

    f->out = out;
    f->baseline.dsp = DSP;
    f->executor = &New_Expression_Executor;
    return (*PG_Trampoline_Throws)(f); // should already be pushed;
}

inline static bool Eval_Step_Throws(REBVAL *out, REBFRM *f) {
    SET_END(out);
    bool threw = Eval_Step_Maybe_Stale_Throws(out, f);
    CLEAR_CELL_FLAG(out, OUT_MARKED_STALE);
    return threw;
}


// It should not be necessary to use a subframe unless there is meaningful
// state which would be overwritten in the parent frame.  For the moment,
// that only happens if a function call is in effect -or- if a SET-WORD! or
// SET-PATH! are running with an expiring `current` in effect.  Else it is
// more efficient to call Eval_Step_In_Frame_Throws(), or the also lighter
//
inline static bool Eval_Step_In_Subframe_Throws(
    REBVAL *out,
    REBFRM *f,
    REBFLGS flags
){
    REBNAT executor = &New_Expression_Executor;
/*    if (Did_Init_Inert_Optimize_Complete(out, f->feed, &flags, &executor))
        return false;  // If eval not hooked, ANY-INERT! may not need a frame */

    // Can't SET_END() here, because sometimes it would be overwriting what
    // the optimization produced.  Trust that it has already done it if it
    // was necessary.

    DECLARE_FRAME (subframe, f->feed, flags);
    subframe->executor = executor;

    Push_Frame(out, subframe);
    bool threw = Eval_Throws(subframe);
    Drop_Frame(subframe);

    return threw;
}


inline static bool Eval_Step_In_Any_Array_At_Throws(
    REBVAL *out,
    REBLEN *index_out,
    const RELVAL *any_array,  // Note: legal to have any_array = out
    REBSPC *specifier,
    REBFLGS flags
){
    SET_END(out);

    if (IS_END(VAL_ARRAY_AT(any_array))) {
        //
        // !!! This optimization likely is worth it, as it's avoiding a feed
        // and frame allocation...and happens once while evaluating basically
        // all arrays.  Review.
        //
        *index_out = 0xDECAFBAD;  // avoid compiler warning
        return false;
    }

    DECLARE_FRAME_AT_CORE (
        f,
        any_array,
        specifier,
        flags | EVAL_FLAG_ALLOCATED_FEED
    );
    INIT_F_EXECUTOR(f, &New_Expression_Executor);

    Push_Frame(out, f);
    bool threw = Eval_Throws(f);

    if (threw)
        *index_out = TRASHED_INDEX;
    else
        *index_out = f->feed->index - 1;

    Drop_Frame(f);

    return threw;
}


// (va_list by pointer: http://stackoverflow.com/a/3369762/211160)
//
// Central routine for doing an evaluation of an array of values by calling
// a C function with those parameters (e.g. supplied as arguments, separated
// by commas).  Uses same method to do so as functions like printf() do.
//
// The evaluator has a common means of fetching values out of both arrays
// and C va_lists via Fetch_Next_In_Frame(), so this code can behave the
// same as if the passed in values came from an array.  However, when values
// originate from C they often have been effectively evaluated already, so
// it's desired that WORD!s or PATH!s not execute as they typically would
// in a block.  So this is often used with EVAL_FLAG_EXPLICIT_EVALUATE.
//
// !!! C's va_lists are very dangerous, there is no type checking!  The
// C++ build should be able to check this for the callers of this function
// *and* check that you ended properly.  It means this function will need
// two different signatures (and so will each caller of this routine).
//
inline static bool Eval_Step_In_Va_Throws_Core(
    REBVAL *out,  // must be initialized, won't change if all empty/invisible
    REBFLGS feed_flags,
    const void *p,
    va_list *vaptr,
    REBFLGS eval_flags
){
    DECLARE_VA_FEED (feed, p, vaptr, feed_flags);

    DECLARE_FRAME (f, feed, eval_flags);
    INIT_F_EXECUTOR(f, &New_Expression_Executor);

    Push_Frame(out, f);
    bool threw = Eval_Throws(f);
    Drop_Frame(f); // will va_end() if not reified during evaluation

    if (threw)
        return true;

    if ((eval_flags & EVAL_FLAG_NO_RESIDUE) and NOT_END(feed->value))
        fail (Error_Apply_Too_Many_Raw());

    // A va_list-based feed has a lookahead, and also may be spooled due to
    // the GC being triggered.  So the va_list had ownership taken, and it's
    // not possible to return a REBIXO here to "resume the va_list later".
    // That can only be done if the feed is held alive across evaluations.
    //
    return false;
}


inline static bool Eval_Value_Throws(
    REBVAL *out,
    const RELVAL *value,  // e.g. a BLOCK! here would just evaluate to itself!
    REBSPC *specifier
){
    if (ANY_INERT(value) and OPTIMIZATIONS_OK) {
        Derelativize(out, value, specifier);
        return false;  // fast things that don't need frames (should inline)
    }

    // We need the const bits on this value to apply, so have to use a low
    // level call.

    SET_END(out);

    REBFED feed_struct;  // opt_first so can't use DECLARE_ARRAY_FEED
    REBFED *feed = &feed_struct;
    Prep_Array_Feed(
        feed,
        value,  // opt_first--in this case, the only value in the feed...
        EMPTY_ARRAY,  // ...because we're using the empty array after that
        0,  // ...at index 0
        specifier,
        FEED_MASK_DEFAULT | (value->header.bits & FEED_FLAG_CONST)
    );

    DECLARE_FRAME (f, feed, EVAL_MASK_DEFAULT);
    INIT_F_EXECUTOR(f, &New_Expression_Executor);

    Push_Frame(out, f);
    bool threw = Eval_Throws(f);
    Drop_Frame(f);

    // The callsites for Eval_Value_Throws() generally expect an evaluative
    // result (at least null).  They might be able to give a better error, but
    // they pretty much all need to give an error.
    //
    // In contrast, note that REEVAL itself errs on the side of voids, so:
    //
    //     >> type of reeval comment "hi"
    //     == #[void!]
    //
    if (IS_END(out))
        fail ("Single step EVAL produced no result (invisible or empty)");

    return threw;
}
