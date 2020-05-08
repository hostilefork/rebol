//
//  File: %c-eval.c
//  Summary: "Central Interpreter Evaluator"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2020 Revolt Open Source Contributors
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
// This file contains Eval_Internal_Maybe_Stale_Throws(), which is the central
// evaluator implementation.  Most callers should use higher level wrappers,
// because the long name conveys any direct caller must handle the following:
//
// * _Maybe_Stale_ => The evaluation targets an output cell which must be
//   preloaded or set to END.  If there is no result (e.g. due to being just
//   comments) then whatever was in that cell will still be there -but- will
//   carry OUT_MARKED_STALE.  This is just an alias for NODE_FLAG_MARKED, and
//   it must be cleared off before passing pointers to the cell to a routine
//   which may interpret that flag differently.
//
// * _Internal_ => This is the fundamental C code for the evaluator, but it
//   can be "hooked".  Those hooks provide services like debug stepping and
//   tracing.  So most calls to this routine should be through a function
//   pointer and not directly.
//
// * _Throws => The return result is a boolean which all callers *must* heed.
//   There is no "thrown value" data type or cell flag, so the only indication
//   that a throw happened comes from this flag.  See %sys-throw.h
//
// Eval_Throws() is a small stub which takes care of the first two concerns,
// though some low-level clients actually want the stale flag.
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * See %sys-eval.h for wrappers that make it easier to set up frames and
//   use the evaluator for a single step.
//
// * See %sys-do.h for wrappers that make it easier to run multiple evaluator
//   steps in a frame and return the final result, giving VOID! by default.
//
// * Eval_Internal_Maybe_Stale_Throws() is LONG.  That's largely on purpose.
//   Breaking it into functions would add overhead (in the debug build if not
//   also release builds) and prevent interesting tricks and optimizations.
//   It is separated into sections, and the invariants in each section are
//   made clear with comments and asserts.
//
// * See %d-eval.c for more detailed assertions of the preconditions,
//   postconditions, and state...which are broken out to help keep this file
//   a more manageable length.
//
// * The evaluator only moves forward, and operates on a strict window of
//   visibility of two elements at a time (current position and "lookback").
//   See `Reb_Feed` for the code that provides this abstraction over Revolt
//   arrays as well as C va_list.
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



#ifdef DEBUG_EXPIRED_LOOKBACK
    #define CURRENT_CHANGES_IF_FETCH_NEXT \
        (f->feed->stress != nullptr)
#else
    #define CURRENT_CHANGES_IF_FETCH_NEXT \
        (v == &f->feed->lookback)
#endif


// To allow frames to share feeds, the feed is held by pointer.  But that
// makes accessing things verbose.  Also, the meaning is usually "next" in
// the evaluator, since it only represents the current value very briefly as
// it is pulled into a local for processing.  These macros shorten + clarify.
//
#define f_spare         FRM_SPARE(f)
#define f_next          f->feed->value  // !!! never nullptr, check in debug?
#define f_next_gotten   f->feed->gotten
#define f_specifier     f->feed->specifier


// SET-WORD!, SET-PATH!, SET-GROUP!, and SET-BLOCK! all want to do roughly
// the same thing as the first step of their evaluation.  They evaluate the
// right hand side into f->out.
//
// -but- because you can be asked to evaluate something like `x: y: z: ...`,
// there could be any number of SET-XXX! before the value to assign is found.
//
// This inline function attempts to keep that stack by means of the local
// variable `v`, if it points to a stable location.  If so, it simply reuses
// the frame it already has.
//
// What makes this slightly complicated is that the current value may be in
// a place that doing a Fetch_Next_In_Frame() might corrupt it.  This could
// be accounted for by pushing the value to some other stack--e.g. the data
// stack.  But for the moment this (uncommon?) case uses a new frame.
//
inline static bool Rightward_Evaluate_Nonvoid_Into_Out_Throws(
    REBFRM *f,
    const RELVAL *v
){
    if (GET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT))  {  // e.g. `10 -> x:`
        CLEAR_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT);
        CLEAR_CELL_FLAG(f->out, UNEVALUATED);  // this helper counts as eval
        return false;
    }

    if (IS_END(f_next))  // `do [x:]`, `do [o/x:]`, etc. are illegal
        fail (Error_Need_Non_End_Core(v, f_specifier));

    // !!! While assigning `x: #[void]` is not legal, we make a special
    // exemption for quoted voids, e.g. '#[void]`.  This means a molded
    // object with void fields can be safely MAKE'd back.
    //
    if (KIND_BYTE(f_next) == REB_VOID + REB_64) {
        Init_Void(f->out);
        Fetch_Next_Forget_Lookback(f);  // advances f->value
        return false;
    }

    // Using a SET-XXX! means you always have at least two elements; it's like
    // an arity-1 function.  `1 + x: whatever ...`.  This overrides the no
    // lookahead behavior flag right up front.
    //
    CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);

    REBFLGS flags = EVAL_MASK_DEFAULT
            | (f->flags.bits & EVAL_FLAG_FULFILLING_ARG);  // if f was, we are

    SET_END(f->out);  // `1 x: comment "hi"` shouldn't set x to 1!

    // !!! This used to have an optimization to reuse the current frame if
    // not CURRENT_CHANGES_IF_FETCH_NEXT.  Reusing frames got more complex
    // because the evaluator might be in a continuation.  The optimization is
    // questionable in a stackless world, and should be reviewed.
    //
    bool x = true;
    if (
        x or
        CURRENT_CHANGES_IF_FETCH_NEXT or GET_EVAL_FLAG(f, CONTINUATION)
    ){
        if (Eval_Step_In_Subframe_Throws(f->out, f, flags))
            return true;
    }
    else {  // !!! Reusing the frame, would inert optimization be worth it?
        if ((*PG_Eval_Maybe_Stale_Throws)())  // reuse `f`
            return true;
    }

    if (IS_END(f->out))  // e.g. `do [x: ()]` or `(x: comment "hi")`.
        fail (Error_Need_Non_End_Core(v, f_specifier));

    CLEAR_CELL_FLAG(f->out, UNEVALUATED);  // this helper counts as eval
    return false;
}

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


//
//  Eval_Just_Use_Out: C
//
// This is a simple no-op continuation which can be used when you've already
// calculated a result, but you're in a position where you need to give a
// continuation because the caller was expecting a call back...and if you
// return a pointer to `f->out` directly the system assumes that's the final
// result.
//
// !!! This is somewhat inefficient and probably calls for some special
// loophole, but it's not easy to think of what a good place for that loophole
// would be right now... so this goes ahead and fits into the homogenous
// continuation model by giving a pass through option.
//
REB_R Eval_Just_Use_Out(REBFRM *f)
{
    return f->out;
}


