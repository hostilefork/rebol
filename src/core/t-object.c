//
//  File: %t-object.c
//  Summary: "object datatype"
//  Section: datatypes
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Ren-C Open Source Contributors
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


static void Append_To_Context(REBVAL *context, REBVAL *arg)
{
    REBCTX *c = VAL_CONTEXT(context);

    // Can be a word:
    if (ANY_WORD(arg)) {
        if (0 == Find_Canon_In_Context(context, VAL_WORD_CANON(arg))) {
            Expand_Context(c, 1); // copy word table also
            Append_Context(c, 0, VAL_WORD_SPELLING(arg));
            // default of Append_Context is that arg's value is void
        }
        return;
    }

    if (not IS_BLOCK(arg))
        fail (arg);

    // Process word/value argument block:

    unstable const RELVAL *item = VAL_ARRAY_AT(arg);

    // Can't actually fail() during a collect, so make sure any errors are
    // set and then jump to a Collect_End()
    //
    REBCTX *error = NULL;

    struct Reb_Collector collector;
    Collect_Start(&collector, COLLECT_ANY_WORD | COLLECT_AS_TYPESET);

    // Leave the [0] slot blank while collecting (ROOTKEY/ROOTPARAM), but
    // valid (but "unreadable") bits so that the copy will still work.
    //
    Init_Unreadable_Void(ARR_HEAD(BUF_COLLECT));
    SET_ARRAY_LEN_NOTERM(BUF_COLLECT, 1);

    // Setup binding table with obj words.  Binding table is empty so don't
    // bother checking for duplicates.
    //
    Collect_Context_Keys(&collector, c, false);

    // Examine word/value argument block

    unstable const RELVAL *word;
    for (word = item; NOT_END(word); word += 2) {
        if (!IS_WORD(word) && !IS_SET_WORD(word)) {
            error = Error_Bad_Value_Core(word, VAL_SPECIFIER(arg));
            goto collect_end;
        }

        const REBSTR *canon = VAL_WORD_CANON(word);

        if (Try_Add_Binder_Index(
            &collector.binder, canon, ARR_LEN(BUF_COLLECT))
        ){
            //
            // Wasn't already collected...so we added it...
            //
            EXPAND_SERIES_TAIL(SER(BUF_COLLECT), 1);
            Init_Context_Key(ARR_LAST(BUF_COLLECT), VAL_WORD_SPELLING(word));
        }
        if (IS_END(word + 1))
            break; // fix bug#708
    }

    TERM_ARRAY_LEN(BUF_COLLECT, ARR_LEN(BUF_COLLECT));

  blockscope {  // Append new words to obj
    REBLEN len = CTX_LEN(c) + 1;
    Expand_Context(c, ARR_LEN(BUF_COLLECT) - len);

    unstable RELVAL *collect_key = ARR_AT(BUF_COLLECT, len);
    for (; NOT_END(collect_key); ++collect_key)
        Append_Context(c, NULL, VAL_KEY_SPELLING(collect_key));
  }

    // Set new values to obj words
    for (word = item; NOT_END(word); word += 2) {
        REBLEN i = Get_Binder_Index_Else_0(
            &collector.binder, VAL_WORD_CANON(word)
        );
        assert(i != 0);

        REBVAL *key = CTX_KEY(c, i);
        REBVAL *var = CTX_VAR(c, i);

        if (GET_CELL_FLAG(var, PROTECTED)) {
            error = Error_Protected_Key(key);
            goto collect_end;
        }

        if (Is_Param_Hidden(key, var)) {
            error = Error_Hidden_Raw();
            goto collect_end;
        }

        if (IS_END(word + 1)) {
            Init_Blank(var);
            break; // fix bug#708
        }
        else
            Derelativize(var, &word[1], VAL_SPECIFIER(arg));
    }

collect_end:
    Collect_End(&collector);

    if (error != NULL)
        fail (error);
}


