//
//  File: %sys-integer.h
//  Summary: "INTEGER! Datatype Header"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Revolt Open Source Contributors
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
// Integers in Rebol were standardized to use a compiler-provided 64-bit
// value.  This was formally added to the spec in C99, but many compilers
// supported it before that.
//
// !!! 64-bit extensions were added by the "rebolsource" fork, with much of
// the code still written to operate on 32-bit values.  Since the standard
// unit of indexing and block length counts remains 32-bit in that 64-bit
// build at the moment, many lingering references were left that operated
// on 32-bit values.  To make this clearer, the macros have been renamed
// to indicate which kind of integer they retrieve.  However, there should
// be a general review for reasoning, and error handling + overflow logic
// for these cases.
//

#if defined(NDEBUG) || !defined(CPLUSPLUS_11) 
    #define VAL_INT64(v) \
        PAYLOAD(Integer, (v)).i64
#else
    // allows an assert, but also lvalue: `VAL_INT64(v) = xxx`
    //
    inline static REBI64 & VAL_INT64(REBCEL *v) { // C++ reference type
        assert(CELL_KIND(v) == REB_INTEGER);
        return PAYLOAD(Integer, v).i64;
    }
    inline static REBI64 VAL_INT64(const REBCEL *v) {
        assert(CELL_KIND(v) == REB_INTEGER);
        return PAYLOAD(Integer, v).i64;
    }
#endif

inline static REBVAL *Init_Integer(RELVAL *out, REBI64 i64) {
    RESET_CELL(out, REB_INTEGER, CELL_MASK_NONE);
    PAYLOAD(Integer, out).i64 = i64;
    return cast(REBVAL*, out);
}

inline static int32_t VAL_INT32(const REBCEL *v) {
    if (VAL_INT64(v) > INT32_MAX or VAL_INT64(v) < INT32_MIN)
        fail (Error_Out_Of_Range(KNOWN(v)));
    return cast(int32_t, VAL_INT64(v));
}

inline static uint32_t VAL_UINT32(const REBCEL *v) {
    if (VAL_INT64(v) < 0 or VAL_INT64(v) > UINT32_MAX)
        fail (Error_Out_Of_Range(KNOWN(v)));
    return cast(uint32_t, VAL_INT64(v));
}

inline static REBYTE VAL_UINT8(const REBCEL *v) {
    if (VAL_INT64(v) > 255 or VAL_INT64(v) < 0)
        fail (Error_Out_Of_Range(KNOWN(v)));
    return cast(REBYTE, VAL_INT32(v));
}


// Rounding flags (passed as refinements to ROUND function):
enum {
    RF_TO = 1 << 0,
    RF_EVEN = 1 << 1,
    RF_DOWN = 1 << 2,
    RF_HALF_DOWN = 1 << 3,
    RF_FLOOR = 1 << 4,
    RF_CEILING = 1 << 5,
    RF_HALF_CEILING = 1 << 6
};

#define ROUND_TO_INT(d) \
    cast(int32_t, floor((MAX(INT32_MIN, MIN(INT32_MAX, d))) + 0.5))
