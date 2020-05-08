//
//  File: %c-yielder.c
//  Summary: "Routines for Creating Coroutine Functions via Stackless Methods"
//  Section: datatypes
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2020 Revolt Open Source Contributors
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the GNU Lesser General Public License (LGPL), Version 3.0.
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.en.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Generators utilize the ability of the system to suspend and resume stacks.
//

#include "sys-core.h"

enum {
    IDX_YIELDER_BODY = 0,  // Init_Continuation_Details_0() uses details[0]
    IDX_YIELDER_STATE = 1,  // can't be frame spare (that's reset each call!)
    IDX_YIELDER_LAST_YIELDER_CONTEXT = 2,  // frame stack fragment to resume
    IDX_YIELDER_LAST_YIELD_RESULT = 3,  // so that `z: yield 1 + 2` is useful
    IDX_YIELDER_DATA_STACK = 4,  // saved if you YIELD during REDUCE, etc.
    IDX_YIELDER_MAX
};


//
//  Yielder_Dispatcher: C
//
// A yielder is a function instance which is made by a generator, that keeps
// a memory of the frame state it was in.  YIELD packs up the frame in a
// restartable way and unwinds it, allowing the continuation to request
// that be the frame that gets executed in the continuation.
//
REB_R Yielder_Dispatcher(REBFRM *f)
{
    REBACT *phase = FRM_PHASE(f);
    REBARR *details = ACT_DETAILS(phase);
    RELVAL *state = ARR_AT(details, IDX_YIELDER_STATE);

    // Each time a new invocation of a function runs, the `spare` cell of the
    // frame is set to END.  But if a dispatcher gets called back due to a
    // continuation, it will be whatever the spare was left at.  This helps
    // differentiate a call while the generator is running (illegal) from one
    // that is a call after a yield (legal) by setting it to a non-END value
    // (e.g. blank) before running a continuation.
    //
    REBVAL *spare = FRM_SPARE(f);

    if (IS_BLANK(state)) {  // first run
        Init_Void(state);  // indicate "running" (no recursions allowed)
        Init_Blank(spare);  // differentiate continuation state from new call

        // Whatever we pass through here as the specifier has to stay working,
        // because it will be threaded and preserved in variables by the
        // running code (also, it's the binding of the YIELD statement, which
        // needs to be able to find the right frame).
        //
        // If there is no yield, we want a callback so we can mark the
        // generator as finished.
        //
        return Init_Continuation_Details_0(f->out, f);  // re-enter after eval
    }

    if (IS_FRAME(state)) {  // interrupted frame stack fragment needs resuming
        REBFRM *yield_frame = CTX_FRAME_IF_ON_STACK(VAL_CONTEXT(state));
        assert(yield_frame != nullptr);

        // The YIELD binding pointed to the context varlist we used in the
        // original yielder dispatch.  That completed--but we need to reuse
        // the identity for this new yielder frame for the YIELD to find it
        // in the stack walk.
        //
        REBCTX *last_yielder_context = VAL_CONTEXT(
            ARR_AT(details, IDX_YIELDER_LAST_YIELDER_CONTEXT)
        );

        // We want the identity of the old varlist to replace this yielder's
        // varlist identity.  But we want the frame's values to reflect the
        // args the user passed in to this invocation of the yielder.  So move
        // those into the old varlist before replacing this varlist with that
        // prior identity.
        //
        REBVAL *param = CTX_KEYS_HEAD(last_yielder_context);
        REBVAL *dest = CTX_VARS_HEAD(last_yielder_context);
        REBVAL *src = FRM_ARGS_HEAD(f);
        for (; NOT_END(src); ++param, ++dest, ++src) {
            if (VAL_PARAM_CLASS(param) == REB_P_LOCAL)
                continue;  // don't overwrite locals (including YIELD)
            Move_Value(dest, src);  // all arguments/refinements are fair game
        }
        assert(IS_END(src));
        assert(IS_END(dest));

        // With variables extracted, we no longer need the varlist for this
        // invocation (wrong identity) so we free it, if it isn't GC-managed.
        //
        if (NOT_SERIES_FLAG(f->varlist, MANAGED))  // won't just get GC'd
            GC_Kill_Series(SER(f->varlist));  // Note: not alloc'd w/tracking

        // When the last yielder dropped from the frame stack, it should have
        // decayed its keysource from a REBFRM* to the action that was
        // invoked (which could be an arbitrary specialization--e.g. different
        // variants of the yielder with different f->original could be used
        // between calls).  This means we can only compare underlying actions.
        //
        // Now we have a new REBFRM*, so we can reattach the context to that.
        //
        assert(
            ACT_UNDERLYING(ACT(LINK_KEYSOURCE(last_yielder_context)))
            == ACT_UNDERLYING(f->original)  // mostly just check not a REBFRM*
        );
        INIT_LINK_KEYSOURCE(last_yielder_context, NOD(f));  // but now it is!

        // Now that the last call's context varlist is pointing at our current
        // invocation frame, we point the other way from the frame to the
        // varlist.  We also update the cached pointer to the rootvar of that
        // frame (used to speed up FRM_PHASE() and FRM_BINDING())
        //
        f->varlist = CTX_VARLIST(last_yielder_context);
        f->rootvar = CTX_ARCHETYPE(last_yielder_context);  // must match

        REBFRM *temp = yield_frame;  // go up to seek frame under past yielder
        while (true) {
            //
            // The top frame for the last yielder could target any output.
            // But the last yielder finished, and that output may well be
            // gone (e.g. written into an API cell that was released).  But
            // more frames than that could have inherited the same f->out.
            // YIELD put a bogus pointer to the read-only TRUE_VALUE cell,
            // which is good enough to be GC safe and also distinct.
            //
            // Anywhere we see that signal, replace it with the output that
            // this new invocation of the yielder wants to write to.
            //
            if (temp->out == TRUE_VALUE)
                temp->out = f->out;

            // We're going to restore the values that were between the yielder
            // and the yield on the data stack.  But that means we have to
            // touch up the `dsp_orig` pointers as well in the frames.
            //
            temp->dsp_orig += f->dsp_orig;  // YIELD made dsp_orig zero-based

            if (temp->prior == nullptr)
                break;
            temp = temp->prior;
        }

        // We chain the stack that was underneath the old call to the yielder
        // so that it now considers this yielder the parent.  We also update
        // the outputs of that subframe to match the output of the current
        // frame (see assert in YIELD that proves subframe had same f->out).
        //
        assert(temp->out == f->out);  // frame under yielder was TRUE_VALUE
        temp->prior = f;

        // We could make YIELD appear to return a VOID! when we jump back in
         // to resume it.  But it's more interesting to return what the YIELD
        // received as an arg (YIELD cached it in details before jumping)
        //
        Move_Value(
            yield_frame->out,
            KNOWN(ARR_AT(details, IDX_YIELDER_LAST_YIELD_RESULT))
        );

        // Now add in all the data stack elements.
        //
        RELVAL *data_stack = KNOWN(ARR_AT(details, IDX_YIELDER_DATA_STACK));
        assert(VAL_INDEX(data_stack) == 0);  // could store some number (?)
        REBVAL *stack_item = KNOWN(VAL_ARRAY_HEAD(data_stack));
        for (; NOT_END(stack_item); ++stack_item)
            Move_Value(DS_PUSH(), stack_item);
        Init_Blank(data_stack);  // no longer needed, let it be GC'd

        // If the yielder actually reaches its end (instead of YIELD-ing)
        // we need to know, so we can mark that it is finished.
        //
        assert(NOT_EVAL_FLAG(f, DELEGATE_CONTROL));

        Init_Void(state);  // indicate "running" (no recursions allowed)
        Init_Blank(spare);  // differentiate continuation state from new call

        TG_Top_Frame = yield_frame;  // jump deep back into the yield...
        return R_DEWIND;  // ...resuming where we left off
    }

    if (IS_LOGIC(state)) {  // finished...no more calls (!!! or error?)
        assert(not VAL_LOGIC(state));  // only using `false` at the moment
        return nullptr;
    }

    assert(IS_VOID(state));  // body is currently running, not yielded or new

    if (IS_END(FRM_SPARE(f)))  // running but this is a new call without yield
        fail ("Generator called while running; recursions not allowed");

    assert(IS_BLANK(FRM_SPARE(f)));  // differentiation for continuations
    Init_False(state);  // indicate the run is finished

    // Finished, and f->out is the actual result we get due to continuation.
    // We discard it so that values don't accidentally "fall out" when there
    // is no `yield`.  (Note RETURN is also 0-arity, so if you want to yield
    // and return prematurely it's two steps: `yield value | return`
    //
    return nullptr;
}