//
//  CT_Context: C
//
REBINT CT_Context(REBCEL(const*) a, REBCEL(const*) b, bool strict)
{
    assert(ANY_CONTEXT_KIND(CELL_KIND(a)));
    assert(ANY_CONTEXT_KIND(CELL_KIND(b)));

    if (CELL_KIND(a) != CELL_KIND(b))  // e.g. ERROR! won't equal OBJECT!
        return CELL_KIND(a) > CELL_KIND(b) ? 1 : 0;

    REBCTX *c1 = VAL_CONTEXT(a);
    REBCTX *c2 = VAL_CONTEXT(b);
    if (c1 == c2)
        return 0;  // short-circuit, always equal if same context pointer

    // Note: can't short circuit on unequal frame lengths alone, as hidden
    // fields of objects (notably `self`) do not figure into the `equal?`
    // of their public portions.

    const REBVAL *key1 = CTX_KEYS_HEAD(c1);
    const REBVAL *key2 = CTX_KEYS_HEAD(c2);
    const REBVAL *var1 = CTX_VARS_HEAD(c1);
    const REBVAL *var2 = CTX_VARS_HEAD(c2);

    // Compare each entry, in order.  Skip any hidden fields, field names are
    // compared case-insensitively.
    //
    // !!! The order dependence suggests that `make object! [a: 1 b: 2]` will
    // not be equal to `make object! [b: 1 a: 2]`.  See #2341
    //
    for (; NOT_END(key1) && NOT_END(key2); key1++, key2++, var1++, var2++) {
      no_advance:
        if (Is_Param_Hidden(key1, var1)) {
            ++key1;
            ++var1;
            if (IS_END(key1))
                break;
            goto no_advance;
        }
        if (Is_Param_Hidden(key2, var2)) {
            ++key2;
            ++var2;
            if (IS_END(key2))
                break;
            goto no_advance;
        }

        const REBSTR *canon1 = VAL_KEY_CANON(key1);
        const REBSTR *canon2 = VAL_KEY_CANON(key2);
        if (canon1 != canon2)  // case-insensitive, even in `strict` compare
            return canon1 > canon2 ? 1 : -1;

        REBINT diff = Cmp_Value(var1, var2, strict);
        if (diff != 0)
            return diff;
    }

    // Either key1 or key2 is at the end here, but the other might contain
    // all hidden values.  Which is okay.  But if a value isn't hidden,
    // they don't line up.
    //
    for (; NOT_END(key1); key1++, var1++) {
        if (not Is_Param_Hidden(key1, var1))
            return 1;
    }
    for (; NOT_END(key2); key2++, var2++) {
        if (not Is_Param_Hidden(key2, var2))
            return -1;
    }

    return 0;
}


//
//  MAKE_Frame: C
//
// !!! The feature of MAKE FRAME! from a VARARGS! would be interesting as a
// way to support usermode authoring of things like MATCH.
//
// For now just support ACTION! (or path/word to specify an action)
//
REB_R MAKE_Frame(
    REBVAL *out,
    enum Reb_Kind kind,
    option(const REBVAL*) parent,
    const REBVAL *arg
){
    if (parent)
        fail (Error_Bad_Make_Parent(kind, unwrap(parent)));

    // MAKE FRAME! on a VARARGS! supports the userspace authoring of ACTION!s
    // like MATCH.  However, MATCH is kept as a native for performance--as
    // many usages will not be variadic, and the ones that are do not need
    // to create GC-managed FRAME! objects.
    //
    if (IS_VARARGS(arg)) {
        DECLARE_LOCAL (temp);
        SET_END(temp);
        PUSH_GC_GUARD(temp);

        if (Do_Vararg_Op_Maybe_End_Throws_Core(
            temp,
            VARARG_OP_TAKE,
            arg,
            REB_P_HARD
        )){
            assert(!"Hard quoted vararg ops should not throw");
        }

        if (IS_END(temp))
            fail ("Cannot MAKE FRAME! on an empty VARARGS!");

        bool threw = Make_Frame_From_Varargs_Throws(out, temp, arg);

        DROP_GC_GUARD(temp);

        return threw ? R_THROWN : out;
    }

    REBDSP lowest_ordered_dsp = DSP;  // Data stack gathers any refinements

    if (not IS_ACTION(arg))
        fail (Error_Bad_Make(kind, arg));

    REBCTX *exemplar = Make_Context_For_Action(
        arg, // being used here as input (e.g. the ACTION!)
        lowest_ordered_dsp, // will weave in any refinements pushed
        nullptr // no binder needed, not running any code
    );

    // See notes in %c-specialize.c about the special encoding used to
    // put /REFINEMENTs in refinement slots (instead of true/false/null)
    // to preserve the order of execution.

    return Init_Frame(out, exemplar, VAL_ACTION_LABEL(arg));
}


