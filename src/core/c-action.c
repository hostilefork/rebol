//
//  File: %c-action.c
//  Summary: "Evaluation for Action Frames"
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
// !!! The evaluator is being transitioned from something more monolithic
// (with gotos for efficiency) into more of a state machine that operates on
// stacks of frame objects (REBFRM*).  As a step in that process, frames
// which represent an action can have their handling broke out into a
// separate source file.  This is a work in progress as the stackless change
// goes further.
//

#include "sys-core.h"


//
//  Dispatch_Internal: C
//
// Default function provided for the hook at the moment of action application,
// with all arguments gathered.
//
// As this is the default, it does nothing besides call the phase dispatcher.
// Debugging and instrumentation might want to do other things...e.g TRACE
// wants to preface the call by dumping the frame, and postfix it by showing
// the evaluative result.
//
// !!! Review if lower-level than C tricks could be used to patch code in
// some builds to not pay the cost for calling through a pointer.
//
REB_R Dispatch_Internal(REBFRM * const f)
  { return ACT_DISPATCHER(FRM_PHASE(f))(f); }


//=//// ARGUMENT LOOP MODES ///////////////////////////////////////////////=//
//
// f->special is kept in sync with one of three possibilities:
//
// * f->param to indicate ordinary argument fulfillment for all the relevant
//   args, refinements, and refinement args of the function.
//
// * f->arg, to indicate that the arguments should only be type-checked.
//
// * some other pointer to an array of REBVAL which is the same length as the
//   argument list.  Any non-null values in that array should be used in lieu
//   of an ordinary argument...e.g. that argument has been "specialized".
//
// All the states can be incremented across the length of the frame.  This
// means `++f->special` can be done without checking for null values.
//
// Additionally, in the f->param state, f->special will never register as
// anything other than a parameter.  This can speed up some checks, such as
// where `IS_NULLED(f->special)` can only match the other two cases.
//
// Done with macros for speed in the debug build (which does not inline).
// The name of the trigger condition is included since reinforcing what's true
// at the callsite is good to help understand the state.

#define SPECIAL_IS_ARG_SO_TYPECHECKING \
    (f->special == f->arg)

#define SPECIAL_IS_PARAM_SO_UNSPECIALIZED \
    (f->special == f->param)

#define SPECIAL_IS_ARBITRARY_SO_SPECIALIZED \
    (f->special != f->param and f->special != f->arg)


// It's called "Finalize" because in addition to checking, any other handling
// that an argument needs once being put into a frame is handled.  VARARGS!,
// for instance, that may come from an APPLY need to have their linkage
// updated to the parameter they are now being used in.
//
inline static void Finalize_Arg(REBFRM *f) {
    assert(not Is_Param_Variadic(f->param));  // use Finalize_Variadic_Arg()
    assert(VAL_PARAM_CLASS(f->param) != REB_P_LOCAL);

    REBYTE kind_byte = KIND_BYTE(f->arg);

    if (kind_byte == REB_0_END) {
        //
        // Note: `1 + comment "foo"` => `1 +`, arg is END
        //
        if (not Is_Param_Endable(f->param))
            fail (Error_No_Arg(f, f->param));

        Init_Endish_Nulled(f->arg);
        SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
        return;
    }

  #if defined(DEBUG_STALE_ARGS)  // see notes on flag definition
    assert(NOT_CELL_FLAG(f->arg, ARG_MARKED_CHECKED));
  #endif

    if (
        kind_byte == REB_BLANK
        and TYPE_CHECK(f->param, REB_TS_NOOP_IF_BLANK)  // e.g. <blank> param
    ){
        SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
        SET_EVAL_FLAG(f, FULFILL_ONLY);
        return;
    }

    // If we're not just typechecking, apply constness if requested.
    //
    // !!! Should explicit mutability override, so people can say things like
    // `foo: func [...] mutable [...]` ?  This seems bad, because the contract
    // of the function hasn't been "tweaked", e.g. with reskinning.
    //
    if (not SPECIAL_IS_ARG_SO_TYPECHECKING)
        if (TYPE_CHECK(f->param, REB_TS_CONST))
            SET_CELL_FLAG(f->arg, CONST);

    // If the <dequote> tag was used on an argument, we want to remove the
    // quotes (and queue them to be added back in if the return was marked
    // with <requote>).
    //
    if (TYPE_CHECK(f->param, REB_TS_DEQUOTE_REQUOTE) and IS_QUOTED(f->arg)) {
        if (GET_EVAL_FLAG(f, FULFILL_ONLY)) {
            //
            // We can only take the quote levels off now if the function is
            // going to be run now.  Because if we are filling a frame to
            // reuse later, it would forget the f->dequotes count.
            //
            if (not TYPE_CHECK(f->param, CELL_KIND(VAL_UNESCAPED(f->arg))))
                fail (Error_Arg_Type(f, f->param, VAL_TYPE(f->arg)));

            SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
            return;
        }

        // Some routines want to requote but also want to be able to
        // return a null without turning it into a single apostrophe.
        // Use the heuristic that if the argument wasn't legally null,
        // then a returned null should duck the requote.
        //
        f->requotes += VAL_NUM_QUOTES(f->arg);
        if (CELL_KIND(VAL_UNESCAPED(f->arg)) == REB_NULLED)
            SET_EVAL_FLAG(f, REQUOTE_NULL);

        Dequotify(f->arg);
    }

    if (TYPE_CHECK(f->param, REB_TS_REFINEMENT)) {
        Typecheck_Refinement_And_Canonize(f->param, f->arg);
        return;
    }

    if (not Typecheck_Including_Quoteds(f->param, f->arg)) {
        fail (Error_Arg_Type(f, f->param, VAL_TYPE(f->arg)));
    }

    SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
}


// While "checking" the variadic argument we actually re-stamp it with
// this parameter and frame's signature.  It reuses whatever the original
// data feed was (this frame, another frame, or just an array from MAKE
// VARARGS!)
//
inline static void Finalize_Variadic_Arg_Core(REBFRM *f, bool enfix) {
    assert(Is_Param_Variadic(f->param));  // use Finalize_Arg()

    // Varargs are odd, because the type checking doesn't actually check the
    // types inside the parameter--it always has to be a VARARGS!.
    //
    if (not IS_VARARGS(f->arg))
        fail (Error_Not_Varargs(f, f->param, VAL_TYPE(f->arg)));

    // Store the offset so that both the arg and param locations can quickly
    // be recovered, while using only a single slot in the REBVAL.  But make
    // the sign denote whether the parameter was enfixed or not.
    //
    VAL_VARARGS_SIGNED_PARAM_INDEX(f->arg) =
        enfix
            ? -(f->arg - FRM_ARGS_HEAD(f) + 1)
            : f->arg - FRM_ARGS_HEAD(f) + 1;

    VAL_VARARGS_PHASE_NODE(f->arg) = NOD(FRM_PHASE(f));
    SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
}

