//
//  File: %sys-action.h
//  Summary: "action! defs BEFORE %tmp-internals.h (see: %sys-action.h)"
//  Section: core
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
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


struct Reb_Action {
    struct Reb_Array paramlist;
};


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
        | SERIES_FLAG_LINK_NODE_NEEDS_MARK  /* underlying */ \
        | SERIES_FLAG_MISC_NODE_NEEDS_MARK  /* meta */ \
        | ARRAY_FLAG_IS_PARAMLIST)

#define SERIES_MASK_DETAILS \
    (NODE_FLAG_NODE  /* not fixed size, may expand via HIJACK etc.*/ \
        | SERIES_FLAG_LINK_NODE_NEEDS_MARK  /* speciality */ \
        /* dispatcher is a c function pointer, should not mark */ )

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

        if (base and p and (cast(REBSER*, p)->header.bits & (
            NODE_FLAG_NODE | NODE_FLAG_FREE | NODE_FLAG_CELL
                | SERIES_MASK_PARAMLIST
                | ARRAY_FLAG_IS_VARLIST
                | ARRAY_FLAG_IS_PAIRLIST
                | ARRAY_FLAG_HAS_FILE_LINE_UNMASKED
        )) != (
            NODE_FLAG_NODE | SERIES_MASK_PARAMLIST
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
        CTX_VAR(Sys_Context, SYS_CTX_##id)
#else
    #define Get_Sys_Function(id) \
        Get_Sys_Function_Debug(SYS_CTX_##id, SYS_CTXKEY_##id)
#endif