//
//  TO_Frame: C
//
// Currently can't convert anything TO a frame; nothing has enough information
// to have an equivalent representation (an OBJECT! could be an expired frame
// perhaps, but still would have no ACTION OF property)
//
REB_R TO_Frame(REBVAL *out, enum Reb_Kind kind, const REBVAL *arg)
{
    UNUSED(out);
    fail (Error_Bad_Make(kind, arg));
}


//
//  MAKE_Context: C
//
REB_R MAKE_Context(
    REBVAL *out,
    enum Reb_Kind kind,
    option(const REBVAL*) parent,
    const REBVAL *arg
){
    // Other context kinds (FRAME!, ERROR!, PORT!) have their own hooks.
    //
    assert(kind == REB_OBJECT or kind == REB_MODULE);

    option(REBCTX*) parent_ctx = parent
        ? VAL_CONTEXT(unwrap(parent))
        : nullptr;

    if (IS_BLOCK(arg)) {
        REBCTX *ctx = Make_Selfish_Context_Detect_Managed(
            REB_OBJECT,
            VAL_ARRAY_AT(arg),
            parent_ctx
        );
        Init_Any_Context(out, kind, ctx); // GC guards it

        // !!! This binds the actual body data, not a copy of it.  See
        // Virtual_Bind_Deep_To_New_Context() for future directions.
        //
        Bind_Values_Deep(VAL_ARRAY_AT_MUTABLE_HACK(arg), CTX_ARCHETYPE(ctx));

        DECLARE_LOCAL (dummy);
        if (Do_Any_Array_At_Throws(dummy, arg, SPECIFIED)) {
            Move_Value(out, dummy);
            return R_THROWN;
        }

        return out;
    }

    // `make object! 10` - currently not prohibited for any context type
    //
    if (ANY_NUMBER(arg)) {
        //
        // !!! Temporary!  Ultimately SELF will be a user protocol.
        // We use Make_Selfish_Context while MAKE is filling in for
        // what will be responsibility of the generators, just to
        // get "completely fake SELF" out of index slot [0]
        //
        REBCTX *context = Make_Selfish_Context_Detect_Managed(
            kind,
            END_NODE,  // values to scan for toplevel set-words (empty)
            parent_ctx
        );

        // !!! Allocation when SELF is not the responsibility of MAKE
        // will be more basic and look like this.
        //
        /*
        REBINT n = Int32s(arg, 0);
        context = Alloc_Context(kind, n);
        RESET_VAL_HEADER(CTX_ARCHETYPE(context), target, CELL_MASK_NONE);
        CTX_SPEC(context) = NULL;
        CTX_BODY(context) = NULL; */

        return Init_Any_Context(out, kind, context);
    }

    if (parent)
        fail (Error_Bad_Make_Parent(kind, unwrap(parent)));

    // make object! map!
    if (IS_MAP(arg)) {
        REBCTX *c = Alloc_Context_From_Map(VAL_MAP(arg));
        return Init_Any_Context(out, kind, c);
    }

    fail (Error_Bad_Make(kind, arg));
}


