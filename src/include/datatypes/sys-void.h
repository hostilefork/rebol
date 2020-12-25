//
//  File: %sys-void.h
//  Summary: "VOID! Datatype Header"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Ren-C Open Source Contributors
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
// "VOID! is a means of giving a hot potato back that is a warning of
//  something, but you don't want to force an error 'in the moment'...
//  in case the returned information wasn't going to be used anyway."
//
// https://forum.rebol.info/t/947
//
// Void! results are the default for `do []`, and unlike NULL a void! *is*
// a value...however a somewhat unfriendly one.  While NULLs are falsey, void!
// is *neither* truthy nor falsey.  Though a void! can be put in an array (a
// NULL can't) if the evaluator tries to run a void! cell in an array, it will
// trigger an error.
//
// In the debug build, it is possible to make an "unreadable" void!.  This
// will behave neutrally as far as the garbage collector is concerned, so
// it can be used as a placeholder for a value that will be filled in at
// some later time--spanning an evaluation.  But if the special IS_UNREADABLE
// checks are not used, it will not respond to IS_VOID() and will also
// refuse VAL_TYPE() checks.  This is useful anytime a placeholder is needed
// in a slot temporarily where the code knows it's supposed to come back and
// fill in the correct thing later...where the asserts serve as a reminder
// if that fill in never happens.
//

inline static REBVAL *Init_Void_Core(
    unstable RELVAL *out,
    const REBSTR *label
){
    RESET_CELL(out, REB_VOID, CELL_FLAG_FIRST_IS_NODE);
    VAL_NODE(out) = NOD(m_cast(REBSTR*, label));
    return cast(REBVAL*, out);
}

#define Init_Void(out,sym) \
    Init_Void_Core((out), Canon(sym))

inline static const REBSTR* VAL_VOID_LABEL(
    unstable REBCEL(const*) v
){
    assert(CELL_KIND(v) == REB_VOID);
    assert(GET_CELL_FLAG(v, FIRST_IS_NODE));
    return cast(const REBSTR*, VAL_NODE(v));
}

inline static bool Is_Void_With_Sym(unstable const RELVAL *v, REBSYM sym) {
    assert(sym != SYM_0);
    if (not IS_VOID(v))
        return false;
    return cast(REBLEN, sym) == cast(REBLEN, STR_SYMBOL(VAL_VOID_LABEL(v)));
}


#if !defined(DEBUG_UNREADABLE_VOIDS)  // release behavior, non-crashing VOID!
    #define Init_Unreadable_Void(v) \
        Init_Void_Core(v, PG_Unreadable_Canon)  // canon available early

    #define IS_VOID_RAW(v) \
        IS_VOID(v)

    #define ASSERT_UNREADABLE_IF_DEBUG(v) \
        assert(IS_VOID(v))  // would have to be a void even if not unreadable

    #define ASSERT_READABLE_IF_DEBUG(v) \
        NOOP
#else
    inline static REBVAL *Init_Unreadable_Void_Debug(
        unstable RELVAL *out, const char *file, int line
    ){
        RESET_CELL_Debug(out, REB_VOID, CELL_FLAG_FIRST_IS_NODE, file, line);

        // While SYM_UNREADABLE might be nice here, that prevents usage at
        // boot time (e.g. data stack initialization)...and it's a good way
        // to crash sites that expect normal voids.  It's usually clear
        // from the assert that the void is unreadable, anyway.
        //
        VAL_NODE(out) = nullptr;  // needs flag for VAL_NODE() to not assert
        return cast(REBVAL*, out);
    }

    #define Init_Unreadable_Void(out) \
        Init_Unreadable_Void_Debug((out), __FILE__, __LINE__)

    #define IS_VOID_RAW(v) \
        (KIND3Q_BYTE_UNCHECKED(v) == REB_VOID)

    inline static bool IS_UNREADABLE_DEBUG(unstable const RELVAL *v) {
        if (KIND3Q_BYTE_UNCHECKED(v) != REB_VOID)
            return false;
        return VAL_NODE(v) == nullptr;
    }

    #define ASSERT_UNREADABLE_IF_DEBUG(v) \
        assert(IS_UNREADABLE_DEBUG(v))

    #define ASSERT_READABLE_IF_DEBUG(v) \
        assert(not IS_UNREADABLE_DEBUG(v))
#endif