//
//  yielder: native [
//
//      return: "Action that can be called repeatedly until it yields NULL"
//          [action!]
//      spec "Arguments passed in to each call for the generator"
//          [block!]
//      body "Code containing YIELD statements"
//          [block!]
//  ]
//
REBNATIVE(yielder)
{
    INCLUDE_PARAMS_OF_YIELDER;

    // We start by making an ordinary-seeming interpreted function, but that
    // has a local "yield" which is bound to the frame upon execution.
    //
    REBVAL *body = rebValue("compose [",
        "let yield: bind :lib/yield binding of 'yield",
        "(as group!", ARG(body), ")",  // GROUP! so it can't backquote 'YIELD
    "]", rebEND);

    REBACT *yielder = Make_Interpreted_Action_May_Fail(
        ARG(spec),
        body,
        MKF_KEYWORDS | MKF_GATHER_LETS,  // no RETURN (similar to DOES)
        IDX_YIELDER_MAX  // details array capacity
    );
    rebRelease(body);

    // !!! Make_Interpreted_Action_May_Fail() does not take a dispatcher
    // argument, because it makes a decision on if an optimized one should
    // be used based on the paramlist.  So we override it, but this points
    // out some dissonance: e.g. what would a `return: <void>` yielder mean?

    REBARR *details = ACT_DETAILS(yielder);
    MISC(details).dispatcher = &Yielder_Dispatcher;

    assert(IS_BLOCK(ARR_AT(details, IDX_YIELDER_BODY)));
    Init_Blank(ARR_AT(details, IDX_YIELDER_STATE));  // starting
    Init_Blank(ARR_AT(details, IDX_YIELDER_LAST_YIELDER_CONTEXT));
    Init_Blank(ARR_AT(details, IDX_YIELDER_LAST_YIELD_RESULT));
    Init_Blank(ARR_AT(details, IDX_YIELDER_DATA_STACK));

    return Init_Action_Unbound(D_OUT, yielder);
}


