//
//  File: %c-native.c
//  Summary: "Function that executes implementation as native code"
//  Section: datatypes
//  Project: "Ren-C Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2020 Ren-C Open Source Contributors
// REBOL is a trademark of REBOL Technologies
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
// A native is unique from other function types, because instead of there
// being a "Native_Dispatcher()", each native has a C function that acts as
// its dispatcher.
//
// Also unique about natives is that the native function constructor must be
// built "by hand", since it is required to get the ball rolling on having
// functions to call at all.
//
// If there *were* a REBNATIVE(native) this would be its spec:
//
//  native: native [
//      spec [block!]
//      /body "Body of equivalent usermode code (for documentation)}
//          [block!]
//  ]
//

#include "sys-core.h"


//
//  Make_Native: C
//
// Reused function in Startup_Natives() as well as extensions loading natives,
// which can be parameterized with a different context in which to look up
// bindings by deafault in the API when that native is on the stack.
//
// Each entry should be one of these forms:
//
//    some-name: native [spec content]
//
//    some-name: native/body [spec content] [equivalent user code]
//
// It is optional to put ENFIX between the SET-WORD! and the spec.
//
// If more refinements are added, this will have to get more sophisticated.
//
// Though the manual building of this table is not as "nice" as running the
// evaluator, the evaluator makes comparisons against native values.  Having
// all natives loaded fully before ever running Eval_Core() helps with
// stability and invariants...also there's "state" in keeping track of which
// native index is being loaded, which is non-obvious.  But these issues
// could be addressed (e.g. by passing the native index number / DLL in).
//
REBVAL *Make_Native(
    RELVAL **item, // the item will be advanced as necessary
    REBSPC *specifier,
    REBNAT dispatcher,
    const REBVAL *module
){
    assert(specifier == SPECIFIED); // currently a requirement

    // Get the name the native will be started at with in Lib_Context
    //
    if (not IS_SET_WORD(*item))
        panic (*item);

    REBVAL *name = SPECIFIC(*item);
    ++*item;

    bool enfix;
    if (IS_WORD(*item) and VAL_WORD_SYM(*item) == SYM_ENFIX) {
        enfix = true;
        ++*item;
    }
    else
        enfix = false;

    // See if it's being invoked with NATIVE or NATIVE/BODY
    //
    bool has_body;
    if (IS_WORD(*item)) {
        if (VAL_WORD_SYM(*item) != SYM_NATIVE)
            panic (*item);
        has_body = false;
    }
    else {
        DECLARE_LOCAL (temp);
        if (
            VAL_WORD_SYM(VAL_SEQUENCE_AT(temp, *item, 0)) != SYM_NATIVE
            or VAL_WORD_SYM(VAL_SEQUENCE_AT(temp, *item, 1)) != SYM_BODY
        ){
            panic (*item);
        }
        has_body = true;
    }
    ++*item;

    const REBVAL *spec = SPECIFIC(*item);
    ++*item;
    if (not IS_BLOCK(spec))
        panic (spec);

    // With the components extracted, generate the native and add it to
    // the Natives table.  The associated C function is provided by a
    // table built in the bootstrap scripts, `Native_C_Funcs`.

    REBCTX *meta;
    REBFLGS flags = MKF_KEYWORDS | MKF_RETURN;
    REBARR *paramlist = Make_Paramlist_Managed_May_Fail(
        &meta,
        spec,
        &flags  // return type checked only in debug build
    );

    // Natives are their own dispatchers; there is no point of interjection
    // to force their outputs to anything but what they return.  Instead of
    // `return: <void>` use `return: [void!]` and `return Init_Void(D_OUT);`
    // And instead of `return: <elide>` use `return: [<invisible>]` along
    // with `return D_OUT;`...having made no modifications to D_OUT.
    //
    assert(not (flags & MKF_IS_VOIDER));
    assert(not (flags & MKF_IS_ELIDER));

    REBACT *act = Make_Action(
        paramlist,
        meta,
        dispatcher, // "dispatcher" is unique to this "native"
        nullptr, // no specialization exemplar (or inherited exemplar)
        IDX_NATIVE_MAX // details array capacity
    );

    SET_ACTION_FLAG(act, IS_NATIVE);
    if (enfix)
        SET_ACTION_FLAG(act, ENFIXED);

    REBARR *details = ACT_DETAILS(act);

    // If a user-equivalent body was provided, we save it in the native's
    // REBVAL for later lookup.
    //
    if (has_body) {
        if (not IS_BLOCK(*item))
            panic (*item);

        Derelativize(ARR_AT(details, IDX_NATIVE_BODY), *item, specifier);
        ++*item;
    }
    else
        Init_Blank(ARR_AT(details, IDX_NATIVE_BODY));

    // When code in the core calls APIs like `rebValue()`, it consults the
    // stack and looks to see where the native function that is running
    // says its "module" is.  Core natives default to Lib_Context.
    //
    Move_Value(ARR_AT(details, IDX_NATIVE_CONTEXT), module);

    // Append the native to the module under the name given.
    //
    REBVAL *var = Append_Context(VAL_CONTEXT(module), name, nullptr);
    Init_Action(var, act, VAL_WORD_SPELLING(name), UNBOUND);

    return var;
}
