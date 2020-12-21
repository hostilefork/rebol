//
//  File: %c-augment.c
//  Summary: "Function generator for expanding the frame of an ACTION!"
//  Section: datatypes
//  Project: "Ren-C Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2019-2020 Ren-C Open Source Contributors
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
// AUGMENT is designed to create a version of a function with an expanded
// frame, adding new parameters.  It does so without affecting the execution:
//
//     >> foo-x: func [x [integer!]] [print ["x is" x]]
//     >> foo-xy: augment :foo-x [y [integer!]]
//
//     >> foo-x 10
//     x is 10
//
//     >> foo-xy 10
//     ** Error: foo-xy is missing its y argument
//
//     >> foo-xy 10 20
//     x is 10
//
// The original function doesn't know about the added parameters, so this is
// is only useful when combined with something like ADAPT or ENCLOSE... to
// inject in phases of code at a higher level that see these parameters:
//
//     >> foo-xy: adapt (augment :foo-x [y [integer!]]) [print ["y is" y]]
//
//     >> foo-xy 10 20
//     y is 20
//     x is 10
//
// AUGMENT leverages Ren-C's concept of "refinements are their own arguments"
// in order to allow normal parameters to be added to the frame after a
// refinement already has appeared.
//

#include "sys-core.h"

enum {
    IDX_AUGMENTER_AUGMENTEE = 1,  // Function with briefer frame to dispatch
    IDX_AUGMENTER_MAX
};


//
//  Augmenter_Dispatcher: C
//
// It might seem an augmentation can just run the underlying frame directly,
// but it needs to switch phases in order to get the frame to report the
// more limited set of fields that are in effect when the frame runs.  So it
// does a cheap switch of phase, and a redo without needing new type checking.
//
REB_R Augmenter_Dispatcher(REBFRM *f)
{
    REBACT *phase = FRM_PHASE(f);
    REBARR *details = ACT_DETAILS(phase);
    assert(ARR_LEN(details) == IDX_AUGMENTER_MAX);

    REBVAL *augmentee = DETAILS_AT(details, IDX_AUGMENTER_AUGMENTEE);

    INIT_FRM_PHASE(f, VAL_ACTION(augmentee));
    FRM_BINDING(f) = NOD(VAL_ACTION_BINDING(augmentee));

    return R_REDO_UNCHECKED;  // signatures should match
}


