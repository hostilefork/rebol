//
//  File: %n-function.c
//  Summary: "Generator for an ACTION! whose body is a block of user code"
//  Section: natives
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2020 Revolt Open Source Contributors
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
// FUNC is the basic means for creating a usermode function, implemented by
// a BLOCK! of code, with another block serving as the "spec" for parameters
// and HELP information:
//
//     >> print-sum-twice: func [
//            {Prints the sum of two integers, and return the sum}
//            return: "The sum" [integer!]
//            x "First Value" [integer!]
//            y "Second Value" [integer!]
//            <local> sum
//        ][
//            sum: x + y
//            loop 2 [print ["The sum is" sum]]
//            return sum
//        ]
//
//     >> print-sum-twice 10 20
//     The sum is 30
//     The sum is 30
//
// Revolt brings many new abilities not present in historical Rebol:
//
// * Return-type checking via `return: [...]` in the spec
//
// * Definitional RETURN, so that each FUNC has a local definition of its
//   own version of return specially bound to its invocation.
//
// * Specific binding of arguments, so that each instance of a recursion
//   can discern WORD!s from each recursion.  (In R3-Alpha, this was only
//   possible using CLOSURE which made a costly deep copy of the function's
//   body on every invocation.  Revolt's method does not require a copy.)
//
// * Invisible functions (return: []) that vanish completely, leaving whatever
//   result was in the evaluation previous to the function call as-is.
//
// * A FRAME! object type that bundles together a function instance and its
//   parameters, which can be invoked or turned into a specialized ACTION!.
//
// * Refinements-as-their-own-arguments--which streamlines the evaluator,
//   saves memory, simplifies naming, and simplifies the FRAME! mechanics.
//
// * Many mechanisms for adjusting the behavior or parameterization of
//   functions without rewriting them, e.g. SPECIALIZE, ADAPT, ENCLOSE,
//   AUGMENT, CHAIN, and HIJACK.  These derived function generators can be
//   applied equally well to natives as well as user functions...or to other
//   derivations.
//

#include "sys-core.h"


//
//  Void_Dispatcher: C
//
// If you write `func [return: <void> ...] []` it uses this dispatcher instead
// of running the evaluator on an empty block.  This is less useless than it
// may sound: you can make fast stub actions that only cost if they are
// HIJACK'd (e.g. ASSERT is done this way).
//
REB_R Void_Dispatcher(REBFRM *f)
{
    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    assert(VAL_LEN_AT(ARR_HEAD(details)) == 0);
    UNUSED(details);

    return Init_Void(f->out);
}


//
//  Null_Dispatcher: C
//
// Analogue to Void_Dispatcher() for `func [return: [<opt>] ...] [null]`
// situations.
//
REB_R Null_Dispatcher(REBFRM *f)
{
    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    assert(VAL_LEN_AT(ARR_HEAD(details)) == 0);
    UNUSED(details);

    return nullptr;
}


//
//  Unchecked_Dispatcher: C
//
// Runs block, then no typechecking (e.g. had no RETURN: [...] type spec)
//
// Doubles as common behavior shared by dispatchers which execute on BLOCK!s
// of code.  (Should it be called "Interpreted_Dispatcher" or similar?)
//
// In order to do additional checking or output tweaking, the best way is to
// change the phase of the frame so that instead of re-entering this unchecked
// dispatcher, it will call some other function to do it.  This is different
// from natives which are their own dispatchers, and able to declare locals
// in their frames to act as a kind of state machine.  But the dispatchers
// are for generic code--hence messing with the frames is not ideal.
//
REB_R Unchecked_Dispatcher(REBFRM *f)
{
    Push_Delegation_Details_0(f->out, f);  // no re-entry after eval
    STATE_BYTE(f) = 1;  // STATE_BYTE() == 0 is reserved for initial entry
    return R_CONTINUATION;
}


