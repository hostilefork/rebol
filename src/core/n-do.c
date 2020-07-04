//
//  File: %n-do.c
//  Summary: "native functions for DO, EVAL, APPLY"
//  Section: natives
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
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
// Revolt's philosophy of DO is that the argument to it represents a place to
// find source code.  Hence `DO 3` does not evaluate to the number 3, any
// more than `DO "print hello"` would evaluate to `"print hello"`.  If a
// generalized evaluator is needed, use the special-purpose function EVAL.
//
// Note that although the code for running blocks and frames is implemented
// here as C, the handler for processing STRING!, FILE!, TAG!, URL!, etc. is
// dispatched out to some Revolt code.  See `system/intrinsic/do*`.
//

#include "sys-core.h"


//
//  reeval: native [
//
//  {Process an evaluated argument *inline* as the evaluator loop would}
//
//      return: [<opt> any-value!]
//      value [any-value!]
//          {BLOCK! passes-thru, ACTION! runs, SET-WORD! assigns...}
//      expressions [<opt> any-value! <...>]
//          {Depending on VALUE, more expressions may be consumed}
//  ]
//
REBNATIVE(reeval)
{
    INCLUDE_PARAMS_OF_REEVAL;

    // REEVAL only *acts* variadic, but uses continuations to do its work
    //
    UNUSED(ARG(expressions));

    // !!! Review ramifications of Enfix.  See SHOVE for how the reevaluate
    // situation was addressed in that case.

    Init_Void(D_OUT);  // `eval lit (comment "this gives void vs. error")`

    REBFLGS flags = EVAL_MASK_DEFAULT;

    DECLARE_FRAME (subframe, frame_->feed, flags);
    subframe->executor = &Reevaluation_Executor;
    subframe->u.reval.value = ARG(value);
    Push_Frame(D_OUT, subframe);

    SET_EVAL_FLAG(frame_, DELEGATE_CONTROL);
    return R_CONTINUATION;
}