//
//  augment*: native [
//
//  {Create an ACTION! variant that acts the same, but has added parameters}
//
//      return: [action!]
//      augmentee "Function whose implementation is to be augmented"
//          [action!]
//      spec "Spec dialect for words to add to the derived function"
//          [block!]
//  ]
//
REBNATIVE(augment_p)  // see extended definition AUGMENT in %base-defs.r
{
    INCLUDE_PARAMS_OF_AUGMENT_P;

    REBACT *augmentee = VAL_ACTION(ARG(augmentee));

    // We reuse the process from Make_Paramlist_Managed_May_Fail(), which
    // pushes parameters to the stack in groups of three items per parameter.

    REBDSP dsp_orig = DSP;
    REBDSP definitional_return_dsp = 0;

    // Start with pushing a cell for the special [0] slot
    //
    Init_Unreadable_Void(DS_PUSH());  // paramlist[0] becomes ACT_ARCHETYPE()
    Move_Value(DS_PUSH(), EMPTY_BLOCK);  // param_types[0] (object canon)
    Move_Value(DS_PUSH(), EMPTY_TEXT);  // param_notes[0] (desc, then canon)

    REBFLGS flags = MKF_KEYWORDS;
    if (ACT_HAS_RETURN(augmentee)) {
        flags |= MKF_RETURN;
        definitional_return_dsp = DSP + 1;
    }

    // For each parameter in the original function, we push a corresponding
    // "triad".
    //
    REBVAL *param = ACT_PARAMS_HEAD(augmentee);
    for (; NOT_END(param); ++param) {
        Move_Value(DS_PUSH(), param);
        if (Is_Param_Hidden(param, param))  // special = param
            Seal_Param(DS_TOP);
        Move_Value(DS_PUSH(), EMPTY_BLOCK);
        Move_Value(DS_PUSH(), EMPTY_TEXT);
    }

    // Now we reuse the spec analysis logic, which pushes more parameters to
    // the stack.  This may add duplicates--which will be detected when we
    // try to pop the stack into a paramlist.
    //
    assert(flags & MKF_RETURN);
    Push_Paramlist_Triads_May_Fail(
        ARG(spec),
        &flags,
        dsp_orig,
        &definitional_return_dsp
    );

    REBCTX *meta;
    REBARR *paramlist = Pop_Paramlist_With_Meta_May_Fail(
        &meta,
        dsp_orig,
        flags,
        definitional_return_dsp
    );

    // Keep track that this derived paramlist is related to the original, so
    // that it's possible to tell a frame built for the augmented function is
    // compatible with the original function (and its ancestors, too)
    //
    LINK_ANCESTOR_NODE(paramlist) = NOD(paramlist);

    // We have to inject a simple dispatcher to flip the phase to one that has
    // the more limited frame.  But we have to make an expanded exemplar if
    // there is one.  (We can't expand the existing exemplar, because more
    // than one AUGMENT might happen to the same function).  :-/

    REBCTX *old_exemplar = ACT_EXEMPLAR(augmentee);
    REBCTX *exemplar;
    if (not old_exemplar)
        exemplar = nullptr;
    else {
        REBLEN old_len = ARR_LEN(ACT_PARAMLIST(augmentee));
        REBLEN delta = ARR_LEN(paramlist) - old_len;
        assert(delta > 0);

        REBARR *old_varlist = CTX_VARLIST(old_exemplar);
        assert(ARR_LEN(old_varlist) == old_len);

        REBARR *varlist = Copy_Array_At_Extra_Shallow(
            old_varlist,
            0,  // index
            SPECIFIED,
            delta,  // extra cells
            SER(old_varlist)->header.bits
        );
        SER(varlist)->info.bits = SER(old_varlist)->info.bits;
        INIT_VAL_CONTEXT_VARLIST(ARR_HEAD(varlist), varlist);

        // We fill in the added parameters in the specialization as undefined
        // starters.  This is considered to be "unspecialized".
        //
        blockscope {
            unstable RELVAL *temp = ARR_AT(varlist, old_len);
            REBLEN i;
            for (i = 0; i < delta; ++i) {
                Init_Void(temp, SYM_UNSET);
                temp = temp + 1;
            }
            TERM_ARRAY_LEN(varlist, ARR_LEN(paramlist));
        }

        // !!! Inefficient, we need to keep the ARG_MARKED_CHECKED bit, but
        // copying won't keep it by default!  Review folding this into the
        // copy machinery as part of the stackless copy implementation.  Done
        // poorly here alongside the copy that should be parameterized.
        //
        blockscope {
            unstable RELVAL *src = ARR_HEAD(old_varlist) + 1;
            unstable RELVAL *dest = ARR_HEAD(varlist) + 1;
            REBLEN i;
            for (i = 1; i < old_len; ++i, ++src, ++dest) {
                if (GET_CELL_FLAG(src, ARG_MARKED_CHECKED))
                    SET_CELL_FLAG(dest, ARG_MARKED_CHECKED);
            }
        }

        MISC_META_NODE(varlist) = nullptr;  // GC sees, must initialize
        exemplar = CTX(varlist);
        INIT_CTX_KEYLIST_SHARED(exemplar, paramlist);
    }

    REBACT* augmentated = Make_Action(
        paramlist,
        meta,
        &Augmenter_Dispatcher,
        exemplar,
        IDX_AUGMENTER_MAX  // size of the ACT_DETAILS array
    );

    Move_Value(
        ARR_AT(ACT_DETAILS(augmentated), IDX_AUGMENTER_AUGMENTEE),
        ARG(augmentee)
    );

    return Init_Action(
        D_OUT,
        augmentated,
        VAL_ACTION_LABEL(ARG(augmentee)),
        UNBOUND
    );
}