//
//  TO_Context: C
//
REB_R TO_Context(REBVAL *out, enum Reb_Kind kind, const REBVAL *arg)
{
    // Other context kinds (FRAME!, ERROR!, PORT!) have their own hooks.
    //
    assert(kind == REB_OBJECT or kind == REB_MODULE);

    if (kind == REB_OBJECT) {
        //
        // !!! Contexts hold canon values now that are typed, this init
        // will assert--a TO conversion would thus need to copy the varlist
        //
        return Init_Object(out, VAL_CONTEXT(arg));
    }

    fail (Error_Bad_Make(kind, arg));
}


//
//  PD_Context: C
//
REB_R PD_Context(
    REBPVS *pvs,
    const RELVAL *picker,
    option(const REBVAL*) setval
){
    REBCTX *c = VAL_CONTEXT(pvs->out);

    if (not IS_WORD(picker))
        return R_UNHANDLED;

    // See if the binding of the word is already to the context (so there's
    // no need to go hunting).  'x
    //
    REBLEN n;
    if (VAL_BINDING(picker) == NOD(c))
        n = VAL_WORD_INDEX(picker);
    else {
        n = Find_Canon_In_Context(pvs->out, VAL_WORD_CANON(picker));

        if (n == 0)
            return R_UNHANDLED;

        // !!! As an experiment, try caching the binding index in the word.
        // This "corrupts" it, but if we say paths effectively own their
        // top-level words that could be all right.  Note this won't help if
        // the word is an evaluative product, as the bits live in the cell
        // and it will be discarded.
        //
        INIT_BINDING(m_cast(RELVAL*, picker), NOD(c));
        INIT_WORD_INDEX(m_cast(RELVAL*, picker), n);
    }

    REBVAL *var = CTX_VAR(c, n);
    if (setval) {
        ENSURE_MUTABLE(pvs->out);

        if (GET_CELL_FLAG(var, PROTECTED))
            fail (Error_Protected_Word_Raw(rebUnrelativize(picker)));
    }

    pvs->u.ref.cell = var;
    pvs->u.ref.specifier = SPECIFIED;
    return R_REFERENCE;
}


//
//  meta-of: native [
//
//  {Get a reference to the "meta" context associated with a value.}
//
//      return: [<opt> any-context!]
//      value [<blank> action! any-context!]
//  ]
//
REBNATIVE(meta_of)  // see notes on MISC_META()
{
    INCLUDE_PARAMS_OF_META_OF;

    REBVAL *v = ARG(value);

    REBCTX *meta;
    if (IS_ACTION(v))
        meta = ACT_META(VAL_ACTION(v));
    else {
        assert(ANY_CONTEXT(v));
        meta = CTX_META(VAL_CONTEXT(v));
    }

    if (not meta)
        return nullptr;

    RETURN (CTX_ARCHETYPE(meta));
}


//
//  set-meta: native [
//
//  {Set "meta" object associated with all references to a value.}
//
//      return: [<opt> any-context!]
//      value [action! any-context!]
//      meta [<opt> any-context!]
//  ]
//
REBNATIVE(set_meta)
//
// See notes accompanying the `meta` field in the REBSER definition.
{
    INCLUDE_PARAMS_OF_SET_META;

    REBCTX *meta;
    if (ANY_CONTEXT(ARG(meta))) {
        if (VAL_BINDING(ARG(meta)) != UNBOUND)
            fail ("SET-META can't store context bindings, must be unbound");

        meta = VAL_CONTEXT(ARG(meta));
    }
    else {
        assert(IS_NULLED(ARG(meta)));
        meta = nullptr;
    }

    REBVAL *v = ARG(value);

    if (IS_ACTION(v))
        MISC_META_NODE(ACT_DETAILS(VAL_ACTION(v))) = NOD(meta);
    else
        MISC_META_NODE(CTX_VARLIST(VAL_CONTEXT(v))) = NOD(meta);

    if (not meta)
        return nullptr;

    RETURN (CTX_ARCHETYPE(meta));
}