//
//  Group_Executor: C
//
// A GROUP! whose contents wind up vaporizing wants to be invisible:
//
//     >> 1 + 2 ()
//     == 3
//
//     >> 1 + 2 (comment "hi")
//     == 3
//
// But there's a limit with group invisibility and enfix.  A single step
// of the evaluator only has one lookahead, because it doesn't know if it
// wants to evaluate the next thing or not:
//
//     >> evaluate [1 (2) + 3]
//     == [(2) + 3]  ; takes one step...so next step will add 2 and 3
//
//     >> evaluate [1 (comment "hi") + 3]
//     == [(comment "hi") + 3]  ; next step errors: `+` has no left argument
//
// It is supposed to be possible for DO to be implemented as a series of
// successive single EVALUATE steps, giving no input beyond the block.  So
// that means even though the `f->out` may technically still hold bits of
// the last evaluation such that `do [1 (comment "hi") + 3]` could draw
// from them to give a left hand argument, it should not do so...and it's
// why those bits are marked "stale".
//
// The other side of the operator is a different story.  Turning up no result,
// the group can just invoke a reevaluate without breaking any rules:
//
//     >> evaluate [1 + (2) 3]
//     == [3]
//
//     >> evaluate [1 + (comment "hi") 3]
//     == []
//
// This subtlety means the continuation for running a GROUP! has the subtlety
// of noticing when no result was produced (an output of END) and then
// re-triggering a step in the parent frame, e.g. to pick up the 3 above.
//
REB_R Group_Executor(REBFRM *f)
{
    if (IS_END(f->out)) {
        if (IS_END(F_VALUE(f)))
            return f->out;  // nothing after to try, indicate value absence

        f->executor = &Eval_Frame_Workhorse;
        f->u.reval.value = Lookback_While_Fetching_Next(f);
        return R_CONTINUATION;
    }

    if (not VAL_LOGIC(FRM_SPARE(f)))
        CLEAR_EVAL_FLAG(f, CONTINUATION);  // why unset?

    CLEAR_CELL_FLAG(f->out, UNEVALUATED);  // `(1)` is evaluative
    f->executor = &Eval_Post_Switch;
    return R_CONTINUATION;
}


//
//  Eval_Brancher: C
//
// !!! Does a double-execution on its branch.  Uses a new idea of a preloaded
// frame `->spare` cell to hold an argument without needing a varlist.  Used
// to implement code-generating branches which don't run unless they match,
// as opposed to using plain GROUP! which would generate unused code:
//
//    >> either 1 @(print "one" [2 + 3]) @(print "run" [4 + 5])
//    one
//    == 5
//
REB_R Eval_Brancher(REBFRM *frame_)
{
    if (IS_SYM_GROUP(D_SPARE)) {
        mutable_KIND_BYTE(D_SPARE) = mutable_MIRROR_BYTE(D_SPARE) = REB_BLOCK;
        CONTINUE (D_SPARE);
    }

    assert(IS_BLOCK(D_SPARE));

    if (not (  // ... any of the legal branch types
        IS_BLOCK(D_OUT)
        or IS_QUOTED(D_OUT)
        or IS_SYM_WORD(D_OUT)
        or IS_SYM_PATH(D_OUT)
        or IS_SYM_GROUP(D_OUT)
        or IS_BLANK(D_OUT)
    )){
        fail ("Invalid branch type produced by SYM-GROUP! redone branch");
    }

    Move_Value(D_SPARE, D_OUT);
    DELEGATE (D_SPARE);
}


//
//  Eval_New_Expression: C
//
// This is the continuation dispatcher for what would be considered a new
// single step in the evaluator.  That can be from the point of view of the
// debugger, or just in terms of marking the point at which an error message
// would begin.
//
REB_R Eval_New_Expression(REBFRM *f)
{
  #ifdef DEBUG_ENSURE_FRAME_EVALUATES
    f->was_eval_called = true;  // see definition for why this flag exists
  #endif

    assert(DSP >= f->dsp_orig);  // REDUCE accrues, APPLY adds refinements
    assert(not IS_TRASH_DEBUG(f->out));  // all invisible will preserve output
    assert(f->out != f_spare);  // overwritten by temporary calculations
    assert(NOT_FEED_FLAG(f->feed, BARRIER_HIT));

  #if !defined(NDEBUG)
    Eval_Core_Expression_Checks_Debug(f);
    assert(NOT_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH));
    if (NOT_EVAL_FLAG(f, FULFILLING_ARG))
        assert(NOT_FEED_FLAG(f->feed, NO_LOOKAHEAD));
    assert(NOT_FEED_FLAG(f->feed, DEFERRING_ENFIX));
  #endif

//=//// START NEW EXPRESSION //////////////////////////////////////////////=//

    assert(Eval_Count >= 0);
    if (--Eval_Count == 0) {
        //
        // Note that Do_Signals_Throws() may do a recycle step of the GC, or
        // it may spawn an entire interactive debugging session via
        // breakpoint before it returns.  It may also FAIL and longjmp out.
        //
        if (Do_Signals_Throws(f->out))
            goto return_thrown;
    }

    assert(NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT));
    SET_CELL_FLAG(f->out, OUT_MARKED_STALE);  // internal use flag only

    UPDATE_EXPRESSION_START(f);  // !!! See FRM_INDEX() for caveats

    // Caching KIND_BYTE(*at) in a local can make a slight performance
    // difference, though how much depends on what the optimizer figures out.
    // Either way, it's useful to have handy in the debugger.
    //
    // Note: int8_fast_t picks `char` on MSVC, shouldn't `int` be faster?
    // https://stackoverflow.com/a/5069643/
    //
    union {
        int byte;  // values bigger than REB_64 are used for in-situ literals
        enum Reb_Kind pun;  // for debug viewing *if* byte < REB_MAX_PLUS_MAX
    } kind;

    kind.byte = KIND_BYTE(f_next);

    // If asked to evaluate `[]` then we have now done all the work the
    // evaluator needs to do--including marking the output stale.
    //
    // See DEBUG_ENSURE_FRAME_EVALUATES for why an empty array does not
    // bypass calling into the evaluator.
    //
    if (kind.byte == REB_0_END)
        goto finished;

    // shorthand for the value we are switch()-ing on

    const RELVAL *v = Lookback_While_Fetching_Next(f);
    // ^-- can't just `v = f_next`, fetch may overwrite--request lookback!

    const REBVAL *gotten = f_next_gotten;
    UNUSED(gotten);

    assert(kind.byte == KIND_BYTE_UNCHECKED(v));
    f->executor = &Eval_Frame_Workhorse;
    f->u.reval.value = v;
    return R_CONTINUATION;

  return_thrown:
    return R_THROWN;

  finished:
    return f->out;
}


