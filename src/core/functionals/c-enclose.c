//
//  File: %c-enclose.c
//  Summary: "Mechanism for making a function that wraps another's execution"
//  Section: datatypes
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2017-2020 Revolt Open Source Contributors
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
// ENCLOSE gives a fully generic ability to make a function that wraps the
// execution of another.  When the enclosure is executed, a frame is built
// for the wrapped function--but not executed.  Then that frame is passed to
// an "outer" function, which can modify the frame arguments and also operate
// upon the result:
//
//     >> add2x3x+1: enclose 'add func [f [frame!]] [
//            f/value1: f/value1 * 2
//            f/value2: f/value2 * 3
//            return 1 + do f
//         ]
//
//     >> add2x3x+1 10 20
//     == 81  ; e.g. (10 * 2) + (20 * 3) + 1
//
// This affords significant flexibility to the "outer" function, as it can
// choose when to `DO F` to execute the frame... or opt to not execute it.
// Given the mechanics of FRAME!, it's also possible to COPY the frame for
// multiple invocations.
//
//     >> print2x: enclose 'print func [f [frame!]] [
//            do copy f
//            f/value: append f/value "again!"
//            do f
//        ]
//
//     >> print2x ["Print" "me"]
//     Print me
//     Print me again!
//
// (Note: Each time you DO a FRAME!, the original frame becomes inaccessible,
// because its contents--the "varlist"--are stolen for function execution,
// where the function freely modifies the argument data while it runs.  If
// the frame did not expire, it would not be practically reusable.)
//
// ENCLOSE has the benefit of inheriting the interface of the function it
// wraps, and should perform better than trying to accomplish similar
// functionality manually.  It's still somewhat expensive, so if ADAPT or
// CHAIN can achieve a goal of simple pre-or-post processing then they may
// be better choices.
//

#include "sys-core.h"

enum {
    IDX_ENCLOSER_INNER = 0,  // The ACTION! being enclosed
    IDX_ENCLOSER_OUTER = 1,  // ACTION! that gets control of inner's FRAME!
    IDX_ENCLOSER_MAX
};


//
//  Encloser_Dispatcher: C
//
// An encloser is called with a frame that was built compatibly to invoke an
// "inner" function.  It wishes to pass this frame as an argument to an
// "outer" function, that takes only that argument.  To do this, the frame's
// varlist must thus be detached from `f` and transitioned from an "executing"
// to "non-executing" state...so that it can be used with DO.
//
REB_R Encloser_Dispatcher(REBFRM *f)
{
    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    assert(ARR_LEN(details) == IDX_ENCLOSER_MAX);

    REBVAL *inner = KNOWN(ARR_AT(details, IDX_ENCLOSER_INNER));
    assert(IS_ACTION(inner));  // takes same arguments that f was built for
    REBVAL *outer = KNOWN(ARR_AT(details, IDX_ENCLOSER_OUTER));
    assert(IS_ACTION(outer));  // takes 1 arg (f remade as a FRAME! for inner)

    assert(GET_SERIES_FLAG(f->varlist, STACK_LIFETIME));

    // We want to call OUTER with a FRAME! value that will dispatch to INNER
    // when (and if) it runs DO on it.  That frame is the one built for this
    // call to the encloser.  If it isn't managed, there's no worries about
    // user handles on it...so just take it.  Otherwise, "steal" its vars.
    //
    REBCTX *c = Steal_Context_Vars(CTX(f->varlist), NOD(FRM_PHASE(f)));
    INIT_LINK_KEYSOURCE(c, NOD(VAL_ACTION(inner)));
    CLEAR_SERIES_FLAG(c, STACK_LIFETIME);

    assert(GET_SERIES_INFO(f->varlist, INACCESSIBLE));  // look dead

    // f->varlist may or may not have wound up being managed.  It was not
    // allocated through the usual mechanisms, so if unmanaged it's not in
    // the tracking list Init_Any_Context() expects.  Just fiddle the bit.
    //
    SET_SERIES_FLAG(c, MANAGED);

    // When the DO of the FRAME! executes, we don't want it to run the
    // encloser again (infinite loop).
    //
    REBVAL *rootvar = CTX_ARCHETYPE(c);
    INIT_VAL_CONTEXT_PHASE(rootvar, VAL_ACTION(inner));
    INIT_BINDING_MAY_MANAGE(rootvar, VAL_BINDING(inner));

    // We don't actually know how long the frame we give back is going to
    // live, or who it might be given to.  And it may contain things like
    // bindings in a RETURN or a VARARGS! which are to the old varlist, which
    // may not be managed...and so when it goes off the stack it might try
    // and think that since nothing managed it then it can be freed.  Go
    // ahead and mark it managed--even though it's dead--so that returning
    // won't free it if there are outstanding references.
    //
    // Note that since varlists aren't added to the manual series list, the
    // bit must be tweaked vs. using Ensure_Array_Managed.
    //
    SET_SERIES_FLAG(f->varlist, MANAGED);

    // It's important we use EVAL_FLAG_DELEGATE_CONTROL because because we
    // have stolen the original frame--there is no longer a complete entity to
    // come back and reinvoke.
    //
    // Note: The `c` context is now detached from a frame, so nothing protects
    // it from garbage collection.  However, Push_Continuation will make a
    // copy of the "with" argument into the function's frame, so it's safe.
    //
    Push_Continuation_With_Core(
        f->out,
        f,
        EVAL_FLAG_DELEGATE_CONTROL,
        outer,
        SPECIFIED,
        rootvar  // `with` argument, see note above on copy for GC-safety
    );
    STATE_BYTE(f) = 1;  // STATE_BYTE() == 0 is reserved for initial entry
    return R_CONTINUATION;
}