//
//  Copy_Context_Extra_Managed: C
//
// If no extra space is requested, the same keylist will be reused.
//
// !!! Copying a context used to be more different from copying an ordinary
// array.  But at the moment, much of the difference is that the marked bit
// in cells gets duplicated (so new context has the same ARG_MARKED_CHECKED
// settings on its variables).  Review if the copying can be cohered better.
//
REBCTX *Copy_Context_Extra_Managed(
    REBCTX *original,
    REBLEN extra,
    REBU64 types
){
    assert(GET_ARRAY_FLAG(CTX_VARLIST(original), IS_VARLIST));
    ASSERT_ARRAY_MANAGED(CTX_KEYLIST(original));
    assert(NOT_SERIES_INFO(original, INACCESSIBLE));

    REBARR *varlist = Make_Array_For_Copy(
        CTX_LEN(original) + extra + 1,
        SERIES_MASK_VARLIST | NODE_FLAG_MANAGED,
        nullptr // original_array, N/A because LINK()/MISC() used otherwise
    );
    REBVAL *dest = SPECIFIC(STABLE(ARR_HEAD(varlist)));

    // The type information and fields in the rootvar (at head of the varlist)
    // get filled in with a copy, but the varlist needs to be updated in the
    // copied rootvar to the one just created.
    //
    Move_Value(dest, CTX_ARCHETYPE(original));
    INIT_VAL_CONTEXT_VARLIST(dest, varlist);

    ++dest;

    // Now copy the actual vars in the context, from wherever they may be
    // (might be in an array, or might be in the chunk stack for FRAME!)
    //
    REBVAL *src = CTX_VARS_HEAD(original);
    for (; NOT_END(src); ++src, ++dest) {
        Move_Var(dest, src); // keep ARG_MARKED_CHECKED

        REBFLGS flags = NODE_FLAG_MANAGED;  // !!! Review, which flags?
        Clonify(dest, flags, types);
    }

    TERM_ARRAY_LEN(varlist, CTX_LEN(original) + 1);
    SER(varlist)->header.bits |= SERIES_MASK_VARLIST;

    REBCTX *copy = CTX(varlist); // now a well-formed context

    if (extra == 0)
        INIT_CTX_KEYLIST_SHARED(copy, CTX_KEYLIST(original));  // ->link field
    else {
        assert(CTX_TYPE(original) != REB_FRAME);  // can't expand FRAME!s

        REBARR *keylist = Copy_Array_At_Extra_Shallow(
            CTX_KEYLIST(original),
            0,
            SPECIFIED,
            extra,
            SERIES_MASK_KEYLIST | NODE_FLAG_MANAGED
        );

        LINK_ANCESTOR_NODE(keylist) = NOD(CTX_KEYLIST(original));

        INIT_CTX_KEYLIST_UNIQUE(copy, keylist);  // ->link field
    }

    // A FRAME! in particular needs to know if it points back to a stack
    // frame.  The pointer is NULLed out when the stack level completes.
    // If we're copying a frame here, we know it's not running.
    //
    if (CTX_TYPE(original) == REB_FRAME)
        MISC_META_NODE(varlist) = nullptr;
    else {
        // !!! Should the meta object be copied for other context types?
        // Deep copy?  Shallow copy?  Just a reference to the same object?
        //
        MISC_META_NODE(varlist) = nullptr;
    }

    return copy;
}