//
//  Eval_Frame_Workhorse: C
//
// Try and encapsulate the main frame work but without actually looping.
// This means it needs more return results than just `bool` for threw.
// It gives it the same signature as a dispatcher.
//
REB_R Eval_Frame_Workhorse(REBFRM *f)
{
    union {
        int byte;  // values bigger than REB_64 are used for in-situ literals
        enum Reb_Kind pun;  // for debug viewing *if* byte < REB_MAX_PLUS_MAX
    } kind;

    // `v` is the shorthand for the value we are switching on
    //
    const RELVAL *v = f->u.reval.value;
    const REBVAL *gotten = nullptr;
    kind.byte = KIND_BYTE(v);

  reevaluate: ;  // meaningful semicolon--subsequent macro may declare things

    // ^-- doesn't advance expression index: `reeval x` starts with `reeval`

//=//// LOOKAHEAD FOR ENFIXED FUNCTIONS THAT QUOTE THEIR LEFT ARG /////////=//

    // Revolt has an additional lookahead step *before* an evaluation in order
    // to take care of this scenario.  To do this, it pre-emptively feeds the
    // frame one unit that f->value is the f_next* value, and a local variable
    // called "current" holds the current head of the expression that the
    // main switch would process.

    if (KIND_BYTE(f_next) != REB_WORD)  // right's kind - END would be REB_0
        goto give_up_backward_quote_priority;

    assert(not f_next_gotten);  // Fetch_Next_In_Frame() cleared it
    f_next_gotten = Try_Lookup_Word(f_next, f_specifier);

    if (not f_next_gotten or not IS_ACTION(f_next_gotten))
        goto give_up_backward_quote_priority;  // note only ACTION! is ENFIXED

    if (NOT_ACTION_FLAG(VAL_ACTION(f_next_gotten), ENFIXED))
        goto give_up_backward_quote_priority;

    if (NOT_ACTION_FLAG(VAL_ACTION(f_next_gotten), QUOTES_FIRST))
        goto give_up_backward_quote_priority;

    // If the action soft quotes its left, that means it's aware that its
    // "quoted" argument may be evaluated sometimes.  If there's evaluative
    // material on the left, treat it like it's in a group.
    //
    if (
        GET_ACTION_FLAG(VAL_ACTION(f_next_gotten), POSTPONES_ENTIRELY)
        or (
            GET_FEED_FLAG(f->feed, NO_LOOKAHEAD)
            and not ANY_SET_KIND(kind.byte)  // not SET-WORD!, SET-PATH!, etc.
        )
    ){
        // !!! cache this test?
        //
        REBVAL *first = First_Unspecialized_Param(VAL_ACTION(f_next_gotten));
        if (
            VAL_PARAM_CLASS(first) == REB_P_SOFT_QUOTE
            or VAL_PARAM_CLASS(first) == REB_P_MODAL
        ){
            goto give_up_backward_quote_priority;  // yield as an exemption
        }
    }

    // Let the <skip> flag allow the right hand side to gracefully decline
    // interest in the left hand side due to type.  This is how DEFAULT works,
    // such that `case [condition [...] default [...]]` does not interfere
    // with the BLOCK! on the left, but `x: default [...]` gets the SET-WORD!
    //
    if (GET_ACTION_FLAG(VAL_ACTION(f_next_gotten), SKIPPABLE_FIRST)) {
        REBVAL *first = First_Unspecialized_Param(VAL_ACTION(f_next_gotten));
        if (not TYPE_CHECK(first, kind.byte))  // left's kind
            goto give_up_backward_quote_priority;
    }

    // Lookback args are fetched from f->out, then copied into an arg
    // slot.  Put the backwards quoted value into f->out.
    //
    Derelativize(f->out, v, f_specifier);  // for NEXT_ARG_FROM_OUT
    SET_CELL_FLAG(f->out, UNEVALUATED);  // so lookback knows it was quoted

    // We skip over the word that invoked the action (e.g. <-, OF, =>).
    // v will then hold a pointer to that word (possibly now resident in the
    // frame's f_spare).  (f->out holds what was the left)
    //
    gotten = f_next_gotten;
    v = Lookback_While_Fetching_Next(f);

    if (
        IS_END(f_next)
        and (kind.byte == REB_WORD or kind.byte == REB_PATH)  // left kind
    ){
        // We make a special exemption for left-stealing arguments, when
        // they have nothing to their right.  They lose their priority
        // and we run the left hand side with them as a priority instead.
        // This lets us do e.g. `(lit =>)` or `help of`
        //
        // Swap it around so that what we had put in the f->out goes back
        // to being in the lookback cell and can be used as current.  Then put
        // what was current into f->out so it can be consumed as the first
        // parameter of whatever that was.

        Move_Value(&f->feed->lookback, f->out);
        Derelativize(f->out, v, f_specifier);
        SET_CELL_FLAG(f->out, UNEVALUATED);

        // leave f_next at END
        v = &f->feed->lookback;
        gotten = nullptr;

        SET_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH);  // for better error message
        SET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT);  // literal right op is arg

        goto give_up_backward_quote_priority;  // run PATH!/WORD! normal
    }

    // Wasn't the at-end exception, so run normal enfix with right winning.

 blockscope {
    DECLARE_FRAME (subframe, f->feed, f->flags.bits & ~(EVAL_FLAG_ALLOCATED_FEED));
    Push_Frame(f->out, subframe);
    Push_Action(subframe, VAL_ACTION(gotten), VAL_BINDING(gotten));
    Begin_Enfix_Action(subframe, VAL_WORD_SPELLING(v));

    kind.byte = REB_ACTION;  // for consistency in the UNEVALUATED check
    goto process_action; }

  give_up_backward_quote_priority:

//=//// BEGIN MAIN SWITCH STATEMENT ///////////////////////////////////////=//

    // This switch is done with a case for all REB_XXX values, in order to
    // facilitate use of a "jump table optimization":
    //
    // http://stackoverflow.com/questions/17061967/c-switch-and-jump-tables
    //
    // Subverting the jump table optimization with specialized branches for
    // fast tests like ANY_INERT() and IS_NULLED_OR_VOID_OR_END() has shown
    // to reduce performance in practice.  The compiler does the right thing.

    assert(kind.byte == KIND_BYTE_UNCHECKED(v));

    switch (kind.byte) {

      case REB_0_END:
        goto finished;


//==//// NULL ////////////////////////////////////////////////////////////==//
//
// Since nulled cells can't be in BLOCK!s, the evaluator shouldn't usually see
// them.  Plus Q APIs quotes spliced values, so `rebValueQ("null?", nullptr)`
// gets a QUOTED! that evaluates to null--it's not a null being evaluated.
//
// But plain `rebValue("null?", nullptr)` would be an error.  Another way
// the evaluator can see NULL is REEVAL, such as `reeval first []`.

      case REB_NULLED:
        fail (Error_Evaluate_Null_Raw());


//==//// VOID! ///////////////////////////////////////////////////////////==//
//
// "A void! is a means of giving a hot potato back that is a warning about
//  something, but you don't want to force an error 'in the moment'...in case
//  the returned information wasn't going to be used anyway."
//
// https://forum.rebol.info/t/947
//
// If we get here, the evaluator is actually seeing it, and it's time to fail.

      case REB_VOID:
        fail (Error_Void_Evaluation_Raw());


//==//// ACTION! /////////////////////////////////////////////////////////==//
//
// If an action makes it to the SWITCH statement, that means it is either
// literally an action value in the array (`do compose [1 (:+) 2]`) or is
// being retriggered via EVAL.
//
// Most action evaluations are triggered from a WORD! or PATH!, which jumps in
// at the `process_action` label.

      case REB_ACTION: {
        REBSTR *opt_label = nullptr;  // not run from WORD!/PATH!, "nameless"

        DECLARE_FRAME (subframe, f->feed, f->flags.bits & ~(EVAL_FLAG_ALLOCATED_FEED));
        Push_Frame(f->out, subframe);
        Push_Action(subframe, VAL_ACTION(v), VAL_BINDING(v));
        Begin_Prefix_Action(subframe, opt_label);

        // We'd like `10 -> = 5 + 5` to work, and to do so it reevaluates in
        // a new frame, but has to run the `=` as "getting its next arg from
        // the output slot, but not being run in an enfix mode".
        //
        if (NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT))
            Expire_Out_Cell_Unless_Invisible(subframe);

        goto process_action; }

      process_action: {  // Note: Also jumped to by the redo_checked code
        //
        // !!! Originally, there was no such thing as identity for a FRAME!
        // that wasn't running a function.
        //
        return R_CONTINUATION;
      }