//
//  Voider_Dispatcher: C
//
// Runs block, then overwrites result w/void (e.g. RETURN: <void>)
//
REB_R Voider_Dispatcher(REBFRM *f)
{
    enum {
        ST_VOIDER_INITIAL_ENTRY = 0,
        ST_VOIDER_RUNNING_BODY
    };

    switch (STATE_BYTE(f)) {
      case ST_VOIDER_INITIAL_ENTRY: goto initial_entry;
      case ST_VOIDER_RUNNING_BODY: goto body_ran;
      default: assert(false);
    }

  initial_entry: {
    Push_Continuation_Details_0(f->out, f);  // body lives in ACT_DETAILS[0]
    STATE_BYTE(f) = ST_VOIDER_RUNNING_BODY;
    return R_CONTINUATION;
  }

  body_ran: {
    return Init_Void(f->out);  // discards whatever actual result was
  }
}


//
//  Returner_Dispatcher: C
//
// Runs block, ensure type matches RETURN: [...] specification, else fail.
//
// Note: Natives get this check only in the debug build, but not here (their
// "dispatcher" *is* the native!)  So the extra check is in Eval_Core().
//
REB_R Returner_Dispatcher(REBFRM *f)
{
    enum {
        ST_RETURNER_INITIAL_ENTRY = 0,
        ST_RETURNER_RUNNING_BODY
    };

    switch (STATE_BYTE(f)) {
      case ST_RETURNER_INITIAL_ENTRY: goto initial_entry;
      case ST_RETURNER_RUNNING_BODY: goto body_ran;
      default: assert(false);
    }

  initial_entry: {
    Push_Continuation_Details_0(f->out, f);  // body lives in ACT_DETAILS[0]
    STATE_BYTE(f) = ST_RETURNER_RUNNING_BODY;
    return R_CONTINUATION;
  }

  body_ran: {
    FAIL_IF_BAD_RETURN_TYPE(f);  // all we do is check the return type
    return f->out;
  }
}


//
//  Elider_Dispatcher: C
//
// Used by "invisible" functions (who in their spec say `RETURN: []`).  Runs
// block but with no net change to f->out.
//
// !!! Review enforcement of arity-0 return in invisibles only!
//
REB_R Elider_Dispatcher(REBFRM *f)
{
    // It might seem that since we don't want the result, we could instruct a
    // delegation to write it into the spare where it won't affect f->out.
    // That way the body could very efficiently be:
    //
    //   `return Init_Delegation_Details_0(FRM_SPARE(f), f);`
    //
    // But the current RETURN implementation climbs the stack and trashes
    // f->out as it goes.  Not only that, there's a rule that all invisibles
    // return R_INVISIBLE...so a pure delegation would have to deal with
    // that.  The current workaround is to save `f->out` into the frame's
    // spare cell, request a callback after the continuation is done, and put
    // it back.  Hopefully a better answer will come along.

    enum {
        ST_ELIDER_INITIAL_ENTRY = 0,
        ST_ELIDER_RUNNING_BODY
    };

    switch (STATE_BYTE(f)) {
      case ST_ELIDER_INITIAL_ENTRY: goto initial_entry;
      case ST_ELIDER_RUNNING_BODY: goto body_ran;
      default: assert(false);
    }

  initial_entry: {
    if (IS_END(f->out)) {  // could happen :-/ e.g. `do [comment "" ...]`
        Init_Void(FRM_SPARE(f));
        SET_CELL_FLAG(FRM_SPARE(f), SPARE_MARKED_END);  // be tricky
    }
    else {
        Move_Value(FRM_SPARE(f), f->out);  // cache, mark first step done
        if (GET_CELL_FLAG(f->out, UNEVALUATED))
            SET_CELL_FLAG(FRM_SPARE(f), UNEVALUATED);  // proxy eval flag
    }
    Push_Continuation_Details_0(f->out, f);  // re-entry after eval
    STATE_BYTE(f) = ST_ELIDER_RUNNING_BODY;
    return R_CONTINUATION;
  }

  body_ran: {
    if (GET_CELL_FLAG(FRM_SPARE(f), SPARE_MARKED_END)) {
        assert(IS_VOID(FRM_SPARE(f)));
        SET_END(f->out);
    } else {
        Move_Value(f->out, FRM_SPARE(f));  // restore output
        if (GET_CELL_FLAG(FRM_SPARE(f), UNEVALUATED))
            SET_CELL_FLAG(f->out, UNEVALUATED);
    }
    return R_INVISIBLE;  // invisibles should always return this
  }
}