//
//  MF_Context: C
//
void MF_Context(REB_MOLD *mo, REBCEL(const*) v, bool form)
{
    REBSTR *s = mo->series;

    REBCTX *c = VAL_CONTEXT(v);

    // Prevent endless mold loop:
    //
    if (Find_Pointer_In_Series(TG_Mold_Stack, c) != NOT_FOUND) {
        if (not form) {
            Pre_Mold(mo, v); // If molding, get #[object! etc.
            Append_Codepoint(s, '[');
        }
        Append_Ascii(s, "...");

        if (not form) {
            Append_Codepoint(s, ']');
            End_Mold(mo);
        }
        return;
    }
    Push_Pointer_To_Series(TG_Mold_Stack, c);

    // Simple rule for starters: don't honor the hidden status of parameters
    // if the frame phase is executing.
    //
    bool honor_hidden = true;
    if (CELL_KIND(v) == REB_FRAME)
        honor_hidden = (VAL_OPT_PHASE(v) == nullptr);

    if (form) {
        //
        // Mold all words and their values ("key: <molded value>")
        //
        REBVAL *key = VAL_CONTEXT_KEYS_HEAD(v);
        REBVAL *var = CTX_VARS_HEAD(c);
        bool had_output = false;
        for (; NOT_END(key); key++, var++) {
            if (Is_Param_Sealed(key))
                continue;
            if (honor_hidden and Is_Param_Hidden(key, var))
                continue;

            Append_Spelling(mo->series, VAL_KEY_SPELLING(key));
            Append_Ascii(mo->series, ": ");
            Mold_Value(mo, var);
            Append_Codepoint(mo->series, LF);
            had_output = true;
        }

        // Remove the final newline...but only if WE added to the buffer
        //
        if (had_output)
            Trim_Tail(mo, '\n');

        Drop_Pointer_From_Series(TG_Mold_Stack, c);
        return;
    }

    // Otherwise we are molding

    Pre_Mold(mo, v);

    Append_Codepoint(s, '[');

    mo->indent++;

    REBVAL *key = VAL_CONTEXT_KEYS_HEAD(v);
    REBVAL *var = CTX_VARS_HEAD(VAL_CONTEXT(v));

    for (; NOT_END(key); ++key, ++var) {
        if (Is_Param_Sealed(key))
            continue;
        if (honor_hidden and Is_Param_Hidden(key, var))
            continue;

        New_Indented_Line(mo);

        const REBSTR *spelling = VAL_KEY_SPELLING(key);
        Append_Utf8(s, STR_UTF8(spelling), STR_SIZE(spelling));

        Append_Ascii(s, ": ");

        if (IS_NULLED(var))
            Append_Ascii(s, "'");  // `field: '` would evaluate to null
        else {
            if (IS_VOID(var) or not ANY_INERT(var))  // needs quoting
                Append_Ascii(s, "'");
            Mold_Value(mo, var);
        }
    }

    mo->indent--;
    New_Indented_Line(mo);
    Append_Codepoint(s, ']');

    End_Mold(mo);

    Drop_Pointer_From_Series(TG_Mold_Stack, c);
}


//
//  Context_Common_Action_Maybe_Unhandled: C
//
// Similar to Series_Common_Action_Maybe_Unhandled().  Introduced because
// PORT! wants to act like a context for some things, but if you ask an
// ordinary object if it's OPEN? it doesn't know how to do that.
//
REB_R Context_Common_Action_Maybe_Unhandled(
    REBFRM *frame_,
    const REBVAL *verb
){
    REBVAL *v = D_ARG(1);
    REBCTX *c = VAL_CONTEXT(v);

    switch (VAL_WORD_SYM(verb)) {
      case SYM_REFLECT: {
        INCLUDE_PARAMS_OF_REFLECT;
        UNUSED(ARG(value));  // covered by `v`

        REBVAL *property = ARG(property);
        switch (VAL_WORD_SYM(property)) {
          case SYM_LENGTH: // !!! Should this be legal?
            return Init_Integer(D_OUT, CTX_LEN(c));

          case SYM_TAIL_Q: // !!! Should this be legal?
            return Init_Logic(D_OUT, CTX_LEN(c) == 0);

          case SYM_WORDS:
            return Init_Block(D_OUT, Context_To_Array(v, 1));

          case SYM_VALUES:
            return Init_Block(D_OUT, Context_To_Array(v, 2));

          case SYM_BODY:
            return Init_Block(D_OUT, Context_To_Array(v, 3));

        // Noticeably not handled by average objects: SYM_OPEN_Q (`open?`)

          default:
            break;
        }

        return R_UNHANDLED; }

      default:
        break;
    }

    return R_UNHANDLED;
}