//==//// WORD! ///////////////////////////////////////////////////////////==//
//
// A plain word tries to fetch its value through its binding.  It will fail
// and longjmp out of this stack if the word is unbound (or if the binding is
// to a variable which is not set).  Should the word look up to an action,
// then that action will be called by jumping to the ACTION! case.
//
// NOTE: The usual dispatch of enfix functions is *not* via a REB_WORD in this
// switch, it's by some code at the `post_switch:` label.  So you only see
// enfix in cases like `(+ 1 2)`, or after PARAMLIST_IS_INVISIBLE e.g.
// `10 comment "hi" + 20`.

      case REB_WORD:
        if (not gotten)
            gotten = Lookup_Word_May_Fail(v, f_specifier);

        if (IS_ACTION(gotten)) {  // before IS_VOID() is common case
            REBACT *act = VAL_ACTION(gotten);

            if (GET_ACTION_FLAG(act, ENFIXED)) {
                if (
                    GET_ACTION_FLAG(act, POSTPONES_ENTIRELY)
                    or GET_ACTION_FLAG(act, DEFERS_LOOKBACK)
                ){
                    if (GET_EVAL_FLAG(f, FULFILLING_ARG)) {
                        CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
                        SET_FEED_FLAG(f->feed, DEFERRING_ENFIX);
                        SET_END(f->out);
                        goto finished;
                    }
                }
            }

            DECLARE_FRAME (subframe, f->feed, f->flags.bits & ~(EVAL_FLAG_ALLOCATED_FEED));
            Push_Frame(f->out, subframe);
            Push_Action(subframe, act, VAL_BINDING(gotten));
            Begin_Action_Core(
                subframe,
                VAL_WORD_SPELLING(v),  // use word as label
                GET_ACTION_FLAG(act, ENFIXED)
            );
            goto process_action;
        }

        if (IS_VOID(gotten))  // need GET/ANY if it's void ("undefined")
            fail (Error_Need_Non_Void_Core(v, f_specifier));

        Move_Value(f->out, gotten);  // no copy CELL_FLAG_UNEVALUATED
        break;


//==//// SET-WORD! ///////////////////////////////////////////////////////==//
//
// Right hand side is evaluated into `out`, and then copied to the variable.
//
// All values are allowed in these assignments, including NULL and VOID!
// https://forum.rebol.info/t/1206

      case REB_SET_WORD: {
        if (Rightward_Evaluate_Nonvoid_Into_Out_Throws(f, v))  // see notes
            goto return_thrown;

      set_word_with_out:

        Move_Value(Sink_Word_May_Fail(v, f_specifier), f->out);
        break; }


//==//// GET-WORD! ///////////////////////////////////////////////////////==//
//
// A GET-WORD! does no dispatch on functions.  It will fetch other values as
// normal, but will error on VOID! and direct you to GET/ANY.  This matches
// Rebol2 behavior, choosing to break with R3-Alpha and Red which will give
// back "voided" values ("UNSET!")...to make typos less likely to bite those
// who wanted to use ACTION!s inertly:
// https://forum.rebol.info/t/should-get-word-of-a-void-raise-an-error/1301

      case REB_GET_WORD:
        if (not gotten)
            gotten = Lookup_Word_May_Fail(v, f_specifier);

        if (IS_VOID(gotten))
            fail (Error_Need_Non_Void_Core(v, f_specifier));

        Move_Value(f->out, gotten);
        break;


//==//// GROUP! ///////////////////////////////////////////////////////////=//
//
// If a GROUP! is seen then it generates another call into Eval_Core().  The
// current frame is not reused, as the source array from which values are
// being gathered changes.
//
// Empty groups vaporize, as do ones that only consist of invisibles.  If
// this is not desired, one should use DO or lead with `(void ...)`
//
//     >> 1 + 2 (comment "vaporize")
//     == 3
//
//     >> 1 + () 2
//     == 3

      case REB_GROUP: {
        f_next_gotten = nullptr;  // arbitrary code changes fetched variables

        // !!! here we alter *this* frame's executor to be the group
        // executor.  This effectively hijacks it from re-entry.  We make a
        // memory in the frame spare of whether or not it was already a
        // continuation so it can put it back.
        //
        if (GET_EVAL_FLAG(f, CONTINUATION))
            Init_True(f_spare);
        else {
            Init_False(f_spare);
            SET_EVAL_FLAG(f, CONTINUATION);
        }
        assert(f->executor == &Eval_New_Expression);
        f->executor = &Group_Executor;

        REBFLGS flags = EVAL_MASK_DEFAULT
            | EVAL_FLAG_CONTINUATION
            | EVAL_FLAG_TO_END;
        DECLARE_FRAME_AT_CORE (subframe, v, f_specifier, flags);

        // See notes on Group_Executor().  We don't want to lose the value in
        // f->out if nothing comes up, but we do need a flag to tell if no
        // new value was produced (e.g. all comments or nothing)
        //
        assert(IS_END(f->out) or GET_CELL_FLAG(f->out, OUT_MARKED_STALE));

        Push_Frame(f->out, subframe);
        return R_CONTINUATION; }


//==//// PATH! ///////////////////////////////////////////////////////////==//
//
// Paths starting with inert values do not evaluate.  `/foo/bar` has a blank
// at its head, and it evaluates to itself.
//
// Other paths run through the GET-PATH! mechanism and then EVAL the result.
// If the get of the path is null, then it will be an error.

      case REB_PATH: {
        assert(VAL_INDEX_UNCHECKED(v) == 0);  // this is the rule for now

        if (ANY_INERT(ARR_HEAD(VAL_ARRAY(v)))) {
            //
            // !!! TODO: Make special exception for `/` here, look up function
            // it is bound to.
            //
            Derelativize(f->out, v, f_specifier);
            break;
        }

        REBVAL *where = GET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT)
            ? f_spare
            : f->out;

        REBSTR *opt_label;
        if (Eval_Path_Throws_Core(
            where,
            &opt_label,  // requesting says we run functions (not GET-PATH!)
            VAL_ARRAY(v),
            VAL_INDEX(v),
            Derive_Specifier(f_specifier, v),
            nullptr,  // `setval`: null means don't treat as SET-PATH!
            EVAL_MASK_DEFAULT | EVAL_FLAG_PUSH_PATH_REFINES
        )){
            if (where != f->out)
                Move_Value(f->out, where);
            goto return_thrown;
        }

        if (IS_ACTION(where)) {  // try this branch before fail on void+null
            REBACT *act = VAL_ACTION(where);

            // PATH! dispatch is costly and can error in more ways than WORD!:
            //
            //     e: trap [do make block! ":a"] e/id = 'not-bound
            //                                   ^-- not ready @ lookahead
            //
            // Plus with GROUP!s in a path, their evaluations can't be undone.
            //
            if (GET_ACTION_FLAG(act, ENFIXED))
                fail ("Use `<-` to shove left enfix operands into PATH!s");

            // !!! Review if invisibles can be supported without ->
            //
            if (GET_ACTION_FLAG(act, IS_INVISIBLE))
                fail ("Use `<-` with invisibles fetched from PATH!");

            DECLARE_FRAME (subframe, f->feed, f->flags.bits & ~(EVAL_FLAG_ALLOCATED_FEED));
            Push_Frame(f->out, subframe);

            // !!! The refinements were pushed in the `f` frame but we want
            // subframe to see the same stack marker.  Extremely inelegant,
            // rethink things if this ever works.
            //
            subframe->dsp_orig = f->dsp_orig;

            Push_Action(subframe, VAL_ACTION(where), VAL_BINDING(where));
            Begin_Prefix_Action(subframe, opt_label);

            if (where == subframe->out)
                Expire_Out_Cell_Unless_Invisible(subframe);

            goto process_action;
        }

        if (IS_VOID(where))  // need `:x/y` if it's void (unset)
            fail (Error_Need_Non_Void_Core(v, f_specifier));

        if (where != f->out)
            Move_Value(f->out, where);  // won't move CELL_FLAG_UNEVALUATED
        else
            CLEAR_CELL_FLAG(f->out, UNEVALUATED);
        break; }