//
//  shove: native [
//
//  {Shove a parameter into an ACTION! as its first argument}
//
//      return: [<opt> any-value!]
//          "REVIEW: How might this handle shoving enfix invisibles?"
//      'left [<end> <opt> any-value!]
//          "Requests parameter convention based on enfixee's first argument"
//      :right [<...> <end> any-value!]
//          "(uses magic -- SHOVE can't be written easily in usermode yet)"
//      /enfix "Follow completion rules for enfix, e.g. `1 + 2 <- * 3` is 9"
//      /set "If left hand side is a SET-WORD! or SET-PATH!, shove and assign"
//  ]
//
REBNATIVE(shove)
//
// PATH!s do not do infix lookup in Rebol, and there are good reasons for this
// in terms of both performance and semantics.  However, it is sometimes
// needed to dispatch via a path--for instance to call an enfix function that
// lives in a context, or even to call one that has refinements.
//
// The SHOVE operation is used to push values from the left to act as the
// first argument of an operation, e.g.:
//
//      >> 10 <- lib/(print "Hi!" first [multiply]) 20
//      Hi!
//      200
//
// It's becoming more possible to write something like this in usermode, but
// it would be inefficient.  This version of shove is a light variation on
// the EVAL native, which retriggers the actual enfix machinery.
{
    INCLUDE_PARAMS_OF_SHOVE;

    REBVAL *left = ARG(left);

    if (NOT_END(D_SPARE)) {  // we've finished the evaluation
        assert(NOT_CELL_FLAG(D_OUT, OUT_MARKED_STALE));  // !!! possible?

        if (REF(set)) {
            if (IS_SET_WORD(left)) {
                assert(IS_BLANK(D_SPARE));
                Move_Value(Sink_Word_May_Fail(left, SPECIFIED), D_OUT);
            }
            else if (IS_SET_PATH(left)) {
                assert(IS_SET_PATH(D_SPARE));  // the composed set path.
                frame_->feed->gotten = nullptr;  // arbitrary code, may disrupt
                rebElideQ(
                    "set/hard", D_SPARE, NULLIFY_NULLED(D_OUT),
                rebEND);
            }
            else
                assert(false); // SET-WORD!/SET-PATH! was checked above
        }
        else
            assert(IS_BLANK(D_SPARE));

        return D_OUT;
    }

    REBFRM *f;
    if (not Is_Frame_Style_Varargs_May_Fail(&f, ARG(right)))
        fail ("SHOVE (<-) not implemented for MAKE VARARGS! [...] yet");

    SHORTHAND (v, f->feed->value, NEVERNULL(const RELVAL*));
    SHORTHAND (specifier, f->feed->specifier, REBSPC*);

    if (IS_END(*v))  // ...shouldn't happen for WORD!/PATH! unless APPLY
        RETURN (ARG(left));  // ...because evaluator wants `help <-` to work

    // It's best for SHOVE to do type checking here, as opposed to setting
    // some kind of EVAL_FLAG_SHOVING and passing that into the evaluator, then
    // expecting it to notice if you shoved into an INTEGER! or something.
    //
    // !!! Pure invisibility should work; see SYNC-INVISIBLES for ideas,
    // something like this should be in the tests and be able to work:
    //
    //    >> 10 <- comment "ignore me" lib/+ 20
    //    == 30
    //
    // !!! To get the feature working as a first cut, this doesn't try get too
    // fancy with apply-like mechanics and slipstream refinements on the
    // stack to enfix functions with refinements.  It specializes the ACTION!.
    // We can do better, but seeing as how you couldn't call enfix actions
    // with refinements *at all* before, this is a step up.

    REBVAL *shovee = ARG(right); // reuse arg cell for the shoved-into

    REBSTR *opt_label = nullptr;
    if (IS_WORD(*v) or IS_PATH(*v)) {
        if (Get_If_Word_Or_Path_Throws(
            D_OUT, // can't eval directly into arg slot
            &opt_label,
            *v,
            *specifier,
            false // !!! see above; false = don't push refinements
        )){
            return R_THROWN;
        }

        Move_Value(shovee, D_OUT);
    }
    else if (IS_GROUP(*v)) {
        if (Do_Any_Array_At_Throws(D_OUT, *v, *specifier))
            return R_THROWN;
        if (IS_END(D_OUT))  // !!! need SHOVE frame for type error
            fail ("GROUP! passed to SHOVE did not evaluate to content");

        Move_Value(shovee, D_OUT);  // Note: can't eval directly into arg slot
    }
    else
        Move_Value(shovee, KNOWN(*v));

    if (not IS_ACTION(shovee) and not ANY_SET_KIND(VAL_TYPE(shovee)))
        fail ("SHOVE's immediate right must be ACTION! or SET-XXX! type");

    // Even if the function isn't enfix, say it is.  This permits things
    // like `5 + 5 -> subtract 7` to give 3.
    //
    if (REF(enfix) and IS_ACTION(shovee)) {
        //
        // This is currently explicitly passed to the Reevaluate() function
        // below.  It's so that `add 1 2 -> 3` is 7
    }
    else
        Fetch_Next_Forget_Lookback(f);  // so that `10 -> = 5 + 5` is true

    // Since we're simulating enfix dispatch, we need to move the first arg
    // where enfix gets it from...the frame output slot.
    //
    // We quoted the argument on the left, but the ACTION! we are feeding
    // into may want it evaluative.  (Enfix handling itself does soft quoting)
    //
  #if !defined(NDEBUG)
    Init_Unreadable_Blank(D_OUT); // make sure we reassign it
  #endif

    if (REF(set)) {
        //
        // Trying to EVAL a SET-WORD! or SET-PATH! with no args would error.
        // Interpret it specially...GET the value and SET it back.  Note this
        // is tricky stuff to do when a SET-PATH! has groups in it to avoid a
        // double evaluation--the API is used here for simplicity.
        //
        if (IS_SET_WORD(left)) {
            Move_Value(D_OUT, Lookup_Word_May_Fail(left, SPECIFIED));
        }
        else if (IS_SET_PATH(left)) {
            f->feed->gotten = nullptr;  // calling arbitrary code, may disrupt
            REBVAL *composed_set_path = rebValueQ("compose", left, rebEND);
            REBVAL *temp = rebValueQ("get/hard", composed_set_path, rebEND);
            Move_Value(D_OUT, temp);
            rebRelease(temp);
            Move_Value(D_SPARE, composed_set_path);
            rebRelease(composed_set_path);
        }
        else
            fail ("Left hand side must be SET-WORD! or SET-PATH!");
    }
    else if (
        GET_CELL_FLAG(left, UNEVALUATED)
        and not (
            IS_ACTION(shovee)
            and GET_ACTION_FLAG(VAL_ACTION(shovee), QUOTES_FIRST)
        )
    ){
        if (Eval_Value_Throws(D_OUT, left, SPECIFIED))
            return R_THROWN;
    }
    else {
        Move_Value(D_OUT, left);
        if (GET_CELL_FLAG(left, UNEVALUATED))
            SET_CELL_FLAG(D_OUT, UNEVALUATED);
    }

    // !!! We're going to perform a continuation here, and use the state
    // being non-end to tell us that we did so.
    //
    if (NOT_END(D_SPARE))
        assert(IS_SET_PATH(D_SPARE));  // the composed path
    else
        Init_Blank(D_SPARE);  // temporary non-end state

    // We want the D_OUT of the frame to be used as the next item in the feed.
    //
    // !!! It's still not clear if this is a feed flag or a frame flag, as
    // a feed might be used by multiple frames with different out pointers.
    // Originally it was moved off the frame due to lack of EVAL_FLAGs.
    //
    SET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT);

    if (REF(enfix)) {  // don't heed enfix state in the action itself
        DECLARE_FRAME (subframe, f->feed, EVAL_MASK_DEFAULT);
        Push_Frame(D_OUT, subframe);

        assert(NOT_EVAL_FLAG(subframe, RUNNING_ENFIX));
        Push_Action(
            subframe,
            VAL_ACTION(shovee),
            VAL_BINDING(shovee)
        );
        Begin_Enfix_Action(subframe, nullptr);  // invisible cache NO_LOOKAHEAD

        Fetch_Next_Forget_Lookback(f);
    }
    else {
        DECLARE_FRAME (subframe, f->feed, EVAL_MASK_DEFAULT);
        subframe->executor = &Reevaluation_Executor;
        subframe->u.reval.value = shovee;

        Push_Frame(D_OUT, subframe);
    }

    return R_CONTINUATION;
}


