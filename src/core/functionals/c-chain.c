//
//  File: %c-chain.c
//  Summary: "Function generator for making a pipeline of post-processing"
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
// CHAIN is used to create a function that matches the interface of a "first"
// function, and then pipes its results through to several subsequent
// post-processing actions:
//
//     >> negadd: chain [:add | :negate]
//
//     >> negadd 2 2
//     == -4
//
// Its design is relatively efficient, because it is supported by the action
// executor directly--which looks for ACTION!s pushed to the stack by
// dispatchers and assumes that means the calls should be chained.
//
// For more complex post-processing which may involve access to the original
// inputs to the first function (or other memory in the process), consider
// using ENCLOSE...which is less efficient, but more powerful.
//
// !!! CHAIN is one of the oldest function derivations, and has not been
// revisited much in its design--e.g. to support multiple return values.
//

#include "sys-core.h"

enum {
    IDX_CHAINER_PIPELINE = 0,  // Chain of functions to execute
    IDX_CHAINER_MAX
};


//
//  Chainer_Dispatcher: C
//
// When a function created with CHAIN is invoked, this dispatcher is run.
//
static REB_R Chainer_Dispatcher(REBFRM *f)
{
    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    assert(ARR_LEN(details) == IDX_CHAINER_MAX);

    REBARR *pipeline = VAL_ARRAY(ARR_AT(details, IDX_CHAINER_PIPELINE));

    // The post-processing pipeline has to be "pushed" so it is not forgotten.
    // Go in reverse order, so the function to apply last is at the bottom of
    // the stack.
    //
    REBVAL *chained = KNOWN(ARR_LAST(pipeline));
    for (; chained != ARR_HEAD(pipeline); --chained) {
        assert(IS_ACTION(chained));
        Move_Value(DS_PUSH(), KNOWN(chained));
    }

    // Extract the first function, itself which might be a chain.
    //
    INIT_FRM_PHASE(f, VAL_ACTION(chained));
    FRM_BINDING(f) = VAL_BINDING(chained);

    return R_REDO_UNCHECKED;  // signatures should match
}


//
//  chain*: native [
//
//  {Create a processing pipeline of actions, each consuming the last result}
//
//      return: [action!]
//      pipeline "Block of ACTION!s to apply (will be LOCKed)"
//          [block!]
//  ]
//
REBNATIVE(chain_p)  // see extended definition CHAIN in %base-defs.r
{
    INCLUDE_PARAMS_OF_CHAIN_P;

    REBVAL *out = D_OUT;  // plan ahead for factoring into Chain_Action(out..

    REBVAL *pipeline = ARG(pipeline);
    REBVAL *first = KNOWN(VAL_ARRAY_AT(pipeline));

    // !!! Current validation is that all are actions.  Should there be other
    // checks?  (That inputs match outputs in the chain?)  Should it be
    // a dialect and allow things other than functions?
    //
    REBVAL *check = first;
    while (NOT_END(check)) {
        if (not IS_ACTION(check))
            fail (check);
        ++check;
    }

    REBARR *paramlist = Copy_Array_Shallow_Flags(
        VAL_ACT_PARAMLIST(first),  // same interface as head of the chain
        SPECIFIED,
        SERIES_MASK_PARAMLIST | NODE_FLAG_MANAGED  // flags not auto-copied
    );
    Sync_Paramlist_Archetype(paramlist);  // [0] cell must hold copied pointer
    MISC_META_NODE(paramlist) = nullptr;  // defaults to being trash

    REBACT *chain = Make_Action(
        paramlist,
        &Chainer_Dispatcher,
        ACT_UNDERLYING(VAL_ACTION(first)),  // same underlying as first action
        ACT_EXEMPLAR(VAL_ACTION(first)),  // same exemplar as first action
        IDX_CHAINER_MAX  // details array capacity
    );
    Deep_Freeze_Array(VAL_ARRAY(pipeline));
    Move_Value(ARR_AT(ACT_DETAILS(chain), IDX_CHAINER_PIPELINE), pipeline);

    return Init_Action_Unbound(out, chain);
}