//
//  generator: native [
//
//      return: "Arity-0 action you can call repeatedly until it yields NULL"
//          [action!]
//      body "Code containing YIELD statements"
//          [block!]
//  ]
//
REBNATIVE(generator)
{
    INCLUDE_PARAMS_OF_GENERATOR;

    return rebValue(NAT_VALUE(yielder), EMPTY_BLOCK, ARG(body), rebEND);
}


//
//  yield: native [
//
//  {Function used with GENERATOR and YIELDER to give back results}
//
//      return: "Same value given as input, won't return until resumption"
//          [<opt> any-value!]
//      value "Value to yield (null is no-op)"
//          [<opt> any-value!]
//  ]
//
REBNATIVE(yield)
//
// The benefits of distinguishing NULL as a generator result meaning the body
// has completed are considered to outweigh the ability to yield NULL.  While
// this is believed to be good, it can be overridden, for instance:
//
//     n-generator: func [body [block!]] [
//         let g: generator compose [
//             yield: enclose 'yield func [f] [
//                 f/value: quote :f/value  ; chained generator will dequote
//                 return dequote do f  ; also dequote YIELD's return
//             ]
//             (as group! body)
//         ]
//         return chain [:g | :dequote]
//     ]
{
    INCLUDE_PARAMS_OF_YIELD;

    assert(frame_ == TG_Top_Frame);  // frame_ is an implicit REBNATIVE() arg
    assert(FRM_PHASE(frame_) == NAT_ACTION(yield));
    REBFRM * const yield_frame = frame_;  // ...make synonyms more obvious

    REBNOD *f_binding = FRM_BINDING(yield_frame);
    if (not f_binding)
        fail (Error_Yield_Archetype_Raw());  // must have yielder to jump to

    REBCTX *yielder_context = CTX(f_binding);
    REBFRM *yielder_frame = CTX_FRAME_MAY_FAIL(yielder_context);
    if (not yielder_frame)
        fail ("Cannot yield to generator that has completed");

    REBACT *yielder_phase = FRM_PHASE(yielder_frame);
    assert(ACT_DISPATCHER(yielder_phase) == &Yielder_Dispatcher);

    // !!! How much sanity checking should be done before doing the passing
    // thru of the NULL?  Err on the side of safety first, and don't let NULL
    // be yielded to the unbound archetype or completed generators.
    //
    if (IS_NULLED(ARG(value)))
        return nullptr;

    REBARR *yielder_details = ACT_DETAILS(yielder_phase);

  blockscope {
    REBFRM *f = yield_frame;
    while (true) {
        if (f->out == yielder_frame->out) {
            //
            // Reassign to mark the output as something randomly bad, but
            // still GC safe.  When the stack gets patched back in, it will
            // be recognized and reset to the new yielder's out.
            //
            f->out = cast(REBVAL*, TRUE_VALUE);
        }

        // We make the dsp_orig stack pointers in each frame relative to the
        // yielder_frame, with that frame as if it were 0.  When the yielder
        // gets called again, we'll add the new yielder's base dsp back in.
        //
        // !!! This may confuse a fail() if it expects to climb the stack and
        // see all the f->dsp_orig be sane.  But as far as the interim state
        // is concerned, there's no good number to put here...leaving it as
        // it was would be wrong too.  This might suggest an EVAL_FLAG for
        // "don't believe the dsp".  Tricky.
        //
        f->dsp_orig -= yielder_frame->dsp_orig;

        if (f->prior == yielder_frame) {
            //
            // The frame below a yielder was not fulfilling an argument, it
            // should be writing into the yielder's out cell.  But when the
            // yielder goes off the stack, that cell will most likely be
            // gone.  We'll have to point it at the new yielder's out cell
            // when we chain it back in.  Also we have to set it to something
            // legal to mark in GC as the cell will go stale.
            //
            assert(f->out == TRUE_VALUE);  // should have matched yielder
            f->prior = nullptr;  // show where the fragment of stack ends
            break;
        }
        f = f->prior;

        if (f == FS_TOP)  // "alive", but couldn't find it in the stack walk
            fail ("Cannot yield to a generator that is suspended");

        if (GET_EVAL_FLAG(f, ROOT_FRAME))
            fail ("Cannot yield across frame that's not a continuation");
    }
  }

    // If any data stack has been accrued, we capture it into an array.  We
    // will have to re-push the values when the yielder is resumed.
    //
    // !!! We do not technically need to manage this array...just keep the
    // values in it alive during GC.  It could be stored in some REBFRM* field
    // and marked.  But for simplicity, we keep it in the yielder's details
    // as a value cell, and manage it.
    //
    RELVAL *data_stack = ARR_AT(yielder_details, IDX_YIELDER_DATA_STACK);
    Init_Block(
        data_stack,
        Pop_Stack_Values(yielder_frame->dsp_orig)
    );

    // We preserve the fragment of call stack leading from the yield up to the
    // yielder in a FRAME! value that the yielder holds in its `details`.
    // The garbage collector should notice it is there, and mark it live up
    // until the nullptr that we put at the root.
    //
    RELVAL *state = ARR_AT(yielder_details, IDX_YIELDER_STATE);
    assert(IS_VOID(state));  // should be in "running" state
    Init_Frame(state, Context_For_Frame_May_Manage(yield_frame));
    ASSERT_ARRAY_MANAGED(VAL_CONTEXT(state));
    assert(CTX_FRAME_IF_ON_STACK(VAL_CONTEXT(state)) == yield_frame);

    // We store the frame chain into the yielder, as a FRAME! value.  The
    // GC of the ACTION's details will keep it alive.
    //
    Init_Frame(
        ARR_AT(yielder_details, IDX_YIELDER_LAST_YIELDER_CONTEXT),
        yielder_context
    );

    // The Init_Frame() should have managed the yielder_frame varlist, which
    // means that when the yielder does Drop_Frame() yielder_context survives.
    // It should decay the keysource from a REBFRM* to the action paramlist,
    // but the next run of the yielder will swap in its new REBFRM* over that.
    //
    assert(CTX_VARLIST(yielder_context) == yielder_frame->varlist);
    ASSERT_ARRAY_MANAGED(yielder_frame->varlist);

    // We don't only write the yielded value into the output slot so it is
    // returned from the yielder.  We also stow an extra copy of the value
    // into the yielder details, which we use to make it act act as the
    // apparent return result of the YIELD when the yielder is called again.
    //
    //    x: yield 1 + 2
    //    print [x]  ; could be useful if this was 3 upon resumption, right?
    //
    Move_Value(yielder_frame->out, ARG(value));
    Move_Value(
        ARR_AT(yielder_details, IDX_YIELDER_LAST_YIELD_RESULT),
        ARG(value)
    );

    /* REBACT *target_fun = FRM_UNDERLYING(target_frame); */

    TG_Top_Frame = yielder_frame;
    return R_DEWIND;
}