//
//  Commenter_Dispatcher: C
//
// This is a specialized version of Elider_Dispatcher() for when the body of
// a function is empty.  This helps COMMENT and functions like it run faster.
//
REB_R Commenter_Dispatcher(REBFRM *f)
{
    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    RELVAL *body = ARR_HEAD(details);
    assert(VAL_LEN_AT(body) == 0);
    UNUSED(body);
    return R_INVISIBLE;
}


//
//  Make_Interpreted_Action_May_Fail: C
//
// This is the support routine behind both `MAKE ACTION!` and FUNC.
//
// Revolt's schematic is *very* different from R3-Alpha, whose definition of
// FUNC was simply:
//
//     make function! copy/deep reduce [spec body]
//
// Revolt's `make action!` doesn't need to copy the spec (it does not save
// it--parameter descriptions are in a meta object).  The body is copied
// implicitly (as it must be in order to relativize it).
//
// There is also a "definitional return" MKF_RETURN option used by FUNC, so
// the body will introduce a RETURN specific to each action invocation, thus
// acting more like:
//
//     return: make action! [
//         [{Returns a value from a function.} value [<opt> any-value!]]
//         [unwind/with (binding of 'return) :value]
//     ]
//     (body goes here)
//
// This pattern addresses "Definitional Return" in a way that does not need to
// build in RETURN as a language keyword in any specific form (in the sense
// that MAKE ACTION! does not itself require it).
//
// FUNC optimizes by not internally building or executing the equivalent body,
// but giving it back from BODY-OF.  This gives FUNC the edge to pretend to
// add containing code and simulate its effects, while really only holding
// onto the body the caller provided.
//
// While plain MAKE ACTION! has no RETURN, UNWIND can be used to exit frames
// but must be explicit about what frame is being exited.  This can be used
// by usermode generators that want to create something return-like.
//
REBACT *Make_Interpreted_Action_May_Fail(
    const REBVAL *spec,
    const REBVAL *body,
    REBFLGS mkf_flags,  // MKF_RETURN, etc.
    REBLEN details_capacity
){
    assert(IS_BLOCK(spec) and IS_BLOCK(body));
    assert(details_capacity >= 1);  // relativized body put in details[0]

    REBACT *a = Make_Action(
        Make_Paramlist_Managed_May_Fail(spec, mkf_flags),
        &Null_Dispatcher,  // will be overwritten if non-[] body
        nullptr,  // no underlying action (use paramlist)
        nullptr,  // no specialization exemplar (or inherited exemplar)
        details_capacity  // we fill in details[0], caller fills any extra
    );

    // We look at the *actual* function flags; e.g. the person may have used
    // the FUNC generator (with MKF_RETURN) but then named a parameter RETURN
    // which overrides it, so the value won't have PARAMLIST_HAS_RETURN.

    REBARR *copy;
    if (VAL_ARRAY_LEN_AT(body) == 0) {  // optimize empty body case

        if (GET_ACTION_FLAG(a, IS_INVISIBLE)) {
            ACT_DISPATCHER(a) = &Commenter_Dispatcher;
        }
        else if (SER(a)->info.bits & ARRAY_INFO_MISC_VOIDER) {
            ACT_DISPATCHER(a) = &Voider_Dispatcher;  // !!! ^-- see info note
        }
        else if (GET_ACTION_FLAG(a, HAS_RETURN)) {
            REBVAL *typeset = ACT_PARAMS_HEAD(a);
            assert(VAL_PARAM_SYM(typeset) == SYM_RETURN);
            if (not TYPE_CHECK(typeset, REB_NULLED))  // `do []` returns
                ACT_DISPATCHER(a) = &Returner_Dispatcher;  // error when run
        }
        else {
            // Keep the Null_Dispatcher passed in above
        }

        // Reusing EMPTY_ARRAY won't allow adding ARRAY_HAS_FILE_LINE bits
        //
        copy = Make_Array_Core(1, NODE_FLAG_MANAGED);
    }
    else {  // body not empty, pick dispatcher based on output disposition

        if (GET_ACTION_FLAG(a, IS_INVISIBLE))
            ACT_DISPATCHER(a) = &Elider_Dispatcher; // no f->out mutation
        else if (SER(a)->info.bits & ARRAY_INFO_MISC_VOIDER) // !!! see note
            ACT_DISPATCHER(a) = &Voider_Dispatcher; // forces f->out void
        else if (GET_ACTION_FLAG(a, HAS_RETURN))
            ACT_DISPATCHER(a) = &Returner_Dispatcher; // type checks f->out
        else
            ACT_DISPATCHER(a) = &Unchecked_Dispatcher; // unchecked f->out

        copy = Copy_And_Bind_Relative_Deep_Managed(
            body,  // new copy has locals bound relatively to the new action
            ACT_PARAMLIST(a),
            TS_WORD,
            did (mkf_flags & MKF_GATHER_LETS) // transitional LET technique
        );
    }

    // Favor the spec first, then the body, for file and line information.
    //
    if (GET_ARRAY_FLAG(VAL_ARRAY(spec), HAS_FILE_LINE_UNMASKED)) {
        LINK_FILE_NODE(copy) = LINK_FILE_NODE(VAL_ARRAY(spec));
        MISC(copy).line = MISC(VAL_ARRAY(spec)).line;
        SET_ARRAY_FLAG(copy, HAS_FILE_LINE_UNMASKED);
    }
    else if (GET_ARRAY_FLAG(VAL_ARRAY(body), HAS_FILE_LINE_UNMASKED)) {
        LINK_FILE_NODE(copy) = LINK_FILE_NODE(VAL_ARRAY(body));
        MISC(copy).line = MISC(VAL_ARRAY(body)).line;
        SET_ARRAY_FLAG(copy, HAS_FILE_LINE_UNMASKED);
    }
    else {
        // Ideally all source series should have a file and line numbering
        // At the moment, if a function is created in the body of another
        // function it doesn't work...trying to fix that.
    }

    // Save the relativized body in the action's details block.  Since it is
    // a RELVAL* and not a REBVAL*, the dispatcher must combine it with a
    // running frame instance (the REBFRM* received by the dispatcher) before
    // executing the interpreted code.
    //
    REBARR *details = ACT_DETAILS(a);
    RELVAL *rebound = Init_Relative_Block(
        ARR_AT(details, IDX_NATIVE_BODY),
        a,
        copy
    );

    // Capture the mutability flag that was in effect when this action was
    // created.  This allows the following to work:
    //
    //    >> do mutable [f: function [] [b: [1 2 3] clear b]]
    //    >> f
    //    == []
    //
    // So even though the invocation is outside the mutable section, we have
    // a memory that it was created under those rules.  (It's better to do
    // this based on the frame in effect than by looking at the CONST flag of
    // the incoming body block, because otherwise ordinary Revolt functions
    // whose bodies were created from dynamic code would have mutable bodies
    // by default--which is not a desirable consequence from merely building
    // the body dynamically.)
    //
    // Note: besides the general concerns about mutability-by-default, when
    // functions are allowed to modify their bodies with words relative to
    // their frame, the words would refer to that specific recursion...and not
    // get picked up by other recursions that see the common structure.  This
    // means compatibility would be with the behavior of R3-Alpha CLOSURE,
    // not with R3-Alpha FUNCTION.
    //
    if (GET_CELL_FLAG(body, CONST))
        SET_CELL_FLAG(rebound, CONST);  // Inherit_Const() would need REBVAL*

    return a;
}