#define Finalize_Variadic_Arg(f) \
    Finalize_Variadic_Arg_Core((f), false)

#define Finalize_Enfix_Variadic_Arg(f) \
    Finalize_Variadic_Arg_Core((f), true)


// When arguments are hard quoted or soft-quoted, they don't call into the
// evaluator to do it.  But they need to use the logic of the evaluator for
// noticing when to defer enfix:
//
//     foo: func [...] [
//          return lit 1 then ["this needs to be returned"]
//     ]
//
// If the first time the THEN was seen was not after the 1, but when the
// LIT ran, it would get deferred until after the RETURN.  This is not
// consistent with the pattern people expect.
//
// Returns TRUE if it set the flag.
//
bool Lookahead_To_Sync_Enfix_Defer_Flag(REBFED *feed) {
    assert(NOT_FEED_FLAG(feed, DEFERRING_ENFIX));
    assert(not feed->gotten);

    CLEAR_FEED_FLAG(feed, NO_LOOKAHEAD);

    if (not IS_WORD(feed->value))
        return false;

    feed->gotten = Try_Lookup_Word(feed->value, feed->specifier);

    if (not feed->gotten or not IS_ACTION(feed->gotten))
        return false;

    if (NOT_ACTION_FLAG(VAL_ACTION(feed->gotten), ENFIXED))
        return false;

    if (GET_ACTION_FLAG(VAL_ACTION(feed->gotten), DEFERS_LOOKBACK))
        SET_FEED_FLAG(feed, DEFERRING_ENFIX);
    return true;
}


// To allow frames to share feeds, the feed is held by pointer.  But that
// makes accessing things verbose.  Also, the meaning is usually "next" in
// the evaluator, since it only represents the current value very briefly as
// it is pulled into a local for processing.  These macros shorten + clarify.
//
#define f_spare         FRM_SPARE(f)
#define f_next          f->feed->value  // !!! never nullptr, check in debug?
#define f_next_gotten   f->feed->gotten
#define f_specifier     f->feed->specifier


