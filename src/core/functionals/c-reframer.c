//
//  File: %c-reframer.c
//  Summary: "Function that can transform arbitrary callsite functions"
//  Section: datatypes
//  Project: "Ren-C Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2020 Ren-C Open Source Contributors
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
// REFRAMER allows one to define a function that does generalized transforms
// on the input (and output) of other functions.  Unlike ENCLOSE, it does not
// specify an exact function it does surgery on the frame of ahead of time.
// Instead, each invocation of the reframing action interacts with the
// instance that follows it at the callsite.
//
// A simple example is a function which removes quotes from the first
// parameter to a function, and adds them back for the result:
//
//     requote: reframer func [f [frame!]] [
//         p: first words of f
//         num-quotes: quotes of f/(p)
//
//         f/(p): dequote f/(p)
//
//         return quote/depth do f num-quotes
//     ]
//
//     >> item: first ['''[a b c]]
//
//     >> requote append item <d>  ; append doesn't accept QUOTED! items
//     == '''[a b c <d>]   ; munging frame and result makes it seem to
//
// !!! Due to the way that REFRAMER works today, it cannot support a chain
// of reframers.  e.g. with MY implemented as a reframer, you couldn't say:
//
//     >> item: my requote append <d>
//
// Being able to do so would require some kind of "compound frame" that could
// allow MY to push through REQUOTE to see APPEND's arguments.  This sounds
// technically difficult, though perhaps pared down versions could be made
// in the near term (e.g. in cases like this, where the reframer takes no
// arguments of its own)
//

#include "sys-core.h"

enum {
    IDX_REFRAMER_SHIM = 1,  // action that can manipulate the reframed frame
    IDX_REFRAMER_PARAM_INDEX,  // index in shim that receives FRAME!
    IDX_REFRAMER_MAX
};


// The REFRAMER native specializes out the FRAME! argument of the function
// being modified when it builds the interface.
//
// So the next thing to do is to fulfill the next function's frame without
// running it, in order to build a frame to put into that specialized slot.
// Then we run the reframer.
//
// !!! As a first cut we build on top of specialize, and look for the
// parameter by means of a particular labeled void.
//
REB_R Reframer_Dispatcher(REBFRM *f)
{
    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    assert(ARR_LEN(details) == IDX_REFRAMER_MAX);

    REBVAL* shim = DETAILS_AT(details, IDX_REFRAMER_SHIM);
    assert(IS_ACTION(shim));

    REBVAL* param_index = DETAILS_AT(details, IDX_REFRAMER_PARAM_INDEX);
    assert(IS_INTEGER(param_index));

    if (IS_END(f_value) or not (IS_WORD(f_value) or IS_PATH(f_value)))
        fail ("REFRAMER can only currently run on subsequent WORD!/PATH!");

    // First run ahead and make the frame we want from the feed.  We push
    // the frame so that we can fold the refinements used into it, without
    // needing to create an intermediate specialized function in the process.
    //
    // Note: We do not overwrite f->out in case of invisibility.

    DECLARE_FRAME (sub, f->feed, EVAL_MASK_DEFAULT);
    Push_Frame(f_spare, sub);

    if (Get_If_Word_Or_Path_Throws(
        sub->out,  // e.g. f_spare
        f_value,
        f_specifier,
        true  // push_refinements = true (DECLARE_FRAME captured original DSP)
    )){
        Drop_Frame(sub);
        return R_THROWN;
    }

    if (not IS_ACTION(sub->out))
        fail (rebUnrelativize(f_value));

    Fetch_Next_Forget_Lookback(sub);  // now, onto the arguments...

    const REBSTR *label = VAL_ACTION_LABEL(sub->out);

    DECLARE_LOCAL (action);
    Move_Value(action, sub->out);
    PUSH_GC_GUARD(action);

    REBVAL *first_arg;
    if (Make_Invocation_Frame_Throws(sub, &first_arg, action)) {
        DROP_GC_GUARD(action);
        return R_THROWN;
    }

    UNUSED(first_arg); // MATCH uses to get its answer faster, we don't need

    REBACT *act = VAL_ACTION(action);

    assert(NOT_SERIES_FLAG(sub->varlist, MANAGED)); // not invoked yet
    assert(FRM_BINDING(sub) == VAL_ACTION_BINDING(action));

    REBCTX *stolen = Steal_Context_Vars(
        CTX(sub->varlist),
        NOD(ACT_PARAMLIST(act))
    );
    assert(ACT_NUM_PARAMS(act) == CTX_LEN(stolen));

    INIT_LINK_KEYSOURCE(CTX_VARLIST(stolen), NOD(ACT_PARAMLIST(act)));

    SET_SERIES_FLAG(sub->varlist, MANAGED); // is inaccessible
    sub->varlist = nullptr; // just let it GC, for now

    // May not be at end or thrown, e.g. (x: does lit y x = 'y)
    //
    DROP_GC_GUARD(action);  // before drop to balance at right time
    Drop_Frame(sub);

    // The exemplar may or may not be managed as of yet.  We want it
    // managed, but Push_Action() does not use ordinary series creation to
    // make its nodes, so manual ones don't wind up in the tracking list.
    //
    SET_SERIES_FLAG(stolen, MANAGED); // can't use Manage_Series

    REBVAL *arg = FRM_ARGS_HEAD(f) + VAL_INT32(param_index);
    Init_Frame(arg, stolen, label);

    INIT_FRM_PHASE(f, VAL_ACTION(shim));
    INIT_FRM_BINDING(f, VAL_ACTION_BINDING(shim));

    return R_REDO_CHECKED;  // the redo will use the updated phase & binding
}