//
//  REBTYPE: C
//
// Handles object!, module!, and error! datatypes.
//
REBTYPE(Context)
{
    REB_R r = Context_Common_Action_Maybe_Unhandled(frame_, verb);
    if (r != R_UNHANDLED)
        return r;

    REBVAL *context = D_ARG(1);
    REBCTX *c = VAL_CONTEXT(context);

    switch (VAL_WORD_SYM(verb)) {
      case SYM_REFLECT: {
        INCLUDE_PARAMS_OF_REFLECT;
        UNUSED(ARG(value));  // covered by `v`

        if (VAL_TYPE(context) != REB_FRAME)
            break;

        REBVAL *property = ARG(property);
        REBSYM sym = VAL_WORD_SYM(property);

        if (sym == SYM_LABEL) {
            //
            // Can be answered for frames that have no execution phase, if
            // they were initialized with a label.
            //
            const REBSTR *label = VAL_FRAME_LABEL(context);
            if (label)
                return Init_Word(D_OUT, label);

            // If the frame is executing, we can look at the label in the
            // REBFRM*, which will tell us what the overall execution label
            // would be.  This might be confusing, however...if the phase
            // is drastically different.  Review.
        }

        if (sym == SYM_ACTION) {
            //
            // Currently this can be answered for any frame, even if it is
            // expired...though it probably shouldn't do this unless it's
            // an indefinite lifetime object, so that paramlists could be
            // GC'd if all the frames pointing to them were expired but still
            // referenced somewhere.
            //
            return Init_Action(
                D_OUT,
                VAL_PHASE_ELSE_ARCHETYPE(context),  // archetypal, no binding
                VAL_FRAME_LABEL(context),
                CTX(VAL_BINDING(context))  // e.g. where RETURN returns to
            );
        }

        REBFRM *f = CTX_FRAME_MAY_FAIL(c);

        switch (sym) {
          case SYM_FILE: {
            REBSTR *file = FRM_FILE(f);
            if (not file)
                return nullptr;
            return Init_Word(D_OUT, file); }

          case SYM_LINE: {
            REBLIN line = FRM_LINE(f);
            if (line == 0)
                return nullptr;
            return Init_Integer(D_OUT, line); }

          case SYM_LABEL: {
            if (not f->label)
                return nullptr;
            return Init_Word(D_OUT, unwrap(f->label)); }

          case SYM_NEAR:
            return Init_Near_For_Frame(D_OUT, f);

          case SYM_PARENT: {
            //
            // Only want action frames (though `pending? = true` ones count).
            //
            REBFRM *parent = f;
            while ((parent = parent->prior) != FS_BOTTOM) {
                if (not Is_Action_Frame(parent))
                    continue;

                REBCTX* ctx_parent = Context_For_Frame_May_Manage(parent);
                RETURN (CTX_ARCHETYPE(ctx_parent));
            }
            return nullptr; }

          default:
            break;
        }
        fail (Error_Cannot_Reflect(VAL_TYPE(context), property)); }


      case SYM_APPEND: {
        REBVAL *arg = D_ARG(2);
        if (IS_NULLED_OR_BLANK(arg))
            RETURN (context);  // don't fail on R/O if it would be a no-op

        ENSURE_MUTABLE(context);
        if (not IS_OBJECT(context) and not IS_MODULE(context))
            return R_UNHANDLED;
        Append_To_Context(context, arg);
        RETURN (context); }

      case SYM_COPY: {  // Note: words are not copied and bindings not changed!
        INCLUDE_PARAMS_OF_COPY;
        UNUSED(PAR(value));  // covered by `context`

        if (REF(part))
            fail (Error_Bad_Refines_Raw());

        REBU64 types = 0;
        if (REF(types)) {
            if (IS_DATATYPE(ARG(types)))
                types = FLAGIT_KIND(VAL_TYPE_KIND(ARG(types)));
            else {
                types |= VAL_TYPESET_LOW_BITS(ARG(types));
                types |= cast(REBU64, VAL_TYPESET_HIGH_BITS(ARG(types))) << 32;
            }
        }
        else if (REF(deep))
            types = TS_STD_SERIES;

        return Init_Any_Context(
            D_OUT,
            VAL_TYPE(context),
            Copy_Context_Extra_Managed(c, 0, types)
        ); }

      case SYM_SELECT:
      case SYM_FIND: {
        REBVAL *arg = D_ARG(2);
        if (not IS_WORD(arg))
            return nullptr;

        REBLEN n = Find_Canon_In_Context(context, VAL_WORD_CANON(arg));
        if (n == 0)
            return nullptr;

        if (VAL_WORD_SYM(verb) == SYM_FIND)
            return Init_True(D_OUT); // !!! obscures non-LOGIC! result?

        RETURN (CTX_VAR(c, n)); }

      default:
        break;
    }

    return R_UNHANDLED;
}


