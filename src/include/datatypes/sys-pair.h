//
//  File: %sys-pair.h
//  Summary: {Definitions for Pairing Series and the Pair Datatype}
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
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
// A "pairing" fits in a REBSER node, but actually holds two distinct REBVALs.
//
// !!! There is consideration of whether series payloads of length 2 might
// be directly allocated as paireds.  This would require positioning such
// series in the pool so that they abutted against END markers.  It would be
// premature optimization to do it right now, but the design leaves it open.
//
// PAIR! values are implemented using the pairing in Revolt, which is to say
// that they are garbage collected and can hold any two values--not just
// two numbers.
//

inline static REBVAL *PAIRING_KEY(REBVAL *paired) {
    return paired + 1;
}


#define VAL_PAIR_NODE(v) \
    PAYLOAD(Any, (v)).first.node 

inline static REBVAL *VAL_PAIRING(const REBCEL *v) {
    assert(CELL_KIND(v) == REB_PAIR);
    return VAL(VAL_NODE(v));
}

#define VAL_PAIR_X(v) \
    PAIRING_KEY(VAL(VAL_PAIRING(v)))

#define VAL_PAIR_Y(v) \
    VAL(VAL_PAIRING(v))

inline static REBDEC VAL_PAIR_X_DEC(const REBCEL *v) {
    if (IS_INTEGER(VAL_PAIR_X(v)))
        return cast(REBDEC, VAL_INT64(VAL_PAIR_X(v)));
    return VAL_DECIMAL(VAL_PAIR_X(v));
}

inline static REBDEC VAL_PAIR_Y_DEC(const REBCEL *v) {
    if (IS_INTEGER(VAL_PAIR_Y(v)))
        return cast(REBDEC, VAL_INT64(VAL_PAIR_Y(v)));
    return VAL_DECIMAL(VAL_PAIR_Y(v));
}

inline static REBI64 VAL_PAIR_X_INT(const REBCEL *v) {
    if (IS_INTEGER(VAL_PAIR_X(v)))
        return VAL_INT64(VAL_PAIR_X(v));
    return ROUND_TO_INT(VAL_DECIMAL(VAL_PAIR_X(v)));
}

inline static REBDEC VAL_PAIR_Y_INT(const REBCEL *v) {
    if (IS_INTEGER(VAL_PAIR_Y(v)))
        return VAL_INT64(VAL_PAIR_Y(v));
    return ROUND_TO_INT(VAL_DECIMAL(VAL_PAIR_Y(v)));
}

inline static REBVAL *Init_Pair(
    RELVAL *out,
    const RELVAL *x,
    const RELVAL *y
){
    assert(ANY_NUMBER(x));
    assert(ANY_NUMBER(y));

    RESET_CELL(out, REB_PAIR, CELL_FLAG_FIRST_IS_NODE);
    REBVAL *p = Alloc_Pairing();
    Move_Value(PAIRING_KEY(p), KNOWN(x));
    Move_Value(p, KNOWN(y));
    Manage_Pairing(p);
    VAL_PAIR_NODE(out) = NOD(p);
    return KNOWN(out);
}

inline static REBVAL *Init_Pair_Int(RELVAL *out, REBI64 x, REBI64 y) {
    RESET_CELL(out, REB_PAIR, CELL_FLAG_FIRST_IS_NODE);
    REBVAL *p = Alloc_Pairing();
    Init_Integer(PAIRING_KEY(p), x);
    Init_Integer(p, y);
    Manage_Pairing(p);
    VAL_PAIR_NODE(out) = NOD(p);
    return KNOWN(out);
}

inline static REBVAL *Init_Pair_Dec(RELVAL *out, REBDEC x, REBDEC y) {
    RESET_CELL(out, REB_PAIR, CELL_FLAG_FIRST_IS_NODE);
    REBVAL *p = Alloc_Pairing();
    Init_Decimal(PAIRING_KEY(p), x);
    Init_Decimal(p, y);
    Manage_Pairing(p);
    VAL_PAIR_NODE(out) = NOD(p);
    return KNOWN(out);
}

inline static REBVAL *Init_Zeroed_Hack(RELVAL *out, enum Reb_Kind kind) {
    //
    // !!! This captures of a dodgy behavior of R3-Alpha, which was to assume
    // that clearing the payload of a value and then setting the header made
    // it the `zero?` of that type.  Review uses.
    //
    if (kind == REB_PAIR) {
        Init_Pair_Int(out, 0, 0);
    }
    else {
        RESET_CELL(out, kind, CELL_MASK_NONE);
        CLEAR(&out->extra, sizeof(union Reb_Value_Extra));
        CLEAR(&out->payload, sizeof(union Reb_Value_Payload));
    }
    return KNOWN(out);
}