//
//  func*: native [
//
//  "Defines an ACTION! with given spec and body"
//
//      return: [action!]
//      spec "Help string (opt) followed by arg words (and opt type + string)"
//          [block!]
//      body "Code implementing the function--use RETURN to yield a result"
//          [block!]
//  ]
//
REBNATIVE(func_p)
{
    INCLUDE_PARAMS_OF_FUNC_P;

    REBACT *func = Make_Interpreted_Action_May_Fail(
        ARG(spec),
        ARG(body),
        MKF_RETURN | MKF_KEYWORDS | MKF_GATHER_LETS,
        1  // details capacity... just the one array slot (will be filled)
    );

    return Init_Action_Unbound(D_OUT, func);
}


//
//  Init_Thrown_Unwind_Value: C
//
// This routine generates a thrown signal that can be used to indicate a
// desire to jump to a particular level in the stack with a return value.
// It is used in the implementation of the UNWIND native.
//
// See notes is %sys-frame.h about how there is no actual REB_THROWN type.
//
REB_R Init_Thrown_Unwind_Value(
    REBVAL *out,
    const REBVAL *level, // FRAME!, ACTION! (or INTEGER! relative to frame)
    const REBVAL *value,
    REBFRM *frame // required if level is INTEGER! or ACTION!
) {
    Move_Value(out, NATIVE_VAL(unwind));

    if (IS_FRAME(level)) {
        INIT_BINDING(out, VAL_CONTEXT(level));
    }
    else if (IS_INTEGER(level)) {
        REBLEN count = VAL_INT32(level);
        if (count <= 0)
            fail (Error_Invalid_Exit_Raw());

        REBFRM *f = frame->prior;
        for (; true; f = f->prior) {
            if (f == FS_BOTTOM)
                fail (Error_Invalid_Exit_Raw());

            if (not Is_Action_Frame(f))
                continue; // only exit functions

            if (Is_Action_Frame_Fulfilling(f))
                continue; // not ready to exit

            --count;
            if (count == 0) {
                INIT_BINDING_MAY_MANAGE(out, SPC(f->varlist));
                break;
            }
        }
    }
    else {
        assert(IS_ACTION(level));

        REBFRM *f = frame->prior;
        for (; true; f = f->prior) {
            if (f == FS_BOTTOM)
                fail (Error_Invalid_Exit_Raw());

            if (not Is_Action_Frame(f))
                continue; // only exit functions

            if (Is_Action_Frame_Fulfilling(f))
                continue; // not ready to exit

            if (VAL_ACTION(level) == f->original) {
                INIT_BINDING_MAY_MANAGE(out, SPC(f->varlist));
                break;
            }
        }
    }

    return Init_Thrown_With_Label(out, value, out);
}