//
//  do: native [
//
//  {Evaluates a block of source code (directly or fetched according to type)}
//
//      return: [<opt> any-value!]
//      source [
//          <blank>  ; opts out of the DO, returns null
//          block!  ; source code in block form
//          sym-block!  ; same
//          group!  ; same as block (or should it have some other nuance?)
//          sym-group!  ; same
//          text!  ; source code in text form
//          binary!  ; treated as UTF-8
//          url!  ; load code from URL via protocol
//          file!  ; load code from file on local disk
//          tag!  ; module name (URL! looked up from table)
//          error!  ; should use FAIL instead
//          action!  ; will only run arity 0 actions (avoids DO variadic)
//          frame!  ; acts like APPLY (voids are optionals, not unspecialized)
//          varargs!  ; simulates as if frame! or block! is being executed
//      ]
//      /args "Sets system/script/args if doing a script (usually a TEXT!)"
//          [any-value!]
//      /only "Don't catch QUIT (default behavior for BLOCK!)"
//  ]
//
REBNATIVE(do)
{
    INCLUDE_PARAMS_OF_DO;

    REBVAL *source = ARG(source);

    // If `source` is not const, tweak it to be explicitly mutable--because
    // otherwise, it would wind up inheriting the FEED_MASK_CONST of our
    // currently executing frame.  That's no good for `loop 2 [do block]`,
    // because we want whatever constness is on block...
    //
    // (Note we *can't* tweak values that are RELVAL in source.  So we either
    // bias to having to do this or Do_XXX() versions explode into passing
    // mutability parameters all over the place.  This is better.)
    //
    if (NOT_CELL_FLAG(source, CONST))
        SET_CELL_FLAG(source, EXPLICITLY_MUTABLE);

  #if !defined(NDEBUG)
    SET_CELL_FLAG(ARG(source), PROTECTED);  // maybe only GC reference, keep!
  #endif

    switch (VAL_TYPE(source)) {
      case REB_BLOCK:
      case REB_SYM_BLOCK:
      case REB_GROUP:
      case REB_SYM_GROUP: {
        if (Do_Any_Array_At_Throws(D_OUT, source, SPECIFIED))
            return R_THROWN;
        return D_OUT; }

      case REB_VARARGS: {
        REBVAL *position;
        if (Is_Block_Style_Varargs(&position, source)) {
            //
            // We can execute the array, but we must "consume" elements out
            // of it (e.g. advance the index shared across all instances)
            //
            // !!! If any VARARGS! op does not honor the "locked" flag on the
            // array during execution, there will be problems if it is TAKE'n
            // or DO'd while this operation is in progress.
            //
            if (Do_Any_Array_At_Throws(D_OUT, position, SPECIFIED)) {
                //
                // !!! A BLOCK! varargs doesn't technically need to "go bad"
                // on a throw, since the block is still around.  But a FRAME!
                // varargs does.  This will cause an assert if reused, and
                // having BLANK! mean "thrown" may evolve into a convention.
                //
                Init_Unreadable_Blank(position);
                return R_THROWN;
            }

            SET_END(position); // convention for shared data at end point
            return D_OUT;
        }

        REBFRM *f;
        if (not Is_Frame_Style_Varargs_May_Fail(&f, source))
            panic (source); // Frame is the only other type

        // By definition, we are in the middle of a function call in the frame
        // the varargs came from.  It's still on the stack, and we don't want
        // to disrupt its state.  Use a subframe.

        Init_Void(D_OUT);
        if (IS_END(f->feed->value))
            return D_OUT;

        DECLARE_FRAME (subframe, f->feed, EVAL_MASK_DEFAULT);

        bool threw;
        Push_Frame(D_OUT, subframe);
        do {
            threw = Eval_Step_Maybe_Stale_Throws(D_OUT, subframe);
        } while (not threw and NOT_END(f->feed->value));
        Drop_Frame(subframe);

        if (threw)
            return R_THROWN;

        CLEAR_CELL_FLAG(D_OUT, OUT_MARKED_STALE);
        return D_OUT; }

      case REB_BINARY:
      case REB_TEXT:
      case REB_URL:
      case REB_FILE:
      case REB_TAG: {
        //
        // See code called in system/intrinsic/do*
        //
        REBVAL *sys_do_helper = Get_Sys_Function(DO_P);
        assert(IS_ACTION(sys_do_helper));

        UNUSED(REF(args)); // detected via `value? :arg`

        if (RunQ_Throws(
            D_OUT,
            true,  // fully = true, error if not all arguments consumed
            rebU1(sys_do_helper),
            source,
            REF(args),
            REF(only) ? TRUE_VALUE : FALSE_VALUE,
            rebEND
        )){
            return R_THROWN;
        }
        return D_OUT; }

      case REB_ERROR:
        //
        // FAIL is the preferred operation for triggering errors, as it has
        // a natural behavior for blocks passed to construct readable messages
        // and "FAIL X" more clearly communicates a failure than "DO X"
        // does.  However DO of an ERROR! would have to raise an error
        // anyway, so it might as well raise the one it is given...and this
        // allows the more complex logic of FAIL to be written in Revolt code.
        //
        fail (VAL_CONTEXT(source));

      case REB_ACTION: {
        //
        // Only run arity 0 functions from DO, otherwise REEVAL must be used.
        //
        if (First_Unspecialized_Param(VAL_ACTION(source)) != nullptr)
            fail (Error_Use_Reeval_For_Do_Raw());

        DELEGATE (source); }

      case REB_FRAME: {
        REBCTX *c = VAL_CONTEXT(source);  // checks for INACCESSIBLE
        if (CTX_FRAME_IF_ON_STACK(c))  // see REDO for tail-call recursion
            fail ("Use REDO to restart a running FRAME! (not DO)");

        DELEGATE (source); }

      default:
        break;
    }

    fail (Error_Use_Reeval_For_Do_Raw());  // https://trello.com/c/YMAb89dv
}