//==//// SET-PATH! ///////////////////////////////////////////////////////==//
//
// See notes on SET-WORD!  SET-PATH!s are handled in a similar way, by
// pushing them to the stack, continuing the evaluation via a lightweight
// reuse of the current frame.
//
// !!! The evaluation ordering is dictated by the fact that there isn't a
// separate "evaluate path to target location" and "set target' step.  This
// is because some targets of assignments (e.g. gob/size/x:) do not correspond
// to a cell that can be returned; the path operation "encodes as it goes"
// and requires the value to set as a parameter to Eval_Path.  Yet it is
// counterintuitive given the "left-to-right" nature of the language:
//
//     >> foo: make object! [[bar][bar: 10]]
//
//     >> foo/(print "left" 'bar): (print "right" 20)
//     right
//     left
//     == 20
//
// Note that nulled cells are allowed: https://forum.rebol.info/t/895/4

      case REB_SET_PATH: {
        if (Rightward_Evaluate_Nonvoid_Into_Out_Throws(f, v))
            goto return_thrown;

      set_path_with_out:

        if (Eval_Path_Throws_Core(
            f_spare,  // output if thrown, used as scratch space otherwise
            nullptr,  // not requesting symbol means refinements not allowed
            VAL_ARRAY(v),
            VAL_INDEX(v),
            Derive_Specifier(f_specifier, v),
            f->out,
            EVAL_MASK_DEFAULT  // evaluating GROUP!s ok
        )){
            Move_Value(f->out, f_spare);
            goto return_thrown;
        }

        break; }


//==//// GET-PATH! ///////////////////////////////////////////////////////==//
//
// Note that the GET native on a PATH! won't allow GROUP! execution:
//
//    foo: [X]
//    path: 'foo/(print "side effect!" 1)
//    get path  ; not allowed, due to surprising side effects
//
// However a source-level GET-PATH! allows them, since they are at the
// callsite and you are assumed to know what you are doing:
//
//    :foo/(print "side effect" 1)  ; this is allowed
//
// Consistent with GET-WORD!, a GET-PATH! acts as GET and won't return VOID!.

      case REB_GET_PATH:
        if (Get_Path_Throws_Core(f->out, v, f_specifier))
            goto return_thrown;

        if (IS_VOID(f->out))  // need GET/ANY if it's void ("undefined")
            fail (Error_Need_Non_Void_Core(v, f_specifier));

        // !!! This didn't appear to be true for `-- "hi" "hi"`, processing
        // GET-PATH! of a variadic.  Review if it should be true.
        //
        /* assert(NOT_CELL_FLAG(f->out, CELL_FLAG_UNEVALUATED)); */
        CLEAR_CELL_FLAG(f->out, UNEVALUATED);
        break;


//==//// GET-GROUP! //////////////////////////////////////////////////////==//
//
// Evaluates the group, and then executes GET-WORD!/GET-PATH!/GET-BLOCK!
// operation on it, if it's a WORD! or a PATH! or BLOCK!.  If it's an arity-0
// action, it is allowed to execute as a form of "functional getter".

      case REB_GET_GROUP: {
        f_next_gotten = nullptr;  // arbitrary code changes fetched variables

        if (Do_Any_Array_At_Throws(f_spare, v, f_specifier)) {
            Move_Value(f->out, f_spare);
            goto return_thrown;
        }

        if (ANY_WORD(f_spare))
            kind.byte
                = mutable_KIND_BYTE(f_spare)
                = mutable_MIRROR_BYTE(f_spare)
                = REB_GET_WORD;
        else if (ANY_PATH(f_spare))
            kind.byte
                = mutable_KIND_BYTE(f_spare)
                = mutable_MIRROR_BYTE(f_spare)
                = REB_GET_PATH;
        else if (ANY_BLOCK(f_spare))
            kind.byte
                = mutable_KIND_BYTE(f_spare)
                = mutable_MIRROR_BYTE(f_spare)
                = REB_GET_BLOCK;
        else if (IS_ACTION(f_spare)) {
            if (Eval_Value_Throws(f->out, f_spare, SPECIFIED))  // only 0-args
                goto return_thrown;
            goto post_switch;
        }
        else
            fail (Error_Bad_Get_Group_Raw());

        v = f_spare;
        f_next_gotten = nullptr;

        goto reevaluate; }


