//
//  File: %sys-rebact.h
//  Summary: "action! defs BEFORE %tmp-internals.h"
//  Section: core
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012-2020 Ren-C Open Source Contributors
// Copyright 2012 REBOL Technologies
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
// See %sys-action.h for information about the workings of REBACT and ACTION!.
// This file just defines basic structures and flags.
//


struct Reb_Action {
    struct Reb_Array details;
};


#define LINK_ANCESTOR_NODE(keylist_or_paramlist) \
    LINK(keylist_or_paramlist).custom.node

#define MISC_META_NODE(varlist_or_details)  \
    MISC(varlist_or_details).custom.node

// Note: LINK on details is the DISPATCHER, on varlists it's KEYSOURCE


//=//// PARAMLIST_FLAG_HAS_RETURN /////////////////////////////////////////=//
//
// See ACT_HAS_RETURN() for remarks.  Note: This is a flag on PARAMLIST, not
// on DETAILS.
//
#define PARAMLIST_FLAG_HAS_RETURN \
    ARRAY_FLAG_23


//=//// DETAILS_FLAG_POSTPONES_ENTIRELY ///////////////////////////////////=//
//
// A postponing operator causes everything on its left to run before it will.
// Like a deferring operator, it is only allowed to appear after the last
// parameter of an expression except it closes out *all* the parameters on
// the stack vs. just one.
//
#define DETAILS_FLAG_POSTPONES_ENTIRELY \
    ARRAY_FLAG_24


//=//// DETAILS_FLAG_IS_BARRIER ///////////////////////////////////////////=//
//
// Special action property set with TWEAK.  Used by |
//
// The "expression barrier" was once a built-in type (BAR!) in order to get
// a property not possible to achieve with functions...that it would error
// if it was used during FULFILL_ARG and would be transparent in evaluation.
//
// Transparency was eventually generalized as "invisibility".  But attempts
// to intuit the barrier-ness from another property (e.g. "enfix but no args")
// were confusing.  It seems an orthogonal feature in its own right, so it
// was added to the TWEAK list pending a notation in function specs.
//
#define DETAILS_FLAG_IS_BARRIER \
    ARRAY_FLAG_25


//=//// DETAILS_FLAG_DEFERS_LOOKBACK //////////////////////////////////////=//
//
// Special action property set with TWEAK.  Used by THEN, ELSE, and ALSO.
//
// Tells you whether a function defers its first real argument when used as a
// lookback.  Because lookback dispatches cannot use refinements, the answer
// is always the same for invocation via a plain word.
//
#define DETAILS_FLAG_DEFERS_LOOKBACK \
    ARRAY_FLAG_26


//=//// DETAILS_FLAG_QUOTES_FIRST /////////////////////////////////////////=//
//
// This is a calculated property, which is cached by Make_Action().
//
// This is another cached property, needed because lookahead/lookback is done
// so frequently, and it's quicker to check a bit on the function than to
// walk the parameter list every time that function is called.
//
#define DETAILS_FLAG_QUOTES_FIRST \
    ARRAY_FLAG_27


//=//// DETAILS_FLAG_SKIPPABLE_FIRST //////////////////////////////////////=//
//
// This is a calculated property, which is cached by Make_Action().
//
// It is good for the evaluator to have a fast test for knowing if the first
// argument to a function is willing to be skipped, as this comes into play
// in quote resolution.  (It's why `x: default [10]` can have default looking
// for SET-WORD! and SET-PATH! to its left, but `case [... default [x]]` can
// work too when it doesn't see a SET-WORD! or SET-PATH! to the left.)
//
#define DETAILS_FLAG_SKIPPABLE_FIRST \
    ARRAY_FLAG_28


//=//// DETAILS_FLAG_IS_NATIVE ////////////////////////////////////////////=//
//
// Native functions are flagged that their dispatcher represents a native in
// order to say that their ACT_DETAILS() follow the protocol that the [0]
// slot is "equivalent source" (may be a TEXT!, as in user natives, or a
// BLOCK!).  The [1] slot is a module or other context into which APIs like
// rebValue() etc. should consider for binding, in addition to lib.  A BLANK!
// in the 1 slot means no additional consideration...bind to lib only.
//
// Note: This is tactially set to be the same as SERIES_INFO_HOLD to make it
// possible to branchlessly mask in the bit to stop frames from being mutable
// by user code once native code starts running.
//
#define DETAILS_FLAG_IS_NATIVE \
    ARRAY_FLAG_29

STATIC_ASSERT(DETAILS_FLAG_IS_NATIVE == SERIES_INFO_HOLD);


//=//// DETAILS_FLAG_ENFIXED //////////////////////////////////////////////=//
//
// An enfix function gets its first argument from its left.  For a time, this
// was the property of a binding and not an ACTION! itself.  This was an
// attempt at simplification which caused more problems than it solved.
//
#define DETAILS_FLAG_ENFIXED \
    ARRAY_FLAG_30