//
//  evaluate: native [
//
//  {Perform a single evaluator step, returning the next source position}
//
//      return: [<opt> block! group! varargs!]
//      :var "Is set with result of evaluation (left as-is if end/invisible)"
//          [<skip> sym-word! sym-path! sym-group!]
//      source [
//          <blank>  ; useful for `evaluate try ...` scenarios when no match
//          block!  ; source code in block form
//          group!  ; same as block (or should it have some other nuance?)
//          varargs!  ; simulates as if frame! or block! is being executed
//      ]
//      /set "DEPRECATED: use `evaluate @var` or `evaluate @(lit var:)` etc."
//          [any-word!]
//  ]
//
REBNATIVE(evaluate)
{
    INCLUDE_PARAMS_OF_EVALUATE;

    if (REF(set))
        fail ("EVALUATE/SET deprecated: https://forum.rebol.info/t/1173");

    REBVAL *source = ARG(source);  // may be only GC reference, don't lose it!
  #if !defined(NDEBUG)
    SET_CELL_FLAG(ARG(source), PROTECTED);
  #endif

    REBVAL *var = ARG(var);

    if (IS_SYM_GROUP(var)) {
        if (Do_Any_Array_At_Throws(D_OUT, var, SPECIFIED))
            return R_THROWN;

        if (IS_BLANK(D_OUT))
            Init_Nulled(var);  // for consistency with <skip>'d var
        if (ANY_WORD(D_OUT) or ANY_PATH(D_OUT))
            Move_Value(var, D_OUT);
        else
            fail ("@(...) for EVALUATE must be BLANK!/ANY-WORD!/ANY-PATH!");
    }

    switch (VAL_TYPE(source)) {
      case REB_BLOCK:
      case REB_GROUP: {
        REBLEN index;
        if (Eval_Step_In_Any_Array_At_Throws(
            D_SPARE,
            &index,
            source,
            SPECIFIED,
            EVAL_MASK_DEFAULT
        )){
            Move_Value(D_OUT, D_SPARE);
            return R_THROWN;
        }

        if (IS_END(D_SPARE))  // we were at array end or was just COMMENT/etc.
            return nullptr;  // leave the result variable with old value

        Move_Value(D_OUT, source);
        VAL_INDEX(D_OUT) = index;
        break; }  // update variable

      case REB_VARARGS: {
        REBVAL *position;
        if (Is_Block_Style_Varargs(&position, source)) {
            //
            // We can execute the array, but we must "consume" elements out
            // of it (e.g. advance the index shared across all instances)
            //
            // !!! If any VARARGS! op does not honor the "locked" flag on the
            // array during execution, there will be problems if it is TAKE'n
            // or DO'd while this operation is in progress.
            //
            REBLEN index;
            if (Eval_Step_In_Any_Array_At_Throws(
                SET_END(D_SPARE),
                &index,
                position,
                SPECIFIED,
                EVAL_MASK_DEFAULT
            )){
                // !!! A BLOCK! varargs doesn't technically need to "go bad"
                // on a throw, since the block is still around.  But a FRAME!
                // varargs does.  This will cause an assert if reused, and
                // having BLANK! mean "thrown" may evolve into a convention.
                //
                Init_Unreadable_Blank(position);
                Move_Value(D_OUT, D_SPARE);
                return R_THROWN;
            }

            if (IS_END(D_SPARE)) {
                SET_END(position);  // convention for shared data at end point
                return nullptr;
            }

            VAL_INDEX(position) = index;
        }
        else {
            REBFRM *f;
            if (not Is_Frame_Style_Varargs_May_Fail(&f, source))
                panic (source); // Frame is the only other type

            // By definition, we're in the middle of a function call in frame
            // the varargs came from.  It's still on the stack--we don't want
            // to disrupt its state (beyond feed advancing).  Use a subframe.

            if (IS_END(f->feed->value))
                return nullptr;

            REBFLGS flags = EVAL_MASK_DEFAULT;
            if (Eval_Step_In_Subframe_Throws(D_SPARE, f, flags)) {
                Move_Value(D_OUT, D_SPARE);
                return R_THROWN;
            }

            if (IS_END(D_SPARE))  // remainder just comments and invisibles
                return nullptr;
        }

        Move_Value(D_OUT, source);  // VARARGS! will have updated position
        break; }  // update variable

      default:
        panic (source);
    }

    if (not IS_NULLED(var))
        Set_Var_May_Fail(
            ARG(var), SPECIFIED,
            D_SPARE, SPECIFIED,
            false  // not hard (e.g. GROUP!s don't run, and not literal)
        );

    return D_OUT;
}


