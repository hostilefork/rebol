//
//  File: %sys-logic.h
//  Summary: "LOGIC! Datatype Header"
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
// A logic can be either true or false.  For purposes of optimization, logical
// falsehood is indicated by one of the value option bits in the header--as
// opposed to in the value payload.  This means it can be tested quickly, and
// that a single check can test for BLANK!, logic false, or nulled.
//

#define FALSE_VALUE \
    c_cast(const REBVAL*, &PG_False_Value)

#define TRUE_VALUE \
    c_cast(const REBVAL*, &PG_True_Value)

inline static bool VAL_LOGIC(const REBCEL *v) {
    assert(CELL_KIND(v) == REB_LOGIC);
    return PAYLOAD(Logic, v).flag;
}

inline static bool IS_TRUTHY(const RELVAL *v) {
    if (KIND_BYTE(v) > REB_LOGIC)
        return true;  // includes QUOTED: `if lit '_ [-- "this is truthy"]`
    if (IS_VOID(v))
        fail (Error_Void_Conditional_Raw());
    if (IS_LOGIC(v))
        return VAL_LOGIC(v);
    assert(IS_BLANK(v) or IS_NULLED(v));
    return false;
}

#define IS_FALSEY(v) \
    (not IS_TRUTHY(v))

inline static REBVAL *Init_Logic(RELVAL *out, bool flag) {
    RESET_CELL(out, REB_LOGIC, CELL_MASK_NONE);
    PAYLOAD(Logic, out).flag = flag;
    return KNOWN(out);
}

#define Init_True(out) \
    Init_Logic((out), true)

#define Init_False(out) \
    Init_Logic((out), false)


// Although a BLOCK! value is true, some constructs are safer by not allowing
// literal blocks.  e.g. `if [x] [print "this is not safe"]`.  The evaluated
// bit can let these instances be distinguished.  Note that making *all*
// evaluations safe would be limiting, e.g. `foo: any [false-thing []]`...
// So ANY and ALL use IS_TRUTHY() directly
//
inline static bool IS_CONDITIONAL_TRUE(const REBVAL *v) {
    if (IS_FALSEY(v))
        return false;
    if (KIND_BYTE(v) == REB_BLOCK)
        if (GET_CELL_FLAG(v, UNEVALUATED))
            fail (Error_Block_Conditional_Raw(v));
    return true;
}

#define IS_CONDITIONAL_FALSE(v) \
    (not IS_CONDITIONAL_TRUE(v))
