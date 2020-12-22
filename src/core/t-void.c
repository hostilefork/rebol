//
//  File: %t-void.c
//  Summary: "Symbolic type for representing an 'ornery' variable value"
//  Section: datatypes
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

#include "sys-core.h"


//
//  MF_Void: C
//
// Voids have a label to help make it clearer why an ornery error-like value
// would be existing.
//
void MF_Void(REB_MOLD *mo, REBCEL(const*) v, bool form)
{
    UNUSED(form); // no distinction between MOLD and FORM

    Append_Codepoint(mo->series, '~');

    const REBSTR* label = VAL_VOID_LABEL(v);
    Append_Utf8(mo->series, STR_UTF8(label), STR_SIZE(label));

    Append_Codepoint(mo->series, '~');
}


//
//  MAKE_Void: C
//
// Can be created from a label.
//
REB_R MAKE_Void(
    REBVAL *out,
    enum Reb_Kind kind,
    option(const REBVAL*) parent,
    const REBVAL *arg
){
    assert(not parent);
    UNUSED(parent);

    if (IS_WORD(arg))
        return Init_Void_Core(out, VAL_WORD_SPELLING(arg));

    fail (Error_Bad_Make(kind, arg));
}


//
//  TO_Void: C
//
// TO is disallowed, e.g. you can't TO convert an integer of 0 to a blank.
//
REB_R TO_Void(REBVAL *out, enum Reb_Kind kind, const REBVAL *data) {
    UNUSED(out);
    fail (Error_Bad_Make(kind, data));
}


//
//  CT_Void: C
//
// To make VOID! potentially more useful in dialecting, the spellings are used
// in comparison.  This makes this code very similar to CT_Word(), so it
// is shared.
//
REBINT CT_Void(REBCEL(const*) a, REBCEL(const*) b, bool strict)
{
    const REBSTR* label_a = VAL_VOID_LABEL(a);
    const REBSTR* label_b = VAL_VOID_LABEL(b);

    return Compare_Spellings(label_a, label_b, strict);
}


//
//  REBTYPE: C
//
REBTYPE(Void)
{
    REBVAL *voided = D_ARG(1);

    switch (VAL_WORD_SYM(verb)) {
      case SYM_REFLECT: {
        INCLUDE_PARAMS_OF_REFLECT;
        UNUSED(ARG(value)); // taken care of by `voided` above.

        switch (VAL_WORD_SYM(ARG(property))) {
          case SYM_LABEL:
            return Init_Word(D_OUT, VAL_VOID_LABEL(voided));

          default:
            break;
        }
        break; }

      case SYM_COPY: { // since `copy/deep [1 _ 2]` is legal, allow `copy _`
        INCLUDE_PARAMS_OF_COPY;
        UNUSED(ARG(value)); // already referenced as `unit`

        if (REF(part))
            fail (Error_Bad_Refines_Raw());

        UNUSED(REF(deep));
        UNUSED(REF(types));

        RETURN (voided); }

      default: break;
    }

    return R_UNHANDLED;
}