//==//// SET-GROUP! //////////////////////////////////////////////////////==//
//
// Synonym for SET on the produced thing, unless it's an action...in which
// case an arity-1 function is allowed to be called and passed the right.

      case REB_SET_GROUP: {
        //
        // Protocol for all the REB_SET_XXX is to evaluate the right before
        // the left.  Same with SET_GROUP!.  (Consider in particular the case
        // of PARSE, where it has to hold the SET-GROUP! in suspension while
        // it looks on the right in order to decide if it will run it at all!)
        //
        if (Rightward_Evaluate_Nonvoid_Into_Out_Throws(f, v))
            goto return_thrown;

        f_next_gotten = nullptr;  // arbitrary code changes fetched variables

        if (Do_Any_Array_At_Throws(f_spare, v, f_specifier)) {
            Move_Value(f->out, f_spare);
            goto return_thrown;
        }

        if (IS_ACTION(f_spare)) {
            //
            // Apply the function, and we can reuse this frame to do it.
            //
            // !!! But really it should not be allowed to take more than one
            // argument.  Hence rather than go through reevaluate, channel
            // it through a variant of the enfix machinery (the way that
            // CHAIN does, which similarly reuses the frame but probably
            // should also be restricted to a single value...though it's
            // being experimented with letting it take more.)
            //
            DECLARE_FRAME (subframe, f->feed, f->flags.bits & ~(EVAL_FLAG_ALLOCATED_FEED));
            Push_Frame(f->out, subframe);
            Push_Action(subframe, VAL_ACTION(f_spare), VAL_BINDING(f_spare));
            Begin_Prefix_Action(subframe, nullptr);  // no label

            kind.byte = REB_ACTION;
            assert(NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT));
            SET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT);

            goto process_action;
        }

        v = f_spare;

        if (ANY_WORD(f_spare)) {
            kind.byte
                = mutable_KIND_BYTE(f_spare)
                = mutable_MIRROR_BYTE(f_spare)
                = REB_SET_WORD;
            goto set_word_with_out;
        }
        else if (ANY_PATH(f_spare)) {
            kind.byte
                = mutable_KIND_BYTE(f_spare)
                = mutable_MIRROR_BYTE(f_spare)
                = REB_SET_PATH;
            goto set_path_with_out;
        }
        else if (ANY_BLOCK(f_spare)) {
            kind.byte
                = mutable_KIND_BYTE(f_spare)
                = mutable_MIRROR_BYTE(f_spare)
                = REB_SET_BLOCK;

            // !!! This code used to be jumped to as part of the implementation of
            // SET-BLOCK!, as "set_block_with_out".  It is likely to be discarded
            // in light of the new purpose of SET-BLOCK! as multiple returns,
            // but was moved here for now.

            if (IS_NULLED(f->out)) // `[x y]: null` is illegal
                fail (Error_Need_Non_Null_Core(v, f_specifier));

            const RELVAL *dest = VAL_ARRAY_AT(v);

            const RELVAL *src;
            if (IS_BLOCK(f->out))
                src = VAL_ARRAY_AT(f->out);
            else
                src = f->out;

            for (
                ;
                NOT_END(dest);
                ++dest,
                IS_END(src) or not IS_BLOCK(f->out) ? NOOP : (++src, NOOP)
            ){
                Set_Var_May_Fail(
                    dest,
                    f_specifier,
                    IS_END(src) ? BLANK_VALUE : src,  // R3-Alpha blanks > END
                    IS_BLOCK(f->out)
                        ? VAL_SPECIFIER(f->out)
                        : SPECIFIED,
                    false  // doesn't use "hard" semantics on groups in paths
                );
            }

            break;
        }

        fail (Error_Bad_Set_Group_Raw()); }


//==//// GET-BLOCK! //////////////////////////////////////////////////////==//
//
// !!! Currently just inert, awaiting future usage.

      case REB_GET_BLOCK:
        Derelativize(f->out, v, f_specifier);
        break;


//==//// SET-BLOCK! //////////////////////////////////////////////////////==//
//
// The evaluator treats SET-BLOCK! specially as a means for implementing
// multiple return values.  The trick is that it does so by pre-loading
// arguments in the frame with variables to update, in a way that could have
// historically been achieved with passing a WORD! or PATH! to a refinement.
// So if there was a function that updates a variable you pass in by name:
//
//     result: updating-function/update arg1 arg2 'var
//
// The /UPDATE parameter is marked as being effectively a "return value", so
// that equivalent behavior can be achieved with:
//
//     [result var]: updating-function arg1 arg2
//
// !!! This is an extremely slow-running prototype of the desired behavior.
// It is a mock up intended to find any flaws in the concept before writing
// a faster native version that would require rewiring the evaluator somewhat.

      case REB_SET_BLOCK: {
        assert(NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT));

        if (VAL_LEN_AT(v) == 0)
            fail ("SET-BLOCK! must not be empty for now.");

        RELVAL *check = VAL_ARRAY_AT(v);
        for (; NOT_END(check); ++check) {
            if (IS_BLANK(check) or IS_WORD(check) or IS_PATH(check))
                continue;
            fail ("SET-BLOCK! elements must be WORD/PATH/BLANK for now.");
        }

        if (not (IS_WORD(f_next) or IS_PATH(f_next) or IS_ACTION(f_next)))
            fail ("SET_BLOCK! must be followed by WORD/PATH/ACTION for now.");

        // Turn SET-BLOCK! into a BLOCK! in `f->out` for easier processing.
        //
        Derelativize(f->out, v, f_specifier);
        mutable_KIND_BYTE(f->out) = REB_BLOCK;
        mutable_MIRROR_BYTE(f->out) = REB_BLOCK;

        // Get the next argument as an ACTION!, specialized if necessary, into
        // the `spare`.  We'll specialize it further to set any output
        // arguments to words from the left hand side.
        //
        if (Get_If_Word_Or_Path_Throws(
            f_spare,
            nullptr,
            f_next,
            f_specifier,
            false
        )){
            goto return_thrown;
        }

        if (not IS_ACTION(f_spare))
            fail ("SET-BLOCK! is only allowed to have ACTION! on right ATM.");

        // Find all the "output" parameters.  Right now that's any parameter
        // which is marked as being legal to be word! or path! *specifically*.
        //
        const REBU64 ts_out = FLAGIT_KIND(REB_TS_REFINEMENT)
            | FLAGIT_KIND(REB_NULLED)
            | FLAGIT_KIND(REB_WORD)
            | FLAGIT_KIND(REB_PATH);

        REBDSP dsp_outputs = DSP;
        REBVAL *temp = VAL_ACT_PARAMS_HEAD(f_spare);
        for (; NOT_END(temp); ++temp) {
            if (not TYPE_CHECK_EXACT_BITS(temp, ts_out))
                continue;
            Init_Word(DS_PUSH(), VAL_TYPESET_STRING(temp));
        }

        DECLARE_LOCAL(outputs);
        Init_Block(outputs, Pop_Stack_Values(dsp_outputs));
        PUSH_GC_GUARD(outputs);

        // !!! You generally don't want to use the API inside the evaluator
        // (this is only a temporary measure).  But if you do, you can't use
        // it inside of a function that has not fulfilled its arguments.
        // So imagine `10 = [a b]: some-func`... the `=` is building a frame
        // with two arguments, and it has the 10 fulfilled but the other
        // cell is invalid bits.  So when the API handle tries to attach its
        // ownership it forces reification of a frame that's partial.  We
        // have to give the API handle a fulfilled frame to stick to, so
        // we wrap in a function that we make look like it ran and got all
        // its arguments.
        //
        DECLARE_END_FRAME(dummy, EVAL_MASK_DEFAULT);
        Push_Dummy_Frame(dummy);

        // Now create a function to splice in to the execution stream that
        // specializes what we are calling so the output parameters have
        // been preloaded with the words or paths from the left block.
        //
        REBVAL *specialized = rebValue(
            "enclose specialize", rebQ1(f_spare), "collect [ use [block] [",
                "block: next", f->out,
                "for-each output", outputs, "["
                    "if tail? block [break]",  // no more outputs wanted
                    "if block/1 [",  // interested in this result
                        "keep setify output",
                        "keep quote compose block/1",  // pre-compose, safety
                    "]",
                    "block: next block",
                "]",
                "if not tail? block [fail {Too many multi-returns}]",
            "] ] func [f] [",
                "for-each output", outputs, "[",
                    "if f/(output) [",  // void in case func doesn't (null?)
                        "set f/(output) void",
                    "]",
                "]",
                "either first", f->out, "[",
                    "set first", f->out, "do f",
                "] [do f]",
            "]",
        rebEND);

        DROP_GC_GUARD(outputs);

        Move_Value(f_spare, specialized);
        rebRelease(specialized);

        Drop_Dummy_Frame_Unbalanced(dummy);

        // Toss away the pending WORD!/PATH!/ACTION! that was in the execution
        // stream previously.
        //
        Fetch_Next_Forget_Lookback(f);

        // Interject the function with our multiple return arguments and
        // return value assignment step.
        //
        gotten = f_spare;
        v = f_spare;
        kind.byte = KIND_BYTE(v);

        goto reevaluate; }