//
//  sync-invisibles: native [
//
//  {If an evaluatable source has pending invisibles, execute and advance}
//
//      return: [<opt> block! group! varargs!]
//      source [block! group!]
//  ]
//
REBNATIVE(sync_invisibles)
{
    INCLUDE_PARAMS_OF_SYNC_INVISIBLES;

    // !!! This hasn't been implemented yet.  It is probably best done as
    // an adaptation of Eval_Core() with some kind of mode flag, and
    // would take some redesign to do efficiently.

    if (VAL_LEN_AT(ARG(source)) == 0)
        return nullptr;

    RETURN (ARG(source));
}


//
//  redo: native [
//
//  {Restart a frame's action from the top with its current state}
//
//      return: "Does not return at all (either errors or restarts)"
//          [<opt>]
//      restartee "Frame to restart, or bound word (e.g. REDO 'RETURN)"
//          [frame! any-word!]
//      /other "Restart in a frame-compatible function (sibling tail-call)"
//          [action!]
//  ]
//
REBNATIVE(redo)
//
// This can be used to implement tail-call recursion:
//
// https://en.wikipedia.org/wiki/Tail_call
//
{
    INCLUDE_PARAMS_OF_REDO;

    REBVAL *restartee = ARG(restartee);
    if (not IS_FRAME(restartee)) {
        if (not Did_Get_Binding_Of(D_OUT, restartee))
            fail ("No context found from restartee in REDO");

        if (not IS_FRAME(D_OUT))
            fail ("Context of restartee in REDO is not a FRAME!");

        Move_Value(restartee, D_OUT);
    }

    REBCTX *c = VAL_CONTEXT(restartee);

    REBFRM *f = CTX_FRAME_IF_ON_STACK(c);
    if (f == NULL)
        fail ("Use DO to start a not-currently running FRAME! (not REDO)");

    // If we were given a sibling to restart, make sure it is frame compatible
    // (e.g. the product of ADAPT-ing, CHAIN-ing, ENCLOSE-ing, HIJACK-ing a
    // common underlying function).
    //
    // !!! It is possible for functions to be frame-compatible even if they
    // don't come from the same heritage (e.g. two functions that take an
    // INTEGER! and have 2 locals).  Such compatibility may seem random to
    // users--e.g. not understanding why a function with 3 locals is not
    // compatible with one that has 2, and the test would be more expensive
    // than the established check for a common "ancestor".
    //
    if (REF(other)) {
        REBVAL *sibling = ARG(other);
        if (FRM_UNDERLYING(f) != ACT_UNDERLYING(VAL_ACTION(sibling)))
            fail ("/OTHER function passed to REDO has incompatible FRAME!");

        INIT_VAL_CONTEXT_PHASE(restartee, VAL_ACTION(sibling));
        INIT_BINDING(restartee, VAL_BINDING(sibling));
    }

    // We need to cooperatively throw a restart instruction up to the level
    // of the frame.  Use REDO as the throw label that Eval_Core() will
    // identify for that behavior.
    //
    Move_Value(D_OUT, NAT_VALUE(redo));
    INIT_BINDING(D_OUT, c);

    // The FRAME! contains its ->phase and ->binding, which should be enough
    // to restart the phase at the point of parameter checking.  Make that
    // the actual value that Eval_Core() catches.
    //
    return Init_Thrown_With_Label(D_OUT, restartee, D_OUT);
}