//
//  reframer*: native [
//
//  {Make a function that manipulate other actions at the callsite}
//
//      return: [action!]
//      shim "The action that has a FRAME! argument to supply"
//          [action!]
//      /parameter "Which parameter of the shim gets given the FRAME!"
//          [word!]
//  ]
//
REBNATIVE(reframer_p)
{
    INCLUDE_PARAMS_OF_REFRAMER_P;

    REBVAL *shim = ARG(shim);

    // Make action as a copy of the input function, with enough space to
    // store the implementation phase and which parameter to fill with the
    // frame.
    //
    REBARR *paramlist = VAL_ACTION_PARAMLIST(shim);

    REBACT *reframer = Make_Action(
        paramlist,
        nullptr,  // meta inherited by REFRAMER helper to REFRAMER*
        &Reframer_Dispatcher,
        ACT_EXEMPLAR(VAL_ACTION(shim)),  // same exemplar as shim
        IDX_REFRAMER_MAX  // details array capacity => [shim, param_index]
    );

    // Find the parameter they want to overwrite with the frame, or default
    // to the last unspecialized parameter.
    //
    REBVAL *param = nullptr;
    if (REF(parameter)) {
        const REBSTR *canon = VAL_WORD_SPELLING(REF(parameter));
        REBVAL *temp = ACT_PARAMS_HEAD(reframer);
        for (; NOT_END(temp); ++temp) {
            if (VAL_PARAM_SPELLING(temp) == canon) {
                param = temp;
                break;
            }
        }
    }
    else {  // default parameter to the last unspecialized one
        param = Last_Unspecialized_Param(reframer);
    }

    // Set the type bits so that we can get the dispatcher to the point of
    // running even though we haven't filled in a frame yet.
    //
    if (not param)
        fail ("Could not find parameter for REFRAMER");
    TYPE_SET(param, REB_VOID);
    Hide_Param(param);

    REBLEN param_index = param - ACT_PARAMS_HEAD(reframer);

    REBARR *details = ACT_DETAILS(reframer);
    Move_Value(ARR_AT(details, IDX_REFRAMER_SHIM), shim);
    Init_Integer(ARR_AT(details, IDX_REFRAMER_PARAM_INDEX), param_index);

    return Init_Action(D_OUT, reframer, VAL_ACTION_LABEL(shim), UNBOUND);
}