//
//  Eval_Arg: C
//
REB_R Eval_Arg(REBFRM *f)
{
    if (GET_EVAL_FLAG(f, DOING_PICKUPS))
        goto fulfill_arg;

    // !!! If not an APPLY or a typecheck of existing values, the data
    // array which backs the frame may not have any initialization of
    // its bits.  The goal is to make it so that the GC uses the
    // f->param position to know how far the frame fulfillment is
    // gotten, and only mark those values.  Hoewver, there is also
    // a desire to differentiate cell formatting between "stack"
    // and "heap" to do certain optimizations.  After a recent change,
    // it's becoming more integrated by using pooled memory for the
    // args...however issues of stamping the bits remain.  This just
    // blindly formats them with NODE_FLAG_STACK to make the arg
    // initialization work, but it's in progress to do this more
    // subtly so that the frame can be left formatted as non-stack.
    //
    if (
        NOT_EVAL_FLAG(f, DOING_PICKUPS)
        and not SPECIAL_IS_ARG_SO_TYPECHECKING
    ){
        Prep_Stack_Cell(f->arg);  // improve...
    }
    else {
        // If the incoming series came from a heap frame, just put
        // a bit on it saying its a stack node for now--this will
        // stop some asserts.  The optimization is not enabled yet
        // which avoids reification on stack nodes of lower stack
        // levels--so it's not going to cause problems -yet-
        //
        f->arg->header.bits |= CELL_FLAG_STACK_LIFETIME;
    }

    assert(f->arg->header.bits & NODE_FLAG_CELL);
    assert(f->arg->header.bits & CELL_FLAG_STACK_LIFETIME);

    //=//// A /REFINEMENT ARG /////////////////////////////////////////////=//

    // Refinements can be tricky because the "visitation order" of the
    // parameters while walking across the parameter array might not
    // match the "consumption order" of the expressions that need to
    // be fetched from the callsite.  For instance:
    //
    //     foo: func [a /b [integer!] /c [integer!]] [...]
    //
    //     foo/b/c 10 20 30
    //     foo/c/b 10 20 30
    //
    // The first PATH! pushes /B to the top of stack, with /C below.
    // The second PATH! pushes /C to the top of stack, with /B below
    //
    // If the refinements can be popped off the stack in the order
    // that they are encountered, then this can be done in one pass.
    // Otherwise a second pass is needed.  But it is accelerated by
    // storing the parameter indices to revisit in the binding of the
    // words (e.g. /B and /C above) on the data stack.

    if (TYPE_CHECK(f->param, REB_TS_REFINEMENT)) {
        assert(NOT_EVAL_FLAG(f, DOING_PICKUPS));  // jump lower

        if (SPECIAL_IS_PARAM_SO_UNSPECIALIZED)  // args from callsite
            goto unspecialized_refinement;  // most common case (?)

        if (SPECIAL_IS_ARG_SO_TYPECHECKING) {
            if (NOT_CELL_FLAG(f->arg, ARG_MARKED_CHECKED))
                Typecheck_Refinement_And_Canonize(f->param, f->arg);
            goto continue_arg_loop;
        }

        // A specialization....

        if (GET_CELL_FLAG(f->special, ARG_MARKED_CHECKED)) {
            Move_Value(f->arg, f->special);
            SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
            goto continue_arg_loop;  // !!! Double-check?
        }

        // A non-checked SYM-WORD! with binding indicates a partial
        // refinement with parameter index that needs to be pushed
        // to top of stack, hence HIGHER priority for fulfilling
        // @ the callsite than any refinements added by a PATH!.
        //
        if (IS_SYM_WORD(f->special)) {
            REBLEN partial_index = VAL_WORD_INDEX(f->special);
            REBSTR *partial_canon = VAL_STORED_CANON(f->special);

            Init_Sym_Word(DS_PUSH(), partial_canon);
            INIT_BINDING(DS_TOP, f->varlist);
            INIT_WORD_INDEX(DS_TOP, partial_index);
        }
        else
            assert(IS_NULLED(f->special));

    //=//// UNSPECIALIZED REFINEMENT SLOT /////////////////////////////////=//

    // We want to fulfill all normal parameters before any refinements
    // that take arguments.  Revolt allows normal parameters *after* any
    // refinement, that are not "refinement arguments".  So a refinement
    // that takes an argument must always fulfill using "pickups".

      unspecialized_refinement: {

        REBVAL *ordered = DS_TOP;
        REBSTR *param_canon = VAL_PARAM_CANON(f->param);  // #2258

        for (; ordered != DS_AT(f->dsp_orig); --ordered) {
            if (VAL_STORED_CANON(ordered) != param_canon)
                continue;

            REBLEN offset = f->arg - FRM_ARGS_HEAD(f);
            INIT_BINDING(ordered, f->varlist);
            INIT_WORD_INDEX(ordered, offset + 1);

            if (Is_Typeset_Invisible(f->param)) {
                //
                // There's no argument, so we won't need to come back
                // for this one.  But we did need to set its index
                // so we knew it was valid (errors later if not set).
                //
                goto used_refinement;
            }

            goto skip_this_arg_for_now;
        }

        goto unused_refinement; }  // not in path, not specialized yet

      unused_refinement:  // Note: might get pushed by a later slot

        Init_Nulled(f->arg);
        SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
        goto continue_arg_loop;

      used_refinement:  // can hit this on redo, copy its argument

        if (f->special == f->arg) {
            /* type checking */
        }
        else {
            Refinify(Init_Word(f->arg, VAL_PARAM_SPELLING(f->param)));
        }
        SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
        goto continue_arg_loop;
    }

    //=//// "PURE" LOCAL: ARG /////////////////////////////////////////////=//

    // This takes care of locals, including "magic" RETURN cells that
    // need to be pre-filled.  !!! Note nuances with compositions:
    //
    // https://github.com/metaeducation/ren-c/issues/823

  fulfill_arg: ;  // semicolon needed--next statement is declaration

    Reb_Param_Class pclass = VAL_PARAM_CLASS(f->param);

    switch (pclass) {
      case REB_P_LOCAL:
        //
        // When REDOing a function frame, it is sent back up to do
        // SPECIAL_IS_ARG_SO_TYPECHECKING, and the check takes care
        // of clearing the locals, they may not be null...
        //
        if (SPECIAL_IS_ARBITRARY_SO_SPECIALIZED)
            assert(IS_NULLED(f->special) or IS_VOID(f->special));

        Init_Void(f->arg);
        SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
        goto continue_arg_loop;

      default:
        break;
    }

    if (GET_CELL_FLAG(f->special, ARG_MARKED_CHECKED)) {

    //=//// SPECIALIZED OR OTHERWISE TYPECHECKED ARG //////////////////////=//

        if (not SPECIAL_IS_ARG_SO_TYPECHECKING) {
            assert(SPECIAL_IS_ARBITRARY_SO_SPECIALIZED);

            // Specializing with VARARGS! is generally not a good
            // idea unless that is an empty varargs...because each
            // call will consume from it.  Specializations you use
            // only once might make sense (?)
            //
            assert(
                not Is_Param_Variadic(f->param)
                or IS_VARARGS(f->special)
            );

            Move_Value(f->arg, f->special);  // won't copy the bit
            SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
        }

        if (
            TYPE_CHECK(f->param, REB_TS_DEQUOTE_REQUOTE)
            and IS_QUOTED(f->arg)
            and NOT_EVAL_FLAG(f, FULFILL_ONLY)
        ){
            f->requotes += VAL_NUM_QUOTES(f->arg);
            Dequotify(f->arg);
        }

        // The flag's whole purpose is that it's not set if the type
        // is invalid (excluding the narrow purpose of slipping types
        // used for partial specialization into refinement slots).
        // But this isn't a refinement slot.  Double check it's true.
        //
        // Note SPECIALIZE checks types at specialization time, to
        // save us the time of doing it on each call.  Also note that
        // NULL is not technically in the valid argument types for
        // refinement arguments, but is legal in fulfilled frames.
        //
        assert(Typecheck_Including_Quoteds(f->param, f->arg));

        goto continue_arg_loop;
    }

    // !!! This is currently a hack for APPLY.  It doesn't do a type
    // checking pass after filling the frame, but it still wants to
    // treat all values (nulls included) as fully specialized.
    //
    if (
        SPECIAL_IS_ARG_SO_TYPECHECKING  // !!! ever allow gathering?
        /* GET_EVAL_FLAG(f, FULLY_SPECIALIZED) */
    ){
        if (Is_Param_Variadic(f->param))
            Finalize_Variadic_Arg(f);
        else
            Finalize_Arg(f);
        goto continue_arg_loop;  // looping to verify args/refines
    }

    //=//// HANDLE IF NEXT ARG IS IN OUT SLOT (e.g. ENFIX, CHAIN) /////////=//

    if (GET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT)) {
        CLEAR_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT);

        if (GET_CELL_FLAG(f->out, OUT_MARKED_STALE)) {
            //
            // Something like `lib/help left-lit` is allowed to work,
            // but if it were just `obj/int-value left-lit` then the
            // path evaluation won...but LEFT-LIT still gets run.
            // It appears it has nothing to its left, but since we
            // remembered what happened we can give an informative
            // error message vs. a perplexing one.
            //
            if (GET_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH))
                fail (Error_Literal_Left_Path_Raw());

            // Seeing an END in the output slot could mean that there
            // was really "nothing" to the left, or it could be a
            // consequence of a frame being in an argument gathering
            // mode, e.g. the `+` here will perceive "nothing":
            //
            //     if + 2 [...]
            //
            // If an enfixed function finds it has a variadic in its
            // first slot, then nothing available on the left is o.k.
            // It means we have to put a VARARGS! in that argument
            // slot which will react with TRUE to TAIL?, so feed it
            // from the global empty array.
            //
            if (Is_Param_Variadic(f->param)) {
                RESET_CELL(f->arg, REB_VARARGS, CELL_MASK_VARARGS);
                INIT_BINDING(f->arg, EMPTY_ARRAY);  // feed finished

                Finalize_Enfix_Variadic_Arg(f);
                goto continue_arg_loop;
            }

            // The OUT_MARKED_STALE flag is also used by BAR! to keep
            // a result in f->out, so that the barrier doesn't destroy
            // data in cases like `(1 + 2 | comment "hi")` => 3, but
            // left enfix should treat that just like an end.

            SET_END(f->arg);
            Finalize_Arg(f);
            goto continue_arg_loop;
        }

        if (Is_Param_Variadic(f->param)) {
            //
            // Stow unevaluated cell into an array-form variadic, so
            // the user can do 0 or 1 TAKEs of it.
            //
            // !!! It be evaluated when they TAKE (it if it's an
            // evaluative arg), but not if they don't.  Should failing
            // to TAKE be seen as an error?  Failing to take first
            // gives out-of-order evaluation.
            //
            ASSERT_NOT_END(f->out);
            REBARR *array1;
            if (IS_END(f->out))
                array1 = EMPTY_ARRAY;
            else {
                REBARR *feed = Alloc_Singular(NODE_FLAG_MANAGED);
                Move_Value(ARR_SINGLE(feed), f->out);

                array1 = Alloc_Singular(NODE_FLAG_MANAGED);
                Init_Block(ARR_SINGLE(array1), feed);  // index 0
            }

            RESET_CELL(f->arg, REB_VARARGS, CELL_MASK_VARARGS);
            INIT_BINDING(f->arg, array1);
            Finalize_Enfix_Variadic_Arg(f);
        }
        else switch (pclass) {
          case REB_P_NORMAL:
          enfix_normal_handling:

            Move_Value(f->arg, f->out);
            if (GET_CELL_FLAG(f->out, UNEVALUATED))
                SET_CELL_FLAG(f->arg, UNEVALUATED);

            // When we see `1 + 2 * 3`, when we're at the 2, we don't
            // want to let the * run yet.  So set a flag which says we
            // won't do lookahead that will be cleared when function
            // takes an argument *or* when a new expression starts.
            //
            // This flag is only set for evaluative left enfix.  What
            // it does is puts the enfix into a *single step defer*.
            //
            if (GET_EVAL_FLAG(f, RUNNING_ENFIX)) {
                assert(NOT_FEED_FLAG(f->feed, NO_LOOKAHEAD));
                if (
                    NOT_ACTION_FLAG(FRM_PHASE(f), POSTPONES_ENTIRELY)
                    and
                    NOT_ACTION_FLAG(FRM_PHASE(f), DEFERS_LOOKBACK)
                ){
                    SET_FEED_FLAG(f->feed, NO_LOOKAHEAD);
                }
            }
            Finalize_Arg(f);
            break;

          case REB_P_HARD_QUOTE:
            if (not GET_CELL_FLAG(f->out, UNEVALUATED)) {
                //
                // This can happen e.g. with `x: 10 | x -> lit`.  We
                // raise an error in this case, while still allowing
                // `10 -> lit` to work, so people don't have to go
                // out of their way rethinking operators if it could
                // just work out for inert types.
                //
                fail (Error_Evaluative_Quote_Raw());
            }

            // Is_Param_Skippable() accounted for in pre-lookback

            Move_Value(f->arg, f->out);
            SET_CELL_FLAG(f->arg, UNEVALUATED);
            Finalize_Arg(f);
            break;

          case REB_P_MODAL: {
            if (not GET_CELL_FLAG(f->out, UNEVALUATED))
                goto enfix_normal_handling;

          handle_modal_in_out: ;  // declaration, semicolon necessary

            enum Reb_Kind new_kind = REB_0;
            switch (VAL_TYPE(f->out)) {
              case REB_SYM_WORD:
                new_kind = REB_GET_WORD;  // so @append doesn't run it
                break;

              case REB_SYM_PATH:
                new_kind = REB_GET_PATH;  // @append/only won't, too
                break;

              case REB_SYM_GROUP:
                new_kind = REB_GROUP;
                break;

              case REB_SYM_BLOCK:
                new_kind = REB_BLOCK;
                break;

              default:
                break;
            }

            if (new_kind != REB_0) {  // The mode is on
                //
                // !!! We could (should?) pre-check the paramlists to
                // make sure users don't try and make a modal argument
                // not followed by a refinement.  That would cost
                // extra, but avoid the test on every call.
                //
                const RELVAL *enable = f->param + 1;
                if (
                    IS_END(enable)
                    or not TYPE_CHECK(enable, REB_TS_REFINEMENT)
                ){
                    fail ("Refinement must follow modal parameter");
                }
                if (not Is_Typeset_Invisible(enable))
                    fail ("Modal refinement cannot take arguments");

                // Signal refinement as being in use.
                //
                Init_Sym_Word(DS_PUSH(), VAL_PARAM_CANON(enable));

                // Update the datatype to the non-@ form for eval
                //
                mutable_KIND_BYTE(f->out)
                    = mutable_MIRROR_BYTE(f->out)
                    = new_kind;
            }

            // Because the possibility of needing to see the uneval'd
            // value existed, the parameter had to act quoted.  Eval.
            //
            if (Eval_Value_Throws(f->arg, f->out, SPECIFIED)) {
                Move_Value(f->arg, f->out);
                goto abort_action;
            }

            Finalize_Arg(f);
            break; }

          case REB_P_SOFT_QUOTE:
            //
            // Note: This permits f->out to not carry the UNEVALUATED
            // flag--enfixed operations which have evaluations on
            // their left are treated as if they were in a GROUP!.
            // This is important to `1 + 2 <- lib/* 3` being 9, while
            // also allowing `1 + x: <- lib/default [...]` to work.

            if (IS_QUOTABLY_SOFT(f->out)) {
                if (Eval_Value_Throws(f->arg, f->out, SPECIFIED)) {
                    Move_Value(f->out, f->arg);
                    goto abort_action;
                }
            }
            else {
                Move_Value(f->arg, f->out);
                SET_CELL_FLAG(f->arg, UNEVALUATED);
            }
            Finalize_Arg(f);
            break;

          default:
            assert(false);
        }

        Expire_Out_Cell_Unless_Invisible(f);

        goto continue_arg_loop;
    }

    //=//// NON-ENFIX VARIADIC ARG (doesn't consume anything *yet*) ///////=//

    // Evaluation argument "hook" parameters (marked in MAKE ACTION!
    // by a `[[]]` in the spec, and in FUNC by `<...>`).  They point
    // back to this call through a reified FRAME!, and are able to
    // consume additional arguments during the function run.
    //
    if (Is_Param_Variadic(f->param)) {
        RESET_CELL(f->arg, REB_VARARGS, CELL_MASK_VARARGS);
        INIT_BINDING(f->arg, f->varlist);  // frame-based VARARGS!

        Finalize_Variadic_Arg(f);
        goto continue_arg_loop;
    }

    //=//// AFTER THIS, PARAMS CONSUME FROM CALLSITE IF NOT APPLY /////////=//

    // If this is a non-enfix action, we're at least at *second* slot:
    //
    //     1 + non-enfix-action <we-are-here> * 3
    //
    // That's enough to indicate we're not going to read this as
    // `(1 + non-enfix-action <we-are-here>) * 3`.  Contrast with the
    // zero-arity case:
    //
    //     >> two: does [2]
    //     >> 1 + two * 3
    //     == 9
    //
    // We don't get here to clear the flag, so it's `(1 + two) * 3`
    //
    // But if it's enfix, arg gathering could still be like:
    //
    //      1 + <we-are-here> * 3
    //
    // So it has to wait until -after- the callsite gather happens to
    // be assured it can delete the flag, to ensure that:
    //
    //      >> 1 + 2 * 3
    //      == 9
    //
    if (NOT_EVAL_FLAG(f, RUNNING_ENFIX))
        CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);

    // Once a deferred flag is set, it must be cleared during the
    // evaluation of the argument it was set for... OR the function
    // call has to end.  If we need to gather an argument when that
    // is happening, it means neither of those things are true, e.g.:
    //
    //     if 1 then [<bad>] [print "this is illegal"]
    //     if (1 then [<good>]) [print "but you can do this"]
    //
    // The situation also arises in multiple arity infix:
    //
    //     arity-3-op: func [a b c] [...]
    //
    //     1 arity-3-op 2 + 3 <ambiguous>
    //     1 arity-3-op (2 + 3) <unambiguous>
    //
    if (GET_FEED_FLAG(f->feed, DEFERRING_ENFIX))
        fail (Error_Ambiguous_Infix_Raw());

    //=//// ERROR ON END MARKER, BAR! IF APPLICABLE ///////////////////////=//

    if (IS_END(f_next) or GET_FEED_FLAG(f->feed, BARRIER_HIT)) {
        if (not Is_Param_Endable(f->param))
            fail (Error_No_Arg(f, f->param));

        Init_Endish_Nulled(f->arg);
        SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
        goto continue_arg_loop;
    }

    switch (pclass) {

    //=//// REGULAR ARG-OR-REFINEMENT-ARG (consumes 1 EVALUATE's worth) ////=//

      case REB_P_NORMAL:
      normal_handling: {
        REBFLGS flags = EVAL_MASK_DEFAULT
            | EVAL_FLAG_FULFILLING_ARG;

        if (IS_VOID(f_next))  // Eval_Step() has callers test this
            fail (Error_Void_Evaluation_Raw());  // must be quoted

        // If eval not hooked, ANY-INERT! may not need a frame
        //
        if (Did_Init_Inert_Optimize_Complete(f->arg, f->feed, &flags))
            break;

        // Can't SET_END() here, because sometimes it would be
        // overwriting what the optimization produced.  Trust that it
        // has already done it if it was necessary.

        flags |= EVAL_FLAG_CONTINUATION;
        DECLARE_FRAME (subframe, f->feed, flags);

        Push_Frame(f->arg, subframe);
        goto evaluate_arg; }  // R_CONTINUATION

    //=//// HARD QUOTED ARG-OR-REFINEMENT-ARG /////////////////////////////=//

      case REB_P_HARD_QUOTE:
        if (not Is_Param_Skippable(f->param))
            Literal_Next_In_Frame(f->arg, f);  // CELL_FLAG_UNEVALUATED
        else {
            if (not Typecheck_Including_Quoteds(f->param, f_next)) {
                assert(Is_Param_Endable(f->param));
                Init_Endish_Nulled(f->arg);  // !EVAL_FLAG_BARRIER_HIT
                SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
                goto continue_arg_loop;
            }
            Literal_Next_In_Frame(f->arg, f);
            SET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED);
            SET_CELL_FLAG(f->arg, UNEVALUATED);
        }

        // Have to account for enfix deferrals in cases like:
        //
        //     return lit 1 then (x => [x + 1])
        //
        Lookahead_To_Sync_Enfix_Defer_Flag(f->feed);

        if (GET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED))
            goto continue_arg_loop;

        break;