//
//  unwind: native [
//
//  {Jump up the stack to return from a specific frame or call.}
//
//      level "Frame, action, or index to exit from"
//          [frame! action! integer!]
//      result "Result for enclosing state"
//          [<opt> <end> any-value!]
//  ]
//
REBNATIVE(unwind)
//
// UNWIND is implemented via a throw that bubbles through the stack.  Using
// UNWIND's action REBVAL with a target `binding` field is the protocol
// understood by Eval_Core to catch a throw itself.
//
// !!! Allowing to pass an INTEGER! to jump from a function based on its
// BACKTRACE number is a bit low-level, and perhaps should be restricted to
// a debugging mode (though it is a useful tool in "code golf").
{
    INCLUDE_PARAMS_OF_UNWIND;

    return Init_Thrown_Unwind_Value(
        D_OUT,
        ARG(level),
        IS_ENDISH_NULLED(ARG(result)) ? VOID_VALUE : ARG(result),
        frame_
    );
}


//
//  return: native [
//
//  {RETURN, giving a result to the caller}
//
//      value "If no argument is given, result will be a VOID!"
//          [<end> <opt> any-value!]
//  ]
//
REBNATIVE(return)
{
    INCLUDE_PARAMS_OF_RETURN;

    REBFRM *f = frame_; // implicit parameter to REBNATIVE()

    // Each ACTION! cell for RETURN has a piece of information in it that can
    // can be unique (the binding).  When invoked, that binding is held in the
    // REBFRM*.  This generic RETURN dispatcher interprets that binding as the
    // FRAME! which this instance is specifically intended to return from.
    //
    REBNOD *f_binding = FRM_BINDING(f);
    if (not f_binding)
        fail (Error_Return_Archetype_Raw());  // must have binding to jump to

    REBFRM *target_frame = CTX_FRAME_MAY_FAIL(CTX(f_binding));

    // !!! We only have a REBFRM via the binding.  We don't have distinct
    // knowledge about exactly which "phase" the original RETURN was
    // connected to.  As a practical matter, it can only return from the
    // current phase (what other option would it have, any other phase is
    // either not running yet or has already finished!).  But this means the
    // `target_frame->phase` may be somewhat incidental to which phase the
    // RETURN originated from...and if phases were allowed different return
    // typesets, then that means the typechecking could be somewhat random.
    //
    // Without creating a unique tracking entity for which phase was
    // intended for the return, it's not known which phase the return is
    // for.  So the return type checking is done on the basis of the
    // underlying function.  So compositions that share frames cannot expand
    // the return type set.  The unfortunate upshot of this is--for instance--
    // that an ENCLOSE'd function can't return any types the original function
    // could not.  :-(
    //
    REBACT *target_fun = FRM_UNDERLYING(target_frame);

    REBVAL *v = ARG(value);

    // Defininitional returns are "locals"--there's no argument type check.
    // So TYPESET! bits in the RETURN param are used for legal return types.
    //
    REBVAL *typeset = ACT_PARAMS_HEAD(target_fun);
    assert(VAL_PARAM_CLASS(typeset) == REB_P_LOCAL);
    assert(VAL_PARAM_SYM(typeset) == SYM_RETURN);

    if (GET_ACTION_FLAG(target_fun, IS_INVISIBLE) and IS_ENDISH_NULLED(v)) {
        //
        // The only legal way invisibles can use RETURN is with no argument.
    }
    else {
        if (IS_ENDISH_NULLED(v))
            Init_Void(v); // `do [return]` acts as `return void`

        // Check type NOW instead of waiting and letting Eval_Core()
        // check it.  Reasoning is that the error can indicate the callsite,
        // e.g. the point where `return badly-typed-value` happened.
        //
        // !!! In the userspace formulation of this abstraction, it indicates
        // it's not RETURN's type signature that is constrained, as if it were
        // then RETURN would be implicated in the error.  Instead, RETURN must
        // take [<opt> any-value!] as its argument, and then report the error
        // itself...implicating the frame (in a way parallel to this native).
        //
        if (not TYPE_CHECK(typeset, VAL_TYPE(v)))
            fail (Error_Bad_Return_Type(target_frame, VAL_TYPE(v)));
    }

    assert(f_binding->header.bits & ARRAY_FLAG_IS_VARLIST);

    Move_Value(D_OUT, NATIVE_VAL(unwind)); // see also Make_Thrown_Unwind_Value
    INIT_BINDING_MAY_MANAGE(D_OUT, f_binding);

    return Init_Thrown_With_Label(D_OUT, v, D_OUT);
}