//==//////////////////////////////////////////////////////////////////////==//
//
// Treat all the other Is_Bindable() types as inert
//
//==//////////////////////////////////////////////////////////////////////==//

      case REB_BLOCK:
        //
      case REB_SYM_BLOCK:
      case REB_SYM_GROUP:
      case REB_SYM_PATH:
      case REB_SYM_WORD:
        //
      case REB_BINARY:
        //
      case REB_TEXT:
      case REB_FILE:
      case REB_EMAIL:
      case REB_URL:
      case REB_TAG:
      case REB_ISSUE:
        //
      case REB_BITSET:
        //
      case REB_MAP:
        //
      case REB_VARARGS:
        //
      case REB_OBJECT:
      case REB_FRAME:
      case REB_MODULE:
      case REB_ERROR:
      case REB_PORT:
        goto inert;


//==//////////////////////////////////////////////////////////////////////==//
//
// Treat all the other not Is_Bindable() types as inert
//
//==//////////////////////////////////////////////////////////////////////==//

      case REB_BLANK:
        //
      case REB_LOGIC:
      case REB_INTEGER:
      case REB_DECIMAL:
      case REB_PERCENT:
      case REB_MONEY:
      case REB_CHAR:
      case REB_PAIR:
      case REB_TUPLE:
      case REB_TIME:
      case REB_DATE:
        //
      case REB_DATATYPE:
      case REB_TYPESET:
        //
      case REB_EVENT:
      case REB_HANDLE:

      case REB_CUSTOM:  // custom types (IMAGE!, VECTOR!) are all inert

      inert:

        Inertly_Derelativize_Inheriting_Const(f->out, v, f->feed);
        break;


//=//// QUOTED! (at 4 or more levels of escaping) /////////////////////////=//
//
// This is the form of literal that's too escaped to just overlay in the cell
// by using a higher kind byte.  See the `default:` case in this switch for
// handling of the more compact forms, that are much more common.
//
// (Highly escaped literals should be rare, but for completeness you need to
// be able to escape any value, including any escaped one...!)

      case REB_QUOTED:
        Derelativize(f->out, v, f_specifier);
        Unquotify(f->out, 1);  // take off one level of quoting
        break;


//==//// QUOTED! (at 3 levels of escaping or less...or just garbage) //////=//
//
// All the values for types at >= REB_64 currently represent the special
// compact form of literals, which overlay inside the cell they escape.
// The real type comes from the type modulo 64.

      default:
        Derelativize(f->out, v, f_specifier);
        Unquotify_In_Situ(f->out, 1);  // checks for illegal REB_XXX bytes
        break;
    }


//=//// END MAIN SWITCH STATEMENT /////////////////////////////////////////=//

    // The UNEVALUATED flag is one of the bits that doesn't get copied by
    // Move_Value() or Derelativize().  Hence it can be overkill to clear it
    // off if one knows a value came from doing those things.  This test at
    // the end checks to make sure that the right thing happened.
    //
    // !!! Stackless processing messes with this because we don't do a
    // recursion so the `kind.byte` is out of date.  Review.
    /*
    if (ANY_INERT_KIND(kind.byte)) {  // if() so as to check which part failed
        assert(GET_CELL_FLAG(f->out, UNEVALUATED));
    }
    else if (GET_CELL_FLAG(f->out, UNEVALUATED)) {
        //
        // !!! Should ONLY happen if we processed a WORD! that looked up to
        // an invisible function, and left something behind that was not
        // previously evaluative.  To track this accurately, we would have
        // to use an EVAL_FLAG_DEBUG_INVISIBLE_UNEVALUATIVE here, because we
        // don't have the word anymore to look up (and even if we did, what
        // it looks up to may have changed).
        //
        assert(kind.byte == REB_WORD or ANY_INERT(f->out));
    }
    */

  post_switch:
    f->executor = &Eval_Post_Switch;
    return R_CONTINUATION;

    // Stay THROWN and let stack levels above try and catch

  return_thrown:
    return R_THROWN;

  finished:
    return f->out;
}


//
//  Eval_Post_Switch: C
//
// When we are sitting at what "looks like the end" of an evaluation step, we
// still have to consider enfix.  e.g.
//
//    evaluate @val [1 + 2 * 3]
//
// We want that to give a position of [] and `val = 9`.  The evaluator
// cannot just dispatch on REB_INTEGER in the switch() above, give you 1,
// and consider its job done.  It has to notice that the word `+` looks up
// to an ACTION! that was assigned with SET/ENFIX, and keep going.
//
// Next, there's a subtlety with FEED_FLAG_NO_LOOKAHEAD which explains why
// processing of the 2 argument doesn't greedily continue to advance, but
// waits for `1 + 2` to finish.
//
// Slightly more nuanced is why PARAMLIST_IS_INVISIBLE functions have to be
// considered in the lookahead also.  Consider this case:
//
//    evaluate @val [1 + 2 * comment ["hi"] 3 4 / 5]
//
// We want `val = 9`, with `pos = [4 / 5]`.  To do this, we can't consider an
// evaluation finished until all the "invisibles" have been processed.
//
// If that's not enough to consider :-) it can even be the case that
// subsequent enfix gets "deferred".  Then, possibly later the evaluated
// value gets re-fed back in, and we jump right to this post-switch point
// to give it a "second chance" to take the enfix.  (See 'deferred'.)
//
// So this post-switch step is where all of it happens, and it's tricky!
//
REB_R Eval_Post_Switch(REBFRM *f)
{
    // If something was run with the expectation it should take the next arg
    // from the output cell, and an evaluation cycle ran that wasn't an
    // ACTION! (or that was an arity-0 action), that's not what was meant.
    // But it can happen, e.g. `x: 10 | x <-`, where <- doesn't get an
    // opportunity to quote left because it has no argument...and instead
    // retriggers and lets x run.

    if (GET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT)) {
        if (GET_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH))
            fail (Error_Literal_Left_Path_Raw());

        assert(!"Unexpected lack of use of NEXT_ARG_FROM_OUT");
    }

//=//// IF NOT A WORD!, IT DEFINITELY STARTS A NEW EXPRESSION /////////////=//

    // For long-pondered technical reasons, only WORD! is able to dispatch
    // enfix.  If it's necessary to dispatch an enfix function via path, then
    // a word is used to do it, like `->` in `x: -> lib/method [...] [...]`.

    REBYTE kind_byte = KIND_BYTE(f_next);

    if (kind_byte == REB_0_END) {
        CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
        goto finished;  // hitting end is common, avoid do_next's switch()
    }

    if (kind_byte == REB_PATH) {
        if (
            GET_FEED_FLAG(f->feed, NO_LOOKAHEAD)
            or VAL_LEN_AT(f_next) != 2
            or not IS_BLANK(ARR_AT(VAL_ARRAY(f_next), 0))
            or not IS_BLANK(ARR_AT(VAL_ARRAY(f_next), 1))
        ){
            CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
            goto finished;
        }

        // We had something like `5 + 5 / 2 + 3`.  This is a special form of
        // path dispatch tentatively called "path splitting" (as opposed to
        // `a/b` which is "path picking").  For the moment, this is not
        // handled as a parameterization to the PD_Xxx() functions, nor is it
        // a separate dispatch like PS_Xxx()...but it just performs division
        // compatibly with history.

        DECLARE_FRAME(subframe, f->feed, f->flags.bits & ~(EVAL_FLAG_ALLOCATED_FEED));
        Push_Frame(f->out, subframe);

        REBNOD *binding = nullptr;
        Push_Action(subframe, NAT_ACTION(path_0), binding);

        REBSTR *opt_label = nullptr;
        Begin_Enfix_Action(subframe, opt_label);

        Fetch_Next_Forget_Lookback(subframe);  // advances f->value
        goto process_action;
    }

    if (kind_byte != REB_WORD) {
        CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
        goto finished;
    }

