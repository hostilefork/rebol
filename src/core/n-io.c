//
//  File: %n-io.c
//  Summary: "native functions for input and output"
//  Section: natives
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

#include "sys-core.h"


//
//  form: native [
//
//  "Converts a value to a human-readable string."
//
//      value [<opt> any-value!]
//          "The value to form"
//  ]
//
REBNATIVE(form)
{
    INCLUDE_PARAMS_OF_FORM;

    return Init_Text(D_OUT, Copy_Form_Value(ARG(value), 0));
}


//
//  mold: native [
//
//  "Converts a value to a REBOL-readable string."
//
//      value "The value to mold"
//          [any-value!]
//      /only "For a block value, mold only its contents, no outer []"
//      /all "Use construction syntax"
//      /flat "No indentation"
//      /limit "Limit to a certain length"
//          [integer!]
//      /truncated "Returns LOGIC! of whether the mold was truncated"
//          [<output>]
//  ]
//
REBNATIVE(mold)
{
    INCLUDE_PARAMS_OF_MOLD;

    DECLARE_MOLD (mo);
    if (REF(all))
        SET_MOLD_FLAG(mo, MOLD_FLAG_ALL);
    if (REF(flat))
        SET_MOLD_FLAG(mo, MOLD_FLAG_INDENT);
    if (REF(limit)) {
        SET_MOLD_FLAG(mo, MOLD_FLAG_LIMIT);
        mo->limit = Int32(ARG(limit));
    }

    Push_Mold(mo);

    if (REF(only) and IS_BLOCK(ARG(value)))
        SET_MOLD_FLAG(mo, MOLD_FLAG_ONLY);

    Mold_Value(mo, ARG(value));

    REBSTR *popped = Pop_Molded_String(mo);  // sets MOLD_FLAG_TRUNCATED

    if (REF(truncated))
        rebElide(
            NATIVE_VAL(set),
            rebQ1(REF(truncated)),
            rebL(mo->opts & MOLD_FLAG_WAS_TRUNCATED),
        rebEND);

    return Init_Text(D_OUT, popped);
}


//
//  write-stdout: native [
//
//  "Boot-only implementation of WRITE-STDOUT (HIJACK'd by STDIO module)"
//
//      return: [<opt> void!]
//      value [<blank> text! char! binary!]
//          "Text to write, if a STRING! or CHAR! is converted to OS format"
//  ]
//
REBNATIVE(write_stdout)
//
// This code isn't supposed to run during normal bootup.  But for debugging
// we don't want a parallel set of PRINT operations and specializations just
// on the off chance something goes wrong in boot.  So this stub is present
// to do debug I/O.
{
    INCLUDE_PARAMS_OF_WRITE_STDOUT;

    REBVAL *v = ARG(value);

  #if defined(NDEBUG)
    UNUSED(v);
    fail ("Boot cannot print output in release build, must load I/O module");
  #else
    if (IS_BINARY(v)) {
        PROBE(v);
    }
    else if (IS_TEXT(v)) {
        printf("%s", cast(const char*, STR_HEAD(VAL_STRING(v))));
        fflush(stdout);
    }
    else {
        assert(IS_CHAR(v));
        printf("%s", VAL_CHAR_ENCODED(v));
    }
    return Init_Void(D_OUT);
  #endif
}


//
//  new-line: native [
//
//  {Sets or clears the new-line marker within a block or group.}
//
//      position "Position to change marker (modified)"
//          [block! group!]
//      mark "Set TRUE for newline"
//          [logic!]
//      /all "Set/clear marker to end of series"
//      /skip "Set/clear marker periodically to the end of the series"
//          [integer!]
//  ]
//
REBNATIVE(new_line)
{
    INCLUDE_PARAMS_OF_NEW_LINE;

    bool mark = VAL_LOGIC(ARG(mark));

    REBVAL *pos = ARG(position);
    FAIL_IF_READ_ONLY(pos);

    RELVAL *item = VAL_ARRAY_AT(pos);

    REBINT skip;
    if (REF(all))
        skip = 1;
    else if (REF(skip)) {
        skip = Int32s(ARG(skip), 1);
        if (skip < 1)
            skip = 1;
    }
    else
        skip = 0;

    REBLEN n;
    for (n = 0; true; ++n, ++item) {
        if (skip != 0 and (n % skip != 0))
            continue;

        if (IS_END(item)) {  // no cell at tail; use flag on array
            if (mark)
                SET_ARRAY_FLAG(VAL_ARRAY(pos), NEWLINE_AT_TAIL);
            else
                CLEAR_ARRAY_FLAG(VAL_ARRAY(pos), NEWLINE_AT_TAIL);
            break;
        }

        if (mark)
            SET_CELL_FLAG(item, NEWLINE_BEFORE);
        else
            CLEAR_CELL_FLAG(item, NEWLINE_BEFORE);

        if (skip == 0)
            break;
    }

    RETURN (pos);
}


//
//  new-line?: native [
//
//  {Returns the state of the new-line marker within a block or group.}
//
//      position [block! group! varargs!] "Position to check marker"
//  ]
//
REBNATIVE(new_line_q)
{
    INCLUDE_PARAMS_OF_NEW_LINE_Q;

    REBVAL *pos = ARG(position);

    REBARR *arr;
    const RELVAL *item;

    if (IS_VARARGS(pos)) {
        REBFRM *f;
        REBVAL *shared;
        if (Is_Frame_Style_Varargs_May_Fail(&f, pos)) {
            if (not f->feed->array) {
                //
                // C va_args input to frame, as from the API, but not in the
                // process of using string components which *might* have
                // newlines.  Review edge cases, like:
                //
                //    REBVAL *new_line_q = rebValue(":new-line?");
                //    bool case_one = rebDid("new-line?", "[\n]");
                //    bool case_two = rebDid(new_line_q, "[\n]");
                //
                assert(f->feed->index == TRASHED_INDEX);
                return Init_Logic(D_OUT, false);
            }

            arr = f->feed->array;
            item = f->feed->value;
        }
        else if (Is_Block_Style_Varargs(&shared, pos)) {
            arr = VAL_ARRAY(shared);
            item = VAL_ARRAY_AT(shared);
        }
        else
            panic ("Bad VARARGS!");
    }
    else {
        assert(IS_GROUP(pos) or IS_BLOCK(pos));
        arr = VAL_ARRAY(pos);
        item = VAL_ARRAY_AT(pos);
    }

    if (NOT_END(item))
        return Init_Logic(D_OUT, GET_CELL_FLAG(item, NEWLINE_BEFORE));

    return Init_Logic(D_OUT, GET_ARRAY_FLAG(arr, NEWLINE_AT_TAIL));
}


//
//  Milliseconds_From_Value: C
//
// Note that this routine is used by the SLEEP extension, as well as by WAIT.
//
REBLEN Milliseconds_From_Value(const RELVAL *v) {
    REBINT msec;

    switch (VAL_TYPE(v)) {
    case REB_INTEGER:
        msec = 1000 * Int32(v);
        break;

    case REB_DECIMAL:
        msec = cast(REBINT, 1000 * VAL_DECIMAL(v));
        break;

    case REB_TIME:
        msec = cast(REBINT, VAL_NANO(v) / (SEC_SEC / 1000));
        break;

    default:
        panic (NULL); // avoid uninitialized msec warning
    }

    if (msec < 0)
        fail (Error_Out_Of_Range(KNOWN(v)));

    return cast(REBLEN, msec);
}