//=//// MODAL ARG  ////////////////////////////////////////////////////=//

      case REB_P_MODAL: {
        if (not ANY_SYM_KIND(VAL_TYPE(f_next)))  // not an @xxx
            goto normal_handling;  // acquire as a regular argument

        Literal_Next_In_Frame(f->out, f);  // f->value is read-only...
        goto handle_modal_in_out; }  // ...in out so we can Unsymify()

    //=//// SOFT QUOTED ARG-OR-REFINEMENT-ARG  ////////////////////////////=//

    // Quotes from the right already "win" over quotes from the left, in
    // a case like `help left-quoter` where they point at teach other.
    // But there's also an issue where something sits between quoting
    // constructs like the `[x]` in between the `else` and `=>`:
    //
    //     if condition [...] else [x] => [...]
    //
    // Here the neutral [x] is meant to be a left argument to the lambda,
    // producing the effect of:
    //
    //     if condition [...] else ([x] => [...])
    //
    // To get this effect, we need a different kind of deferment that
    // hops over a unit of material.  Soft quoting is unique in that it
    // means we can do that hop over exactly one unit without breaking
    // the evaluator mechanics of feeding one element at a time with
    // "no takebacks".
    //
    // First, we cache the quoted argument into the frame slot.  This is
    // the common case of what is desired.  But if we advance the feed and
    // notice a quoting enfix construct afterward looking left, we call
    // into a nested evaluator before finishing the operation.

      case REB_P_SOFT_QUOTE:
        Literal_Next_In_Frame(f->arg, f);  // CELL_FLAG_UNEVALUATED

        // See remarks on Lookahead_To_Sync_Enfix_Defer_Flag().  We
        // have to account for enfix deferrals in cases like:
        //
        //     return if false '[foo] else '[bar]
        if (
            Lookahead_To_Sync_Enfix_Defer_Flag(f->feed) and
            GET_ACTION_FLAG(VAL_ACTION(f->feed->gotten), QUOTES_FIRST)
        ){
            // We need to defer and let the right hand quote that is
            // quoting leftward win.  We use the EVAL_FLAG_POST_SWITCH
            // flag to jump into a subframe where subframe->out is
            // the f->arg, and it knows to get the arg from there.

            REBFLGS flags = EVAL_MASK_DEFAULT
                | EVAL_FLAG_FULFILLING_ARG
                | EVAL_FLAG_POST_SWITCH
                | EVAL_FLAG_INERT_OPTIMIZATION;

            if (IS_VOID(f_next))  // Eval_Step() has callers test this
                fail (Error_Void_Evaluation_Raw());  // must be quoted

            DECLARE_FRAME (subframe, f->feed, flags);

            Push_Frame(f->arg, subframe);
            bool threw = Eval_Throws(subframe);
            Drop_Frame(subframe);

            if (threw) {
                Move_Value(f->out, f->arg);
                goto abort_action;
            }
        }
        else if (IS_QUOTABLY_SOFT(f->arg)) {
            //
            // We did not defer the quoted argument.  If the argument
            // is something like a GROUP!, GET-WORD!, or GET-PATH!...
            // it has to be evaluated.
            //
            // !!! Must be converted to a continuation...

            Move_Value(f_spare, f->arg);
            if (Eval_Value_Throws(f->arg, f_spare, f_specifier)) {
                Move_Value(f->out, f->arg);
                goto abort_action;
            }
        }
        break;

      default:
        assert(false);
    }

    goto finalize_arg;

  finalize_arg:
    return R_INVISIBLE;

  continue_arg_loop:
    return f->out;

  abort_action:
    return R_THROWN;

  skip_this_arg_for_now:
    return R_IMMEDIATE;

  evaluate_arg:
    return R_CONTINUATION;
}