//
//  enclose*: native [
//
//  {Wrap code around an ACTION! with access to its FRAME! and return value}
//
//      return: [action!]
//      inner "Action that a FRAME! will be built for, then passed to OUTER"
//          [action! word! path!]
//      outer "Gets a FRAME! for INNER before invocation, can DO it (or not)"
//          [action! word! path!]
//  ]
//
REBNATIVE(enclose_p)  // see extended definition ENCLOSE in %base-defs.r
{
    INCLUDE_PARAMS_OF_ENCLOSE_P;

    REBVAL *inner = ARG(inner);
    REBSTR *opt_inner_name;
    const bool push_refinements = false;
    if (Get_If_Word_Or_Path_Throws(
        D_OUT,
        &opt_inner_name,
        inner,
        SPECIFIED,
        push_refinements
    )){
        return R_THROWN;
    }

    if (not IS_ACTION(D_OUT))
        fail (PAR(inner));
    Move_Value(inner, D_OUT); // Frees D_OUT, and GC safe (in ARG slot)

    REBVAL *outer = ARG(outer);
    REBSTR *opt_outer_name;
    if (Get_If_Word_Or_Path_Throws(
        D_OUT,
        &opt_outer_name,
        outer,
        SPECIFIED,
        push_refinements
    )){
        return R_THROWN;
    }

    if (not IS_ACTION(D_OUT))
        fail (PAR(outer));
    Move_Value(outer, D_OUT);  // Frees D_OUT, and GC safe (in ARG slot)

    REBARR *paramlist = Copy_Array_Shallow_Flags(
        VAL_ACT_PARAMLIST(inner),  // new function same interface as `inner`
        SPECIFIED,
        SERIES_MASK_PARAMLIST | NODE_FLAG_MANAGED
    );
    Sync_Paramlist_Archetype(paramlist);  // [0] cell must hold copied pointer
    MISC_META_NODE(paramlist) = nullptr;  // defaults to being trash

    REBACT *enclosure = Make_Action(
        paramlist,
        &Encloser_Dispatcher,
        ACT_UNDERLYING(VAL_ACTION(inner)),  // same underlying as inner
        ACT_EXEMPLAR(VAL_ACTION(inner)),  // same exemplar as inner
        IDX_ENCLOSER_MAX  // details array capacity => [inner, outer]
    );

    REBARR *details = ACT_DETAILS(enclosure);
    Move_Value(ARR_AT(details, IDX_ENCLOSER_INNER), inner);
    Move_Value(ARR_AT(details, IDX_ENCLOSER_OUTER), outer);

    return Init_Action_Unbound(D_OUT, enclosure);
}
