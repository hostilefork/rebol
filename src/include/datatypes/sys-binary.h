//
//  File: %sys-binary.h
//  Summary: {Definitions for binary series}
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Revolt Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// A BINARY! value holds a byte-size series.  The bytes may be arbitrary, or
// if the series has SERIES_FLAG_IS_STRING then modifications are constrained
// to only allow valid UTF-8 data.  Such binary "views" are possible due to
// things like the AS operator (`as binary! "abc"`).
//
// R3-Alpha used a binary series to hold the data for BITSET!.  See notes in
// %sys-bitset.h regarding this usage (which has a "negated" bit in the
// MISC() field).
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * Since strings use MISC() and LINK() for various features, and binaries
//   can be "views" on string series, this means that generally speaking a
//   binary series can't use MISC() and LINK() for its own purposes.  (For
//   the moment, typesets cannot be aliased, so you can't get into a situation
//   like `as text! as binary! make bitset! [...]`)


//=//// BINARY! SERIES ////////////////////////////////////////////////////=//

#define BIN_AT(s,n) \
    SER_AT(REBYTE, (s), (n))

#define BIN_HEAD(s) \
    SER_HEAD(REBYTE, (s))

#define BIN_TAIL(s) \
    SER_TAIL(REBYTE, (s))

#define BIN_LAST(s) \
    SER_LAST(REBYTE, (s))

inline static REBLEN BIN_LEN(REBBIN *s) {
    assert(SER_WIDE(s) == 1);
    return SER_USED(s);
}

inline static void TERM_BIN(REBSER *s) {
    assert(SER_WIDE(s) == 1);
    BIN_HEAD(s)[SER_USED(s)] = 0;
}

inline static void TERM_BIN_LEN(REBSER *s, REBLEN len) {
    assert(SER_WIDE(s) == 1);
    SET_SERIES_USED(s, len);
    BIN_HEAD(s)[len] = 0;
}

// Make a byte series of length 0 with the given capacity.  The length will
// be increased by one in order to allow for a null terminator.  Binaries are
// given enough capacity to have a null terminator in case they are aliased
// as UTF-8 data later, e.g. `as word! binary`, since it would be too late
// to give them that capacity after-the-fact to enable this.
//
inline static REBSER *Make_Binary_Core(REBLEN capacity, REBFLGS flags)
{
    REBSER *bin = Make_Series_Core(capacity + 1, sizeof(REBYTE), flags);
    TERM_SEQUENCE(bin);
    return bin;
}

#define Make_Binary(capacity) \
    Make_Binary_Core(capacity, SERIES_FLAGS_NONE)


//=//// BINARY! VALUES ////////////////////////////////////////////////////=//

#define VAL_BIN_HEAD(v) \
    BIN_HEAD(VAL_SERIES(v))

inline static REBYTE *VAL_BIN_AT(const REBCEL *v) {
    assert(CELL_KIND(v) == REB_BINARY or CELL_KIND(v) == REB_BITSET);
    if (VAL_INDEX(v) > BIN_LEN(VAL_SERIES(v)))
        fail (Error_Past_End_Raw());  // don't give deceptive return pointer
    return BIN_AT(VAL_SERIES(v), VAL_INDEX(v));
}

#define VAL_BIN_AT_HEAD(v,n) \
    BIN_AT(VAL_SERIES(v), (n))  // see remarks on VAL_ARRAY_AT_HEAD()

#define Init_Binary(out,bin) \
    Init_Any_Series((out), REB_BINARY, (bin))

#define Init_Binary_At(out,bin,offset) \
    Init_Any_Series_At((out), REB_BINARY, (bin), (offset))

inline static REBBIN *VAL_BINARY(const REBCEL* v) {
    assert(CELL_KIND(v) == REB_BINARY);
    return VAL_SERIES(v);
}


#define BYTE_BUF TG_Byte_Buf