//
//  construct: native [
//
//  "Creates an ANY-CONTEXT! instance"
//
//      return: [<opt> any-context!]
//      spec [<blank> block!]
//          "Object specification block (bindings modified)"
//      /only "Values are kept as-is"
//      /with "Use a parent/prototype context"
//          [any-context!]
//  ]
//
REBNATIVE(construct)
//
// !!! This assumes you want a SELF defined.  The entire concept of SELF
// needs heavy review.
//
// !!! This mutates the bindings of the spec block passed in, should it
// be making a copy instead (at least by default, perhaps with performance
// junkies saying `construct/rebind` or something like that?
//
// !!! /ONLY should be done with a "predicate", e.g. `construct .quote [...]`
{
    INCLUDE_PARAMS_OF_CONSTRUCT;

    REBVAL *spec = ARG(spec);
    REBCTX *parent = REF(with) ? VAL_CONTEXT(ARG(with)) : nullptr;

    // This parallels the code originally in CONSTRUCT.  Run it if the /ONLY
    // refinement was passed in.
    //
    if (REF(only)) {
        Init_Object(
            D_OUT,
            Construct_Context_Managed(
                REB_OBJECT,
                VAL_ARRAY_AT_MUTABLE_HACK(spec),  // warning: modifies binding!
                VAL_SPECIFIER(spec),
                parent
            )
        );
        return D_OUT;
    }

    // Scan the object for top-level set words in order to make an
    // appropriately sized context.
    //
    REBCTX *ctx = Make_Selfish_Context_Detect_Managed(
        parent ? CTX_TYPE(parent) : REB_OBJECT,  // !!! Presume object?
        VAL_ARRAY_AT(spec),
        parent
    );
    Init_Object(D_OUT, ctx);  // GC protects context

    // !!! This binds the actual body data, not a copy of it.  See
    // Virtual_Bind_Deep_To_New_Context() for future directions.
    //
    Bind_Values_Deep(VAL_ARRAY_AT_ENSURE_MUTABLE(spec), CTX_ARCHETYPE(ctx));

    DECLARE_LOCAL (dummy);
    if (Do_Any_Array_At_Throws(dummy, spec, SPECIFIED)) {
        Move_Value(D_OUT, dummy);
        return R_THROWN;  // evaluation result ignored unless thrown
    }

    return D_OUT;
}
