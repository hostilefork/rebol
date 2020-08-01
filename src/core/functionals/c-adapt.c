//
//  File: %c-adapt.c
//  Summary: "Function generator injecting code block before running another"
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
// The ADAPT operation is an efficient way to create a variation of a function
// that does some amount of pre-processing (which can include modifying the
// arguments), before the original implementation is called:
//
//     >> ap1: adapt 'append [if integer? :value [value: value + 716]]
//
//     >> ap1 [a b c] 304
//     == [a b c 1020]
//
// What makes it efficient is that the adapted function operates on the same
// function frame as what it is adapting.  It does--however--need to run a
// type check on any modified arguments before passing control to the original
// "adaptee", as failure to do so could pass bad bit patterns to natives
// and lead to crashes.
//
//    >> negbad: adapt 'negate [number: to text! number]
//
//    >> negbad 1020
//    ** Error: Internal phase disallows TEXT! for its `number` argument
//
// More complete control of execution and manipulating the return result is
// possible with the ENCLOSE operation, but at a greater performance cost.
//

#include "sys-core.h"

enum {
    IDX_ADAPTER_PRELUDE = 0,  // Relativized block to run before Adaptee
    IDX_ADAPTER_ADAPTEE = 1,  // The ACTION! being adapted
    IDX_ADAPTER_MAX
};


//
//  Adapter_Dispatcher: C
//
// Each time a function created with ADAPT is executed, this code runs to
// invoke the "prelude" before passing control to the "adaptee" function.
//
REB_R Adapter_Dispatcher(REBFRM *f)
{
    enum {
        ST_ADAPTER_INITIAL_ENTRY = 0,
        ST_ADAPTER_RUNNING_PRELUDE
    };

    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    assert(ARR_LEN(details) == IDX_ADAPTER_MAX);

    switch (STATE_BYTE(f)) {
      case ST_ADAPTER_INITIAL_ENTRY: goto initial_entry;
      case ST_ADAPTER_RUNNING_PRELUDE: goto prelude_finished;
      default: assert(false);
    }

  initial_entry: {
    //
    // !!! Note: If the prelude throws--including a RETURN--that means the
    // adaptee will not be run.
    //
    assert(IS_BLOCK(ARR_AT(details, IDX_ADAPTER_PRELUDE)));
    assert(IDX_ADAPTER_PRELUDE == 0);
    Push_Continuation_Details_0(f->out, f);  // we run ACT_DETAILS[0]
    STATE_BYTE(f) = ST_ADAPTER_RUNNING_PRELUDE;
    return R_CONTINUATION;
  }

  prelude_finished: {
    //
    // Perform a "REDO_CHECKED" which is like a continuation that makes
    // sure none of the frame cells were given types that would potentially
    // confuse the adapted function (which might be native and could crash
    // if it thought there was typechecking but got unexpected bits).

    REBVAL* adaptee = KNOWN(ARR_AT(details, IDX_ADAPTER_ADAPTEE));
    INIT_FRM_PHASE(f, VAL_ACTION(adaptee));
    FRM_BINDING(f) = VAL_BINDING(adaptee);

    return R_REDO_CHECKED;  // the redo will use the updated phase & binding
  }
}


//
//  adapt*: native [
//
//  {Create a variant of an ACTION! that preprocesses its arguments}
//
//      return: [action!]
//      adaptee "Function or specifying word (preserves word for debug info)"
//          [action! word! path!]
//      prelude "Code to run in constructed frame before adaptee runs"
//          [block!]
//  ]
//
REBNATIVE(adapt_p)  // see extended definition ADAPT in %base-defs.r
{
    INCLUDE_PARAMS_OF_ADAPT_P;

    REBVAL *adaptee = ARG(adaptee);

    REBSTR *opt_adaptee_name;
    const bool push_refinements = false;
    if (Get_If_Word_Or_Path_Throws(
        D_OUT,
        &opt_adaptee_name,
        adaptee,
        SPECIFIED,
        push_refinements
    )){
        return R_THROWN;
    }

    if (not IS_ACTION(D_OUT))
        fail (PAR(adaptee));
    Move_Value(adaptee, D_OUT);  // Frees D_OUT, and GC safe (in ARG slot)

    REBARR *paramlist = Copy_Array_Shallow_Flags(
        VAL_ACT_PARAMLIST(adaptee),  // same interface as head of pipeline
        SPECIFIED,
        SERIES_MASK_PARAMLIST
            | (SER(VAL_ACTION(adaptee))->header.bits & PARAMLIST_MASK_INHERIT)
            | NODE_FLAG_MANAGED
    );
    Sync_Paramlist_Archetype(paramlist);  // [0] cell must hold copied pointer
    MISC_META_NODE(paramlist) = nullptr;  // defaults to being trash

    REBACT *underlying = ACT_UNDERLYING(VAL_ACTION(adaptee));

    REBACT *adaptation = Make_Action(
        paramlist,
        &Adapter_Dispatcher,
        underlying,  // same underlying as adaptee
        ACT_EXEMPLAR(VAL_ACTION(adaptee)),  // same exemplar as adaptee
        IDX_ADAPTER_MAX  // details array capacity => [prelude, adaptee]
    );

    // !!! In a future branch it may be possible that specific binding allows
    // a read-only input to be "viewed" with a relative binding, and no copy
    // would need be made if input was R/O.  For now, we copy to relativize.
    //
    REBARR *prelude = Copy_And_Bind_Relative_Deep_Managed(
        ARG(prelude),
        paramlist,
        TS_WORD,
        false  // do not gather LETs
    );

    // We can't use a simple Init_Block() here, because the prelude has been
    // relativized.  It is thus not a REBVAL*, but a RELVAL*...so the
    // Adapter_Dispatcher() must combine it with the FRAME! instance before
    // it can be executed (e.g. the `REBFRM *f` it is dispatching).
    //
    REBARR *details = ACT_DETAILS(adaptation);
    Init_Relative_Block(
        ARR_AT(details, IDX_ADAPTER_PRELUDE),
        underlying,
        prelude
    );
    Move_Value(ARR_AT(details, IDX_ADAPTER_ADAPTEE), adaptee);

    return Init_Action_Unbound(D_OUT, adaptation);
}
