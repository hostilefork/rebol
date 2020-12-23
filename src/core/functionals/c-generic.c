//
//  File: %c-generic.c
//  Summary: "Function that dispatches implementation based on argument types"
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
// A "generic" is what R3-Alpha/Rebol2 had called "ACTION!" (until Ren-C took
// that as the umbrella term for all "invokables").  This kind of dispatch is
// based on the first argument's type, with the idea being a single C function
// for the type has a switch() statement in it and can handle many different
// such actions for that type.
//
// (e.g. APPEND [a b c] [d] would look at the type of the first argument,
// notice it was a BLOCK!, and call the common C function for arrays with an
// append instruction--where that instruction also handles insert, length,
// etc. for BLOCK!s.)
//
// !!! This mechanism is a very primitive kind of "multiple dispatch".  Rebol
// will certainly need to borrow from other languages to develop a more
// flexible idea for user-defined types, vs. this very limited concept.
//
// https://en.wikipedia.org/wiki/Multiple_dispatch
// https://en.wikipedia.org/wiki/Generic_function
// https://stackoverflow.com/q/53574843/
//

#include "sys-core.h"

enum {
    IDX_GENERIC_VERB = 1,  // Word whose symbol is being dispatched
    IDX_GENERIC_MAX
};


//
//  Generic_Dispatcher: C
//
REB_R Generic_Dispatcher(REBFRM *f)
{
    REBACT *phase = FRM_PHASE(f);
    REBARR *details = ACT_DETAILS(phase);
    REBVAL *verb = DETAILS_AT(details, IDX_GENERIC_VERB);
    assert(IS_WORD(verb));

    // !!! It's technically possible to throw in locals or refinements at
    // any point in the sequence.  So this should really be using something
    // like a First_Unspecialized_Arg() call.  For now, we just handle the
    // case of a RETURN: sitting in the first parameter slot.
    //
    REBVAL *first_arg;
    if (ACT_HAS_RETURN(phase))
        first_arg = FRM_ARG(f, 2);
    else
        first_arg = FRM_ARG(f, 1);

    return Run_Generic_Dispatch(first_arg, f, verb);
}


//
//  generic: enfix native [
//
//  {Creates datatype action (currently for internal use only)}
//
//      return: [void!]
//      :verb [set-word!]
//      spec [block!]
//  ]
//
REBNATIVE(generic)
//
// The `generic` native is searched for explicitly by %make-natives.r and put
// in second place for initialization (after the `native` native).
//
// It is designed to be an enfix function that quotes its first argument,
// so when you write FOO: GENERIC [...], the FOO: gets quoted to be the verb.
{
    INCLUDE_PARAMS_OF_GENERIC;

    REBVAL *spec = ARG(spec);

    REBCTX *meta;
    REBFLGS flags = MKF_KEYWORDS | MKF_RETURN;
    REBARR *paramlist = Make_Paramlist_Managed_May_Fail(
        &meta,
        spec,
        &flags  // return type checked only in debug build
    );
    assert(not (flags & MKF_IS_VOIDER));
    assert(not (flags & MKF_IS_ELIDER));

    // !!! There is no system yet for extension types to register which of
    // the generic actions they can handle.  So for the moment, we just say
    // that any custom type will have its action dispatcher run--and it's
    // up to the handler to give an error if there's a problem.  This works,
    // but it limits discoverability of types in HELP.  A better answeer would
    // be able to inventory which types had registered generic dispatchers
    // and list the appropriate types from HELP.
    //
    RELVAL *param = STABLE_HACK(ARR_AT(paramlist, 1));
    if (SER(paramlist)->header.bits & PARAMLIST_FLAG_HAS_RETURN) {
        assert(VAL_PARAM_SYM(param) == SYM_RETURN);
        TYPE_SET(param, REB_CUSTOM);
        ++param;
    }
    while (VAL_PARAM_CLASS(param) != REB_P_NORMAL)
        ++param;
    TYPE_SET(param, REB_CUSTOM);

    REBACT *generic = Make_Action(
        paramlist,
        meta,
        &Generic_Dispatcher,  // return type is only checked in debug build
        nullptr,  // no specialization exemplar (or inherited exemplar)
        IDX_NATIVE_MAX  // details array capacity
    );

    SET_ACTION_FLAG(generic, IS_NATIVE);

    REBARR *details = ACT_DETAILS(generic);
    Init_Word(ARR_AT(details, IDX_NATIVE_BODY), VAL_WORD_SPELLING(ARG(verb)));
    Move_Value(ARR_AT(details, IDX_NATIVE_CONTEXT), Lib_Context);

    REBVAL *verb_var = Sink_Word_May_Fail(ARG(verb), SPECIFIED);
    Init_Action(verb_var, generic, VAL_WORD_SPELLING(ARG(verb)), UNBOUND);

    return Init_Void(D_OUT, SYM_VOID);
}