//=//// DETAILS_FLAG_31 ///////////////////////////////////////////////////=//
//
#define DETAILS_FLAG_31 \
    ARRAY_FLAG_31


// These are the flags which are scanned for and set during Make_Action
//
#define DETAILS_MASK_CACHED \
    (DETAILS_FLAG_QUOTES_FIRST | DETAILS_FLAG_SKIPPABLE_FIRST)

// These flags should be copied when specializing or adapting.  They may not
// be derivable from the paramlist (e.g. a native with no RETURN does not
// track if it requotes beyond the paramlist).
//
#define DETAILS_MASK_INHERIT \
    (DETAILS_FLAG_DEFERS_LOOKBACK | DETAILS_FLAG_POSTPONES_ENTIRELY)


#define SET_ACTION_FLAG(s,name) \
    (cast(REBSER*, ACT(s))->header.bits |= DETAILS_FLAG_##name)

#define GET_ACTION_FLAG(s,name) \
    ((cast(REBSER*, ACT(s))->header.bits & DETAILS_FLAG_##name) != 0)

#define CLEAR_ACTION_FLAG(s,name) \
    (cast(REBSER*, ACT(s))->header.bits &= ~DETAILS_FLAG_##name)

#define NOT_ACTION_FLAG(s,name) \
    ((cast(REBSER*, ACT(s))->header.bits & DETAILS_FLAG_##name) == 0)



// Includes SERIES_FLAG_ALWAYS_DYNAMIC because an action's paramlist is always
// allocated dynamically, in order to make access to the archetype and the
// parameters faster than ARR_AT().  See code for ACT_PARAM(), etc.
//
// Includes SERIES_FLAG_FIXED_SIZE because for now, the user can't expand
// them (e.g. by APPENDing to a FRAME! value).  Also, no internal tricks
// for function composition expand them either at this time.
//
#define SERIES_MASK_PARAMLIST \
    (NODE_FLAG_NODE | SERIES_FLAG_ALWAYS_DYNAMIC | SERIES_FLAG_FIXED_SIZE \
        | SERIES_FLAG_LINK_NODE_NEEDS_MARK  /* ancestor */ \
        /* misc is not currently used */ )

#define SERIES_MASK_DETAILS \
    (NODE_FLAG_NODE  /* not fixed size, may expand via HIJACK etc.*/ \
        | SERIES_FLAG_MISC_NODE_NEEDS_MARK  /* meta */ \
        | ARRAY_FLAG_IS_DETAILS \
        /* LINK is dispatcher, a c function pointer, should not mark */ )

#if !defined(DEBUG_CHECK_CASTS)

    #define ACT(p) \
        cast(REBACT*, (p))

#else

    template <typename P>
    inline REBACT *ACT(P p) {
        constexpr bool derived =
            std::is_same<P, nullptr_t>::value  // here to avoid check below
            or std::is_same<P, REBACT*>::value;

        constexpr bool base =
            std::is_same<P, void*>::value
            or std::is_same<P, REBNOD*>::value
            or std::is_same<P, REBSER*>::value
            or std::is_same<P, REBARR*>::value;

        static_assert(
            derived or base,
            "ACT() works on void/REBNOD/REBSER/REBARR/REBACT/nullptr"
        );

        bool b = base;  // needed to avoid compiler constexpr warning
        if (b and p and (reinterpret_cast<const REBSER*>(p)->header.bits & (
            NODE_FLAG_NODE | NODE_FLAG_FREE | NODE_FLAG_CELL
                | SERIES_MASK_DETAILS
                | ARRAY_FLAG_IS_VARLIST
                | ARRAY_FLAG_IS_PAIRLIST
                | ARRAY_FLAG_HAS_FILE_LINE_UNMASKED
        )) != (
            NODE_FLAG_NODE | SERIES_MASK_DETAILS
        )){
            panic (p);
        }

        // !!! This uses a regular C cast because the `cast()` macro has not
        // been written in such a way as to tolerate nullptr, and C++ will
        // not reinterpret_cast<> a nullptr.  Review more elegant answers.
        //
        return (REBACT*)p;
    }

#endif


// The method for generating system indices isn't based on LOAD of an object,
// because the bootstrap Rebol may not have a compatible scanner.  So it uses
// simple heuristics.  (See STRIPLOAD in %common.r)
//
// This routine will try and catch any mismatch in the debug build by checking
// that the name in the context key matches the generated #define constant
//
#if defined(NDEBUG)
    #define Get_Sys_Function(id) \
        CTX_VAR(VAL_CONTEXT(Sys_Context), SYS_CTX_##id)
#else
    #define Get_Sys_Function(id) \
        Get_Sys_Function_Debug(SYS_CTX_##id, SYS_CTXKEY_##id)
#endif
