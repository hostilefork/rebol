//
//  File: %c-typechecker.c
//  Summary: "Function generator for an optimized typechecker"
//  Section: datatypes
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2016-2020 Revolt Open Source Contributors
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
// Making a typechecker is very easy:
//
//     >> integer?: func [v [any-value!]] [integer! = type of :v]
//
//     >> integer? 10
//     == #[true]
//
//     >> integer? <foo>
//     == #[false]
//
// But given that it is done so often, it's more efficient to have a custom
// dispatcher for making a typechecker:
//
//     >> integer?: typechecker integer!
//
// This makes a near-native optimized version of the type checker which uses
// a custom dispatcher.  It works for both datatypes and typesets.
//

#include "sys-core.h"

enum {
    IDX_TYPECHECKER_TYPE = 0,  // datatype or typeset to check
    IDX_TYPECHECKER_MAX
};


//
//  Datatype_Checker_Dispatcher: C
//
// Dispatcher used by TYPECHECKER generator for when argument is a datatype.
//
REB_R Datatype_Checker_Dispatcher(REBFRM *f)
{
    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    assert(ARR_LEN(details) == IDX_TYPECHECKER_MAX);

    RELVAL *datatype = ARR_AT(details, IDX_TYPECHECKER_TYPE);

    if (VAL_TYPE_KIND_OR_CUSTOM(datatype) == REB_CUSTOM) {
        if (VAL_TYPE(FRM_ARG(f, 1)) != REB_CUSTOM)
            return Init_False(f->out);

        REBTYP *typ = VAL_TYPE_CUSTOM(datatype);
        return Init_Logic(
            f->out,
            CELL_CUSTOM_TYPE(FRM_ARG(f, 1)) == typ
        );
    }

    return Init_Logic(  // otherwise won't be equal to any custom type
        f->out,
        VAL_TYPE(FRM_ARG(f, 1)) == VAL_TYPE_KIND_OR_CUSTOM(datatype)
    );
}


//
//  Typeset_Checker_Dispatcher: C
//
// Dispatcher used by TYPECHECKER generator for when argument is a typeset.
//
REB_R Typeset_Checker_Dispatcher(REBFRM *f)
{
    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    assert(ARR_LEN(details) == IDX_TYPECHECKER_MAX);

    RELVAL *typeset = ARR_AT(details, IDX_TYPECHECKER_TYPE);
    assert(IS_TYPESET(typeset));

    return Init_Logic(f->out, TYPE_CHECK(typeset, VAL_TYPE(FRM_ARG(f, 1))));
}


//
//  typechecker: native [
//
//  {Generator for an optimized typechecking ACTION!}
//
//      return: [action!]
//      type [datatype! typeset!]
//  ]
//
REBNATIVE(typechecker)
{
    INCLUDE_PARAMS_OF_TYPECHECKER;

    REBVAL *type = ARG(type);

    REBARR *paramlist = Make_Array_Core(
        2,
        SERIES_MASK_PARAMLIST | NODE_FLAG_MANAGED
    );

    REBVAL *archetype = RESET_CELL(
        Alloc_Tail_Array(paramlist),
        REB_ACTION,
        CELL_MASK_ACTION
    );
    VAL_ACT_PARAMLIST_NODE(archetype) = NOD(paramlist);
    INIT_BINDING(archetype, UNBOUND);

    Init_Param(
        Alloc_Tail_Array(paramlist),
        REB_P_NORMAL,
        Canon(SYM_VALUE),
        TS_OPT_VALUE  // Allow null (e.g. <opt>), returns false
    );

    MISC_META_NODE(paramlist) = nullptr;  // !!! auto-generate info for HELP?

    REBACT *typechecker = Make_Action(
        paramlist,
        IS_DATATYPE(type)
            ? &Datatype_Checker_Dispatcher
            : &Typeset_Checker_Dispatcher,
        nullptr,  // no underlying action (use paramlist)
        nullptr,  // no specialization exemplar (or inherited exemplar)
        IDX_TYPECHECKER_MAX  // details array capacity
    );
    Move_Value(ARR_AT(ACT_DETAILS(typechecker), IDX_TYPECHECKER_TYPE), type);

    return Init_Action_Unbound(D_OUT, typechecker);
}