//=//// FETCH WORD! TO PERFORM SPECIAL HANDLING FOR ENFIX/INVISIBLES //////=//

    // First things first, we fetch the WORD! (if not previously fetched) so
    // we can see if it looks up to any kind of ACTION! at all.

    if (not f_next_gotten)
        f_next_gotten = Try_Lookup_Word(f_next, f_specifier);
    else
        assert(f_next_gotten == Try_Lookup_Word(f_next, f_specifier));

//=//// NEW EXPRESSION IF UNBOUND, NON-FUNCTION, OR NON-ENFIX /////////////=//

    // These cases represent finding the start of a new expression.
    //
    // Fall back on word-like "dispatch" even if ->gotten is null (unset or
    // unbound word).  It'll be an error, but that code path raises it for us.

    if (
        not f_next_gotten
        or not IS_ACTION(f_next_gotten)
        or not GET_ACTION_FLAG(VAL_ACTION(f_next_gotten), ENFIXED)
    ){
      lookback_quote_too_late: // run as if starting new expression

        CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);

        // Since it's a new expression, EVALUATE doesn't want to run it
        // even if invisible, as it's not completely invisible (enfixed)
        //
        goto finished;
    }

//=//// IT'S A WORD ENFIXEDLY TIED TO A FUNCTION (MAY BE "INVISIBLE") /////=//

    if (GET_ACTION_FLAG(VAL_ACTION(f_next_gotten), QUOTES_FIRST)) {
        //
        // Left-quoting by enfix needs to be done in the lookahead before an
        // evaluation, not this one that's after.  This happens in cases like:
        //
        //     left-lit: enfix func [:value] [:value]
        //     lit <something> left-lit
        //
        // But due to the existence of <end>-able and <skip>-able parameters,
        // the left quoting function might be okay with seeing nothing on the
        // left.  Start a new expression and let it error if that's not ok.
        //
        assert(NOT_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH));
        if (GET_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH))
            fail (Error_Literal_Left_Path_Raw());

        REBVAL *first = First_Unspecialized_Param(VAL_ACTION(f_next_gotten));
        if (VAL_PARAM_CLASS(first) == REB_P_SOFT_QUOTE) {
            if (GET_FEED_FLAG(f->feed, NO_LOOKAHEAD)) {
                CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
                goto finished;
            }
        }
        else if (NOT_EVAL_FLAG(f, INERT_OPTIMIZATION))
            goto lookback_quote_too_late;
    }

    if (
        GET_EVAL_FLAG(f, FULFILLING_ARG)
        and not (
            GET_ACTION_FLAG(VAL_ACTION(f_next_gotten), DEFERS_LOOKBACK)
                                       // ^-- `1 + if false [2] else [3]` => 4
            or GET_ACTION_FLAG(VAL_ACTION(f_next_gotten), IS_INVISIBLE)
                                       // ^-- `1 + 2 + comment "foo" 3` => 6
        )
    ){
        if (GET_FEED_FLAG(f->feed, NO_LOOKAHEAD)) {
            // Don't do enfix lookahead if asked *not* to look.

            CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);

            assert(NOT_FEED_FLAG(f->feed, DEFERRING_ENFIX));
            SET_FEED_FLAG(f->feed, DEFERRING_ENFIX);

            goto finished;
        }

        CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
    }

    // A deferral occurs, e.g. with:
    //
    //     return if condition [...] else [...]
    //
    // The first time the ELSE is seen, IF is fulfilling its branch argument
    // and doesn't know if its done or not.  So this code senses that and
    // runs, returning the output without running ELSE, but setting a flag
    // to know not to do the deferral more than once.
    //
    if (
        GET_EVAL_FLAG(f, FULFILLING_ARG)
        and (
            GET_ACTION_FLAG(VAL_ACTION(f_next_gotten), POSTPONES_ENTIRELY)
            or (
                GET_ACTION_FLAG(VAL_ACTION(f_next_gotten), DEFERS_LOOKBACK)
                and NOT_FEED_FLAG(f->feed, DEFERRING_ENFIX)
            )
        )
    ){
        if (GET_EVAL_FLAG(f->prior, ERROR_ON_DEFERRED_ENFIX)) {
            //
            // Operations that inline functions by proxy (such as MATCH and
            // ENSURE) cannot directly interoperate with THEN or ELSE...they
            // are building a frame with PG_Dummy_Action as the function, so
            // running a deferred operation in the same step is not an option.
            // The expression to the left must be in a GROUP!.
            //
            fail (Error_Ambiguous_Infix_Raw());
        }

        CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);

        if (
            Is_Action_Frame(f->prior)
            //
            // ^-- !!! Before stackless it was always the case when we got
            // here that a function frame was fulfilling, because SET-WORD!
            // would reuse frames while fulfilling arguments...but stackless
            // changed this and has SET-WORD! start new frames.  Review.
            //
            and not Is_Action_Frame_Fulfilling(f->prior)
        ){
            // This should mean it's a variadic frame, e.g. when we have
            // the 2 in the output slot and are at the THEN in:
            //
            //     variadic2 1 2 then (t => [print ["t is" t] <then>])
            //
            // We want to treat this like a barrier.
            //
            SET_FEED_FLAG(f->feed, BARRIER_HIT);
            goto finished;
        }

        SET_FEED_FLAG(f->feed, DEFERRING_ENFIX);

        // Leave the enfix operator pending in the frame, and it's up to the
        // parent frame to decide whether to change the executor and use
        // Eval_Post_Switch to jump back in and finish fulfilling this arg or
        // not.  If it does resume and we get to this check again,
        // f->prior->deferred can't be null, else it'd be an infinite loop.
        //
        goto finished;
    }

    CLEAR_FEED_FLAG(f->feed, DEFERRING_ENFIX);

    // An evaluative lookback argument we don't want to defer, e.g. a normal
    // argument or a deferable one which is not being requested in the context
    // of parameter fulfillment.  We want to reuse the f->out value and get it
    // into the new function's frame.

    DECLARE_FRAME (subframe, f->feed, f->flags.bits & ~(EVAL_FLAG_ALLOCATED_FEED));
    Push_Frame(f->out, subframe);
    Push_Action(subframe, VAL_ACTION(f_next_gotten), VAL_BINDING(f_next_gotten));
    Begin_Enfix_Action(subframe, VAL_WORD_SPELLING(f_next));

    Fetch_Next_Forget_Lookback(subframe);  // advances next
  process_action:
    return R_CONTINUATION;


  finished:
    return f->out;  // not thrown
}
