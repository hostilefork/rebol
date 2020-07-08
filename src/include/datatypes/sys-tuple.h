//
//  File: %sys-tuple.h
//  Summary: "Tuple Datatype Header"
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
// TUPLE! is a Rebol2/R3-Alpha concept to fit up to 7 byte-sized integers
// directly into a value payload without needing to make a series allocation.
// At source level they would be numbers separated by dots, like `1.2.3.4.5`.
// This was mainly applied for IP addresses and RGB/RGBA constants, and
// considered to be a "lightweight"...it would allow PICK and POKE like a
// series, but did not behave like one due to not having a position.
//
// !!! Revolt challenges the value of the TUPLE! type as defined.  Color
// literals are often hexadecimal (where BINARY! would do) and IPv6 addresses
// have a different notation.  It may be that `.` could be used for a more
// generalized partner to PATH!, where `a.b.1` would be like a/b/1
//

#define MAX_TUPLE \
    ((sizeof(uint32_t) * 2)) // for same properties on 64-bit and 32-bit

#if !defined(CPLUSPLUS_11)

    #define VAL_TUPLE(v)        PAYLOAD(Bytes, (v)).common
    #define VAL_TUPLE_LEN(v)    EXTRA(Any, (v)).u

#else
    // C++ build can inject a check that it's a tuple, and still give l-value

    inline static const REBYTE *VAL_TUPLE(const REBCEL *v) {
        assert(CELL_KIND(v) == REB_TUPLE);
        return PAYLOAD(Bytes, v).common;
    }

    inline static REBYTE *VAL_TUPLE(REBCEL *v) {
        assert(CELL_KIND(v) == REB_TUPLE);
        return PAYLOAD(Bytes, v).common;
    }

    inline static REBYTE VAL_TUPLE_LEN(const REBCEL *v) {
        assert(CELL_KIND(v) == REB_TUPLE);
        assert(EXTRA(Any, v).u <= MAX_TUPLE);
        return EXTRA(Any, v).u;
    }

    inline static uintptr_t &VAL_TUPLE_LEN(REBCEL *v) {
        assert(CELL_KIND(v) == REB_TUPLE);
        return EXTRA(Any, v).u;
    }
#endif


inline static REBVAL *Init_Tuple(
    RELVAL *out,
    const REBYTE *data,
    REBLEN len
){
    RESET_CELL(out, REB_TUPLE, CELL_MASK_NONE);

    REBLEN n = len;
    REBYTE *bp;
    for (bp = VAL_TUPLE(out); n > 0; --n)
        *bp++ = *data++;

    // !!! Historically, 1.0.0 = 1.0.0.0 under non-strict equality.  Make the
    // comparison easier just by setting all the bytes to zero.
    //
    memset(bp, 0, MAX_TUPLE - len);

    VAL_TUPLE_LEN(out) = len;
    return cast(REBVAL*, out);
}