//
//  applique: native [
//
//  {Invoke an ACTION! with all required arguments specified}
//
//      return: [<opt> any-value!]
//      applicand "Literal action, or location to find one (preserves name)"
//          [action! word! path!]
//      def "Frame definition block (will be bound and evaluated)"
//          [block!]
//      /opt "Treat nulls as unspecialized <<experimental!>>"
//  ]
//
REBNATIVE(applique)
{
    INCLUDE_PARAMS_OF_APPLIQUE;

    REBVAL *applicand = ARG(applicand);

    REBFLGS flags = EVAL_MASK_DEFAULT;
    if (not REF(opt))
        flags |= EVAL_FLAG_FULLY_SPECIALIZED;

    DECLARE_END_FRAME (f, flags);  // up front (captures f->dsp)
    Push_Frame(D_OUT, f);  // to avoid GC during `def` evaluation

    // Argument can be a literal action (APPLY :APPEND) or a WORD!/PATH!.
    // If it is a path, we push the refinements to the stack so they can
    // be taken into account, e.g. APPLY 'APPEND/ONLY/DUP pushes /ONLY, /DUP
    //
    REBDSP lowest_ordered_dsp = DSP;
    REBSTR *opt_label;
    if (Get_If_Word_Or_Path_Throws(
        D_OUT,
        &opt_label,
        applicand,
        SPECIFIED,
        true // push_refinements, don't specialize ACTION! on 'APPEND/ONLY/DUP
    )){
        return R_THROWN;
    }

    if (not IS_ACTION(D_OUT))
        fail (PAR(applicand));
    Move_Value(applicand, D_OUT);

    // Make a FRAME! for the ACTION!, weaving in the ordered refinements
    // collected on the stack (if any).  Any refinements that are used in
    // any specialization level will be pushed as well, which makes them
    // out-prioritize (e.g. higher-ordered) than any used in a PATH! that
    // were pushed during the Get of the ACTION!.
    //
    struct Reb_Binder binder;
    INIT_BINDER(&binder);
    REBCTX *exemplar = Make_Context_For_Action_Push_Partials(
        applicand,
        f->baseline.dsp, // lowest_ordered_dsp of refinements to weave in
        &binder,
        CELL_MASK_STACK
    );
    Manage_Array(CTX_VARLIST(exemplar)); // binding code into it

    // Bind any SET-WORD!s in the supplied code block into the FRAME!, so
    // e.g. APPLY 'APPEND [VALUE: 10]` will set VALUE in exemplar to 10.
    //
    // !!! Today's implementation mutates the bindings on the passed-in block,
    // like R3-Alpha's MAKE OBJECT!.  See Virtual_Bind_Deep_To_New_Context()
    // for potential future directions.
    //
    Bind_Values_Inner_Loop(
        &binder,
        VAL_ARRAY_HEAD(ARG(def)), // !!! bindings are mutated!  :-(
        exemplar,
        FLAGIT_KIND(REB_SET_WORD), // types to bind (just set-word!),
        0, // types to "add midstream" to binding as we go (nothing)
        BIND_DEEP
    );

    // Reset all the binder indices to zero, balancing out what was added.
    //
    REBVAL *key = CTX_KEYS_HEAD(exemplar);
    REBVAL *var = CTX_VARS_HEAD(exemplar);
    for (; NOT_END(key); key++, ++var) {
        if (Is_Param_Unbindable(key))
            continue; // shouldn't have been in the binder
        if (Is_Param_Hidden(key))
            continue; // was part of a specialization internal to the action
        Remove_Binder_Index(&binder, VAL_KEY_CANON(key));
    }
    SHUTDOWN_BINDER(&binder); // must do before running code that might BIND

    // Run the bound code, ignore evaluative result (unless thrown)
    //
    PUSH_GC_GUARD(exemplar);
    DECLARE_LOCAL (temp);
    bool def_threw = Do_Any_Array_At_Throws(temp, ARG(def), SPECIFIED);
    DROP_GC_GUARD(exemplar);

    assert(CTX_KEYS_HEAD(exemplar) == ACT_PARAMS_HEAD(VAL_ACTION(applicand)));
    f->param = CTX_KEYS_HEAD(exemplar);
    REBCTX *stolen = Steal_Context_Vars(
        exemplar,
        NOD(VAL_ACTION(applicand))
    );
    INIT_LINK_KEYSOURCE(stolen, NOD(f));  // changes CTX_KEYS_HEAD result

    if (def_threw) {
        Free_Unmanaged_Array(CTX_VARLIST(stolen)); // could TG_Reuse it
        RETURN (temp);
    }

    if (not REF(opt)) {
        //
        // If nulls are taken literally as null arguments, then no arguments
        // are gathered at the callsite, so the "ordering information"
        // on the stack isn't needed.  Eval_Core() will just treat a
        // slot with an INTEGER! for a refinement as if it were "true".
        //
        DS_DROP_TO(lowest_ordered_dsp); // zero refinements on stack, now
    }

    f->varlist = CTX_VARLIST(stolen);
    SET_SERIES_FLAG(f->varlist, STACK_LIFETIME);
    f->rootvar = CTX_ARCHETYPE(stolen);
    f->arg = FRM_ARGS_HEAD(f);  // start state (cues enumeration)
    // f->param assigned above
    f->special = f->arg; // signal only type-check the existing data
    INIT_FRM_PHASE(f, VAL_ACTION(applicand));
    FRM_BINDING(f) = VAL_BINDING(applicand);

    Begin_Prefix_Action(f, opt_label);

    bool action_threw = Eval_Throws(f);
    assert(action_threw or IS_END(f->feed->value));  // we started at END_FLAG

    Drop_Frame(f);

    if (action_threw)
        return R_THROWN;

    return D_OUT;
}