//
//  Eval_Action: C
//
// !!! Attempt to break out the evaluation of actions in a continuation
// sort of fashion.
//
REB_R Eval_Action(REBFRM *f)
{
    if (Is_Throwing(f)) {
        if (GET_EVAL_FLAG(f, DISPATCHER_CATCHES))
            goto redo_continuation;  // might want to see BREAK/CONTINUE
        goto action_threw;  // could be an UNWIND or similar
    }
    if (GET_EVAL_FLAG(f, ARG_FINISHED)) {
        CLEAR_EVAL_FLAG(f, ARG_FINISHED);
        goto finalize_arg;
    }
    if (GET_EVAL_FLAG(f, ACTION_FOLLOWUP)) {
        CLEAR_EVAL_FLAG(f, ACTION_FOLLOWUP);
        goto redo_continuation;
    }

    if (GET_EVAL_FLAG(f, DELEGATE_CONTROL)) {
        CLEAR_EVAL_FLAG(f, DELEGATE_CONTROL);
        goto dispatch_completed;
    }

  process_action:

  #if !defined(NDEBUG)
    assert(f->original);  // set by Begin_Action()
    Do_Process_Action_Checks_Debug(f);
  #endif

    assert(DSP >= f->dsp_orig);  // path processing may push REFINEMENT!s

    assert(NOT_EVAL_FLAG(f, DOING_PICKUPS));

    //=//// ACTION! ARGUMENT FULFILLMENT AND/OR TYPE CHECKING PROCESS /////=//

    // This one processing loop is able to handle ordinary action invocation,
    // specialization, and type checking of an already filled action frame.
    // It walks through both the formal parameters (in the spec) and the
    // actual arguments (in the call frame) using pointer incrementation.
    //
    // Based on the parameter type, it might need to "consume" an expression
    // from values that come after the invocation point.  But not all
    // parameters will consume arguments for all calls.

    for (; NOT_END(f->param); ++f->param, ++f->arg, ++f->special) {

    //=//// CONTINUES (AT TOP SO GOTOS DO NOT CROSS INITIALIZATIONS ///////=//

        goto loop_body;  // optimized out

      finalize_arg:

        // If FEED_FLAG_NO_LOOKAHEAD was set going into the argument
        // gathering above, it should have been cleared or converted into
        // FEED_FLAG_DEFER_ENFIX.
        //
        //     1 + 2 * 3
        //           ^-- this deferred its chance, so 1 + 2 will complete
        //
        assert(NOT_FEED_FLAG(f->feed, NO_LOOKAHEAD));

    //=//// TYPE CHECKING FOR (MOST) ARGS AT END OF ARG LOOP //////////////=//

        // Some arguments can be fulfilled and skip type checking or
        // take care of it themselves.  But normal args pass through
        // this code which checks the typeset and also handles it when
        // a void arg signals the revocation of a refinement usage.

        assert(
            not SPECIAL_IS_ARG_SO_TYPECHECKING  // was handled, unless...
            or NOT_EVAL_FLAG(f, FULLY_SPECIALIZED)  // ...this!
        );

        Finalize_Arg(f);

      continue_arg_loop:

        assert(GET_CELL_FLAG(f->arg, ARG_MARKED_CHECKED));

        if (GET_EVAL_FLAG(f, DOING_PICKUPS)) {
            if (DSP != f->dsp_orig)
                goto next_pickup;

            f->param = END_NODE;  // don't need f->param in paramlist
            goto arg_loop_and_any_pickups_done;
        }
        continue;

      skip_this_arg_for_now:  // the GC marks args up through f->arg...

        Init_Unreadable_Blank(f->arg);  // ...so cell must have valid bits
        continue;

        goto finalize_arg;  // have to put outside pclass initialization

    //=//// ACTUAL LOOP BODY //////////////////////////////////////////////=//

      loop_body: blockscope {
        REB_R r = Eval_Arg(f);
        if (r == f->out)  // finished this arg
            goto continue_arg_loop;
        if (r == R_THROWN)
            goto abort_action;
        if (r == R_IMMEDIATE)
            goto skip_this_arg_for_now;
        if (r == R_INVISIBLE)
            goto finalize_arg;
        assert(r == R_CONTINUATION);
        return R_CONTINUATION;
      }
    }

    assert(IS_END(f->arg));  // arg can otherwise point to any arg cell

    // There may have been refinements that were skipped because the
    // order of definition did not match the order of usage.  They were
    // left on the stack with a pointer to the `param` and `arg` after
    // them for later fulfillment.
    //
    // Note that there may be functions on the stack if this is the
    // second time through, and we were just jumping up to check the
    // parameters in response to a R_REDO_CHECKED; if so, skip this.
    //
    if (DSP != f->dsp_orig and IS_SYM_WORD(DS_TOP)) {

      next_pickup:

        assert(IS_SYM_WORD(DS_TOP));

        if (not IS_WORD_BOUND(DS_TOP)) {  // the loop didn't index it
            mutable_KIND_BYTE(DS_TOP) = REB_WORD;
            mutable_MIRROR_BYTE(DS_TOP) = REB_WORD;
            fail (Error_Bad_Refine_Raw(DS_TOP));  // so duplicate or junk
        }

        // FRM_ARGS_HEAD offsets are 0-based, while index is 1-based.
        // But +1 is okay, because we want the slots after the refinement.
        //
        REBINT offset =
            VAL_WORD_INDEX(DS_TOP) - (f->arg - FRM_ARGS_HEAD(f)) - 1;
        f->param += offset;
        f->arg += offset;
        f->special += offset;

        assert(VAL_STORED_CANON(DS_TOP) == VAL_PARAM_CANON(f->param));
        assert(TYPE_CHECK(f->param, REB_TS_REFINEMENT));
        DS_DROP();

        if (Is_Typeset_Invisible(f->param)) {  // no callsite arg, drop
            if (DSP != f->dsp_orig)
                goto next_pickup;

            f->param = END_NODE;  // don't need f->param in paramlist
            goto arg_loop_and_any_pickups_done;
        }

        assert(IS_UNREADABLE_DEBUG(f->arg) or IS_NULLED(f->arg));
        SET_EVAL_FLAG(f, DOING_PICKUPS);

      blockscope {
        REB_R r = Eval_Arg(f);
        if (r == f->out)  // finished this arg
            goto continue_arg_loop;
        if (r == R_THROWN)
            goto abort_action;
        if (r == R_IMMEDIATE) {
            assert(!"Shouldn't skip a pickup, right?");
            goto skip_this_arg_for_now;
        }
        if (r == R_INVISIBLE)
            goto finalize_arg;
        assert(r == R_CONTINUATION);
        return R_CONTINUATION;
      }
    }

  arg_loop_and_any_pickups_done:

    CLEAR_EVAL_FLAG(f, DOING_PICKUPS);  // reevaluate may set flag again
    assert(IS_END(f->param));  // signals !Is_Action_Frame_Fulfilling()

//==////////////////////////////////////////////////////////////////==//
//
// ACTION! ARGUMENTS NOW GATHERED, DISPATCH PHASE
//
//==////////////////////////////////////////////////////////////////==//

    if (GET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT)) {
        if (GET_EVAL_FLAG(f, DIDNT_LEFT_QUOTE_PATH))
            fail (Error_Literal_Left_Path_Raw());
    }

  redo_unchecked:

    // This happens if you have something intending to act as enfix but
    // that does not consume arguments, e.g. `x: enfixed func [] []`.
    // An enfixed function with no arguments might sound dumb, but that
    // can be useful as a convenience way of saying "takes the left hand
    // argument but ignores it" (e.g. with skippable args).  Allow it.
    //
    if (GET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT)) {
        assert(GET_EVAL_FLAG(f, RUNNING_ENFIX));
        CLEAR_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT);
    }

    assert(IS_END(f->param));
    assert(
        IS_END(f_next)
        or FRM_IS_VALIST(f)
        or IS_VALUE_IN_ARRAY_DEBUG(f->feed->array, f_next)
    );

    if (GET_EVAL_FLAG(f, FULFILL_ONLY)) {
        Init_Nulled(f->out);
        goto skip_output_check;
    }

    Expire_Out_Cell_Unless_Invisible(f);

    // The spare cell is used during reevaluation and argument gathering,
    // including being a place to help cheat the "no going back" of
    // lookback when necessary (enforced by va_list in C, even though a
    // Rebol block can be looked back into).  But once the frame is built
    // it's available for dispatcher or native use.  Initially there was
    // no guarantee what would be in the spare cell, but for the cost of
    // setting it to END (one byte, cheap!) we have a testable state for
    // if it's the first run of a continuation.  This can be useful for
    // functions that don't want to pay for an extra local cell.
    //
    // !!! Should f->out be initialized to END when non-invisibles are
    // called (since they are committing to output) for a similar "cheap!"
    // means of tracking extra state information?
    //
    SET_END(f_spare);

    // While you can't evaluate into an array cell (because it may move)
    // an evaluation is allowed to be performed into stable cells on the
    // stack -or- API handles.
    //
    // !!! Could get complicated if a manual lifetime is used and freed
    // during an evaluation.  Not currently possible since there's nothing
    // like a rebValue() which targets a cell passed in by the user.  But
    // if such a thing ever existed it would have that problem...and would
    // need to take a "hold" on the cell to prevent a rebFree() while the
    // evaluation was in progress.
    //
    assert(f->out->header.bits & (
        CELL_FLAG_STACK_LIFETIME
        | NODE_FLAG_ROOT
    ));

    f_next_gotten = nullptr;  // arbitrary code changes fetched variables

    // Note that the dispatcher may push ACTION! values to the data stack
    // which are used to process the return result after the switch.
    //
  redo_continuation:
  blockscope {
    //
    // These flags are optionally set by the dispatcher for use with
    // REB_R_CONTINUATION.  They are EVAL_FLAGs instead of part of the
    // REB_R signal because they apply after the `r` has been processed
    // and forgotten, and the continuation is resuming.  They are cleared
    // by Drop_Action() but must be cleared on each dispatcher re-entry.
    //
    f->flags.bits &= ~(
        EVAL_FLAG_DELEGATE_CONTROL
        | EVAL_FLAG_DISPATCHER_CATCHES
    );

    const REBVAL *r = (*PG_Dispatch)(f);  // default just calls FRM_PHASE

    if (r == f->out) {  // assume most common branch for speed
        assert(NOT_CELL_FLAG(f->out, OUT_MARKED_STALE));
        CLEAR_CELL_FLAG(f->out, UNEVALUATED);  // others Move_Value()
    }
    else if (not r) {  // API and internal code can both return `nullptr`
        Init_Nulled(f->out);
    }
    else if (GET_CELL_FLAG(r, ROOT)) {  // API, from Alloc_Value()
        Handle_Api_Dispatcher_Result(f, r);
    }
    else switch (KIND_BYTE(r)) {  // it's a "pseudotype" instruction
        //
        // !!! As the workings of stackless continue to sift out, the general
        // idea seems to be that all continuations are based on building a
        // frame as the currency for the continuation and linking it in...
        // then getting an optional callback.  Initial concepts were that
        // this would be memoized in some way that the dispatcher could help
        // you with, but that help should be done another way.
        //
      case REB_R_CONTINUATION:
        assert(FS_TOP != f);  // should have pushed a frame
        assert(GET_EVAL_FLAG(FS_TOP, CONTINUATION));
        return R_CONTINUATION;

        // !!! Thrown values used to be indicated with a bit on the value
        // itself, but now it's conveyed through a return value.  This
        // means typical return values don't have to run through a test
        // for if they're thrown or not, but it means Eval_Core has to
        // return a boolean to pass up the state.  It may not be much of
        // a performance win either way, but recovering the bit in the
        // values is a definite advantage--as header bits are scarce!
        //
      case REB_R_THROWN:
        goto action_threw;

      case REB_R_DEWIND:  // accept that `f` was changed
        f = FS_TOP;

        // !!! As written, a DEWIND operation must unwind to an action
        // frame.  A reason for this is the action frame may be tied to
        // a feed that has only one instance...jumping above it to try
        // and "continue" would drop off instructions that were pending
        // to be processed in the frame.  This may suggest a better
        // architecture is needed, because it means you can't keep
        // a yielder's frame itself alive... it terminates so the YIELD
        // instruction must be updated with the new frame each time.
        //
        assert(Is_Action_Frame(f));
        break;

      case REB_R_REDO:
        //
        // This instruction represents the idea that it is desired to
        // run the f->phase again.  The dispatcher may have changed the
        // value of what f->phase is, for instance.

        if (not EXTRA(Any, r).flag)  // R_REDO_UNCHECKED
            goto redo_unchecked;

        goto redo_checked;

      case REB_R_INVISIBLE: {
        assert(GET_ACTION_FLAG(FRM_PHASE(f), IS_INVISIBLE));

        if (NOT_SERIES_INFO(f->varlist, TELEGRAPH_NO_LOOKAHEAD))
            CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
        else {
            SET_FEED_FLAG(f->feed, NO_LOOKAHEAD);
            CLEAR_SERIES_INFO(f->varlist, TELEGRAPH_NO_LOOKAHEAD);
        }

        // !!! Ideally we would check that f->out hadn't changed, but
        // that would require saving the old value somewhere...
        //
        // !!! Why is this test a NOT?

        if (NOT_CELL_FLAG(f->out, OUT_MARKED_STALE) or IS_END(f_next))
            goto skip_output_check;

        // If an invisible is at the start of a frame and nothing is
        // after it, it has to retrigger until it finds something (or
        // until it hits the end of the frame).  It should not do a
        // START_NEW_EXPRESSION()...the expression index doesn't update.
        //
        //     do [comment "a" 1] => 1
        //
        // !!! This was actually something more like a R_REEVALUATE.  It
        // should be rethought in light of a debugger that can single step
        // across invisibles (?)
        //
        Drop_Action(f);
        return R_CONTINUATION; }  // !!! e.g. R_REEVALUATE

      default:
        assert(!"Invalid pseudotype returned from action dispatcher");
    }
  }

  dispatch_completed:

    //==////////////////////////////////////////////////////////////////==//
    //
    // ACTION! CALL COMPLETION
    //
    //==////////////////////////////////////////////////////////////////==//

    // Here we know the function finished and nothing threw past it or
    // FAIL / fail()'d.  It should still be in REB_ACTION evaluation
    // type, and overwritten the f->out with a non-thrown value.  If the
    // function composition is a CHAIN, the chained functions are still
    // pending on the stack to be run.

  #if !defined(NDEBUG)
    Do_After_Action_Checks_Debug(f);
  #endif

  skip_output_check:

    // If we have functions pending to run on the outputs (e.g. this was
    // the result of a CHAIN) we can run those chained functions in the
    // same REBFRM, for efficiency.
    //
    while (DSP != f->dsp_orig) {
        //
        // We want to keep the label that the function was invoked with,
        // because the other phases in the chain are implementation
        // details...and if there's an error, it should still show the
        // name the user invoked the function with.  But we have to drop
        // the action args, as the paramlist is likely be completely
        // incompatible with this next chain step.
        //
        REBSTR *opt_label = f->opt_label;
        Drop_Action(f);
        Push_Action(f, VAL_ACTION(DS_TOP), VAL_BINDING(DS_TOP));
        DS_DROP();

        // We use the same mechanism as enfix operations do...give the
        // next chain step its first argument coming from f->out
        //
        // !!! One side effect of this is that unless CHAIN is changed
        // to check, your chains can consume more than one argument.
        // This might be interesting or it might be bugs waiting to
        // happen, trying it out of curiosity for now.
        //
        Begin_Prefix_Action(f, opt_label);
        assert(NOT_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT));
        SET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT);

        goto process_action;
    }

    // We assume that null return results don't count for the requoting,
    // unless the dequoting was explicitly of a quoted null parameter.
    // Just a heuristic--if it doesn't work for someone, they'll have to
    // take QUOTED! themselves and do whatever specific logic they need.
    //
    if (GET_ACTION_FLAG(f->original, RETURN_REQUOTES)) {
        if (
            KIND_BYTE_UNCHECKED(f->out) != REB_NULLED
            or GET_EVAL_FLAG(f, REQUOTE_NULL)
        ){
            Quotify(f->out, f->requotes);
        }
    }

    return nullptr;


  action_threw:
  blockscope {
    const REBVAL *label = VAL_THROWN_LABEL(f->out);
    if (IS_ACTION(label)) {
        if (
            VAL_ACTION(label) == NAT_ACTION(unwind)
            and VAL_BINDING(label) == NOD(f->varlist)
        ){
            // Eval_Core catches unwinds to the current frame, so throws
            // where the "/name" is the UNWIND native with a binding to
            // this frame, and the thrown value is the return code.
            //
            // !!! This might be a little more natural if the name of
            // the throw was a FRAME! value.  But that also would mean
            // throws named by frames couldn't be taken advantage by
            // the user for other features, while this only takes one
            // function away.
            //
            CATCH_THROWN(f->out, f->out);
            goto dispatch_completed;
        }
        else if (
            VAL_ACTION(label) == NAT_ACTION(redo)
            and VAL_BINDING(label) == NOD(f->varlist)
        ){
            // This was issued by REDO, and should be a FRAME! with
            // the phase and binding we are to resume with.
            //
            CATCH_THROWN(f->out, f->out);
            assert(IS_FRAME(f->out));

            // !!! We are reusing the frame and may be jumping to an
            // "earlier phase" of a composite function, or even to
            // a "not-even-earlier-just-compatible" phase of another
            // function.  Type checking is necessary, as is zeroing
            // out any locals...but if we're jumping to any higher
            // or different phase we need to reset the specialization
            // values as well.
            //
            // Since dispatchers run arbitrary code to pick how (and
            // if) they want to change the phase on each redo, we
            // have no easy way to tell if a phase is "earlier" or
            // "later".  The only thing we have is if it's the same
            // we know we couldn't have touched the specialized args
            // (no binding to them) so no need to fill those slots
            // in via the exemplar.  Otherwise, we have to use the
            // exemplar of the phase.
            //
            // REDO is a fairly esoteric feature to start with, and
            // REDO of a frame phase that isn't the running one even
            // more esoteric, with REDO/OTHER being *extremely*
            // esoteric.  So having a fourth state of how to handle
            // f->special (in addition to the three described above)
            // seems like more branching in the baseline argument
            // loop.  Hence, do a pre-pass here to fill in just the
            // specializations and leave everything else alone.
            //
            REBCTX *exemplar;
            if (
                FRM_PHASE(f) != VAL_PHASE(f->out)
                and did (exemplar = ACT_EXEMPLAR(VAL_PHASE(f->out)))
            ){
                f->special = CTX_VARS_HEAD(exemplar);
                f->arg = FRM_ARGS_HEAD(f);
                for (; NOT_END(f->arg); ++f->arg, ++f->special) {
                    if (IS_NULLED(f->special))  // no specialization
                        continue;
                    Move_Value(f->arg, f->special);  // reset it
                }
            }

            INIT_FRM_PHASE(f, VAL_PHASE(f->out));
            FRM_BINDING(f) = VAL_BINDING(f->out);

            goto redo_checked;
        }
    }
  }

  abort_action:

    Drop_Action(f);
    DS_DROP_TO(f->dsp_orig);  // any unprocessed refinements / chains on stack

    return R_THROWN;

  redo_checked:  // R_REDO_CHECKED

    Expire_Out_Cell_Unless_Invisible(f);

    f->param = ACT_PARAMS_HEAD(FRM_PHASE(f));
    f->arg = FRM_ARGS_HEAD(f);
    f->special = f->arg;

    return R_CONTINUATION;
}

