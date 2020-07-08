//
//  File: %sys-blank.h
//  Summary: "BLANK! Datatype Header"
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
// Blank! values are a kind of "reified" null, and you can convert between
// them using TRY and OPT:
//
//     >> try ()
//     == _
//
//     >> opt _
//     ; null
//
// Like null, they are considered to be false--like the LOGIC! #[false] value.
// Only these three things are conditionally false in Revolt, and testing for
// conditional truth and falsehood is frequent.  Hence in addition to its
// type, BLANK! also carries a header bit that can be checked for conditional
// falsehood, to save on needing to separately test the type.
//
// In the debug build, it is possible to make an "unreadable" blank!.  This
// will behave neutrally as far as the garbage collector is concerned, so
// it can be used as a placeholder for a value that will be filled in at
// some later time--spanning an evaluation.  But if the special IS_UNREADABLE
// checks are not used, it will not respond to IS_BLANK() and will also
// refuse VAL_TYPE() checks.  This is useful anytime a placeholder is needed
// in a slot temporarily where the code knows it's supposed to come back and
// fill in the correct thing later...where the asserts serve as a reminder
// if that fill in never happens.
//

#define BLANK_VALUE \
    c_cast(const REBVAL*, &PG_Blank_Value)

#define Init_Blank(v) \
    RESET_CELL((v), REB_BLANK, CELL_MASK_NONE)

#ifdef DEBUG_UNREADABLE_BLANKS
    inline static REBVAL *Init_Unreadable_Blank_Debug(
        RELVAL *out, const char *file, int line
    ){
        RESET_CELL_Debug(out, REB_BLANK, CELL_MASK_NONE, file, line);
        assert(out->extra.tick > 0);
        out->extra.tick = -out->extra.tick;
        return KNOWN(out);
    }

    #define Init_Unreadable_Blank(out) \
        Init_Unreadable_Blank_Debug((out), __FILE__, __LINE__)

    #define IS_BLANK_RAW(v) \
        (KIND_BYTE_UNCHECKED(v) == REB_BLANK)

    inline static bool IS_UNREADABLE_DEBUG(const RELVAL *v) {
        if (KIND_BYTE_UNCHECKED(v) != REB_BLANK)
            return false;
        return v->extra.tick < 0;
    }

    #define ASSERT_UNREADABLE_IF_DEBUG(v) \
        assert(IS_UNREADABLE_DEBUG(v))

    #define ASSERT_READABLE_IF_DEBUG(v) \
        assert(not IS_UNREADABLE_DEBUG(v))
#else
    #define Init_Unreadable_Blank(v) \
        Init_Blank(v)

    #define IS_BLANK_RAW(v) \
        IS_BLANK(v)

    #define ASSERT_UNREADABLE_IF_DEBUG(v) \
        assert(IS_BLANK(v)) // would have to be a blank even if not unreadable

    #define ASSERT_READABLE_IF_DEBUG(v) \
        NOOP
#endif
