//
//  File: %c-bind.c
//  Summary: "Word Binding Routines"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Ren-C Open Source Contributors
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
// Binding relates a word to a context.  Every word can be either bound,
// specifically bound to a particular context, or bound relatively to a
// function (where additional information is needed in order to find the
// specific instance of the variable for that word as a key).
//

#include "sys-core.h"


//
//  Bind_Values_Inner_Loop: C
//
// Bind_Values_Core() sets up the binding table and then calls
// this recursive routine to do the actual binding.
//
void Bind_Values_Inner_Loop(
    struct Reb_Binder *binder,
    RELVAL *head,
    const RELVAL *tail,
    REBCTX *context,
    REBU64 bind_types, // !!! REVIEW: force word types low enough for 32-bit?
    REBU64 add_midstream_types,
    REBFLGS flags
){
    RELVAL *v = head;
    for (; v != tail; ++v) {
        REBCEL(const*) cell = VAL_UNESCAPED(v);
        enum Reb_Kind heart = CELL_HEART(cell);

        // !!! Review use of `heart` bit here, e.g. when a REB_PATH has an
        // REB_BLOCK heart, why would it be bound?  Problem is that if we
        // do not bind `/` when REB_WORD is asked then `/` won't be bound.
        //
        REBU64 type_bit = FLAGIT_KIND(heart);

        if (type_bit & bind_types) {
            const REBCAN *canon = VAL_WORD_CANON(cell);
            REBINT n = Get_Binder_Index_Else_0(binder, canon);
            if (n > 0) {
                //
                // A binder index of 0 should clearly not be bound.  But
                // negative binder indices are also ignored by this process,
                // which provides a feature of building up state about some
                // words while still not including them in the bind.
                //
                assert(cast(REBLEN, n) <= CTX_LEN(context));

                // We're overwriting any previous binding, which may have
                // been relative.

                Bind_Any_Word(v, context, n);
            }
            else if (type_bit & add_midstream_types) {
                //
                // Word is not in context, so add it if option is specified
                //
                Append_Context(context, v, nullptr);
                Add_Binder_Index(binder, canon, VAL_WORD_INDEX(v));
            }
        }
        else if (flags & BIND_DEEP) {
            if (ANY_ARRAY_KIND(heart)) {
                const RELVAL *sub_tail = VAL_ARRAY_TAIL(VAL_UNESCAPED(v));
                RELVAL *sub_at = VAL_ARRAY_AT_MUTABLE_HACK(VAL_UNESCAPED(v));
                Bind_Values_Inner_Loop(
                    binder,
                    sub_at,
                    sub_tail,
                    context,
                    bind_types,
                    add_midstream_types,
                    flags
                );
            }
        }
    }
}


//
//  Bind_Values_Core: C
//
// Bind words in an array of values terminated with END
// to a specified context.  See warnings on the functions like
// Bind_Values_Deep() about not passing just a singular REBVAL.
//
// NOTE: If types are added, then they will be added in "midstream".  Only
// bindings that come after the added value is seen will be bound.
//
void Bind_Values_Core(
    RELVAL *head,
    const RELVAL *tail,
    const RELVAL *context,
    REBU64 bind_types,
    REBU64 add_midstream_types,
    REBFLGS flags // see %sys-core.h for BIND_DEEP, etc.
) {
    struct Reb_Binder binder;
    INIT_BINDER(&binder);

    REBCTX *c = VAL_CONTEXT(context);

    // Associate the canon of a word with an index number.  (This association
    // is done by poking the index into the REBSER of the series behind the
    // ANY-WORD!, so it must be cleaned up to not break future bindings.)
    //
  blockscope {
    REBLEN index = 1;
    const REBKEY *key_tail;
    const REBKEY *key = CTX_KEYS(&key_tail, c);
    const REBVAR *var = CTX_VARS_HEAD(c);
    for (; key != key_tail; key++, var++, index++)
        if (not Is_Var_Hidden(var))
            Add_Binder_Index(&binder, KEY_CANON(key), index);
  }

    Bind_Values_Inner_Loop(
        &binder,
        head,
        tail,
        c,
        bind_types,
        add_midstream_types,
        flags
    );

  blockscope {  // Reset all the binder indices to zero
    const REBKEY *key_tail;
    const REBKEY *key = CTX_KEYS(&key_tail, c);
    const REBVAR *var = CTX_VARS_HEAD(c);
    for (; key != key_tail; ++key, ++var)
        if (not Is_Var_Hidden(var))
            Remove_Binder_Index(&binder, KEY_CANON(key));
  }

    SHUTDOWN_BINDER(&binder);
}


//
//  Unbind_Values_Core: C
//
// Unbind words in a block, optionally unbinding those which are
// bound to a particular target (if target is NULL, then all
// words will be unbound regardless of their VAL_WORD_CONTEXT).
//
void Unbind_Values_Core(
    RELVAL *head,
    const RELVAL *tail,
    option(REBCTX*) context,
    bool deep
){
    RELVAL *v = head;
    for (; v != tail; ++v) {
        //
        // !!! We inefficiently dequote all values just to make sure we don't
        // damage shared bindings; review more efficient means of doing this.
        //
        enum Reb_Kind heart = CELL_HEART(VAL_UNESCAPED(v));

        if (
            ANY_WORD_KIND(heart)
            and (
                not context
                or VAL_WORD_BINDING(v) == CTX_VARLIST(unwrap(context))
            )
        ){
            Unbind_Any_Word(v);
        }
        else if (ANY_ARRAY_KIND(heart) and deep) {
            const RELVAL *sub_tail = VAL_ARRAY_TAIL(v);
            RELVAL *sub_at = VAL_ARRAY_AT_MUTABLE_HACK(v);
            Unbind_Values_Core(sub_at, sub_tail, context, true);
        }
    }
}


//
//  Try_Bind_Word: C
//
// Returns 0 if word is not part of the context, otherwise the index of the
// word in the context.
//
REBLEN Try_Bind_Word(const RELVAL *context, REBVAL *word)
{
    REBLEN n = Find_Canon_In_Context(context, VAL_WORD_CANON(word));
    if (n != 0) {
        Bind_Any_Word(word, VAL_CONTEXT(context), n);
                            // ^-- "may have been relative" (old comment ?)
    }
    return n;
}


//
//  let: native [
//
//  {LET is noticed by FUNC to mean "create a local binding"}
//
//      return: [<invisible>]
//      'word [<skip> word!]
//  ]
//
REBNATIVE(let)
//
// !!! Currently LET is a no-op, but in the future should be able to inject
// new bindings into a code stream as it goes.  The mechanisms for that are
// not yet designed, hence the means for creating new variables is actually
// parallel to how SET-WORD!s were scanned for in R3-Alpha's FUNCTION.
{
    INCLUDE_PARAMS_OF_LET;
    UNUSED(ARG(word));  // just skip over WORD!s (vs. look them up)

    RETURN_INVISIBLE;
}


//
//  Clonify_And_Bind_Relative: C
//
// Recursive function for relative function word binding.  The code for
// Clonify() is merged in for efficiency, because it recurses...and we want
// to do the binding in the same pass.
//
// !!! Since the ultimate desire is to factor out common code, try not to
// constant-fold the Clonify implementation here--to make the factoring clear.
//
// !!! Should this return true if any relative bindings were made?
//
static void Clonify_And_Bind_Relative(
    REBVAL *v,  // Note: incoming value is not relative
    const RELVAL *src,
    REBFLGS flags,
    REBU64 deep_types,
    struct Reb_Binder *binder,
    REBACT *relative,
    REBU64 bind_types,
    REBLEN *param_num  // if not null, gathering LETs (next index for LET)
){
    if (C_STACK_OVERFLOWING(&bind_types))
        Fail_Stack_Overflow();

    if (param_num and IS_WORD(src) and VAL_WORD_ID(src) == SYM_LET) {
        if (IS_WORD(src + 1) or IS_SET_WORD(src + 1)) {
            const REBCAN *canon = VAL_WORD_CANON(src + 1);
            if (Try_Add_Binder_Index(binder, canon, *param_num)) {
                Init_Word(DS_PUSH(), canon);
                ++(*param_num);
            }
            else {
                // !!! Should double LETs be an error?  With virtual binding
                // it would override, but we can't do that now...so it may
                // be better to just prohibit it.
            }
        }

        // !!! We don't actually add the new words as we go, but rather all at
        // once from the stack.  This may be superfluous, and we could use
        // regular appends and trust the expansion logic.
    }

    assert(flags & NODE_FLAG_MANAGED);

    // !!! Could theoretically do what COPY does and generate a new hijackable
    // identity.  There's no obvious use for this; hence not implemented.
    //
    assert(not (deep_types & FLAGIT_KIND(REB_ACTION)));

    // !!! It may be possible to do this faster/better, the impacts on higher
    // quoting levels could be incurring more cost than necessary...but for
    // now err on the side of correctness.  Unescape the value while cloning
    // and then escape it back.
    //
    REBLEN num_quotes = VAL_NUM_QUOTES(v);
    Dequotify(v);

    enum Reb_Kind kind = cast(enum Reb_Kind, KIND3Q_BYTE_UNCHECKED(v));
    assert(kind < REB_MAX_PLUS_MAX);  // we dequoted it (pseudotypes ok)

    enum Reb_Kind heart = CELL_HEART(cast(REBCEL(const*), v));

    if (deep_types & FLAGIT_KIND(kind) & TS_SERIES_OBJ) {
        //
        // Objects and series get shallow copied at minimum
        //
        REBSER *series;
        const RELVAL *sub_src;

        bool would_need_deep;

        if (ANY_CONTEXT_KIND(heart)) {
            INIT_VAL_CONTEXT_VARLIST(
                v,
                CTX_VARLIST(Copy_Context_Shallow_Managed(VAL_CONTEXT(v)))
            );
            series = CTX_VARLIST(VAL_CONTEXT(v));
            sub_src = BLANK_VALUE;  // don't try to look for LETs

            would_need_deep = true;
        }
        else if (ANY_ARRAY_KIND(heart)) {
            series = Copy_Array_At_Extra_Shallow(
                VAL_ARRAY(v),
                0, // !!! what if VAL_INDEX() is nonzero?
                VAL_SPECIFIER(v),
                0,
                NODE_FLAG_MANAGED
            );

            INIT_VAL_NODE1(v, series);  // copies args
            INIT_SPECIFIER(v, UNBOUND);  // copied w/specifier--not relative

            sub_src = VAL_ARRAY_AT(v);  // look for LETs

            // See notes in Clonify()...need to copy immutable paths so that
            // binding pointers can be changed in the "immutable" copy.
            //
            if (ANY_PATH_KIND(kind))
                Freeze_Array_Shallow(ARR(series));

            would_need_deep = true;
        }
        else if (ANY_SERIES_KIND(heart)) {
            series = Copy_Series_Core(
                VAL_SERIES(v),
                NODE_FLAG_MANAGED
            );
            INIT_VAL_NODE1(v, series);
            sub_src = BLANK_VALUE;  // don't try to look for LETs

            would_need_deep = false;
        }
        else {
            would_need_deep = false;
            sub_src = nullptr;
            series = nullptr;
        }

        // If we're going to copy deeply, we go back over the shallow
        // copied series and "clonify" the values in it.
        //
        if (would_need_deep and (deep_types & FLAGIT_KIND(kind))) {
            REBVAL *sub = SPECIFIC(ARR_HEAD(ARR(series)));
            RELVAL *sub_tail = ARR_TAIL(ARR(series));
            for (; sub != sub_tail; ++sub, ++sub_src)
                Clonify_And_Bind_Relative(
                    sub,
                    sub_src,
                    flags,
                    deep_types,
                    binder,
                    relative,
                    bind_types,
                    param_num
                );
        }
    }
    else {
        // We're not copying the value, so inherit the const bit from the
        // original value's point of view, if applicable.
        //
        if (NOT_CELL_FLAG(v, EXPLICITLY_MUTABLE))
            v->header.bits |= (flags & ARRAY_FLAG_CONST_SHALLOW);
    }

    // !!! Review use of `heart` here, in terms of meaning
    //
    if (FLAGIT_KIND(heart) & bind_types) {
        REBINT n = Get_Binder_Index_Else_0(binder, VAL_WORD_CANON(v));
        if (n != 0) {
            //
            // Word' symbol is in frame.  Relatively bind it.  Note that the
            // action bound to can be "incomplete" (LETs still gathering)
            //
            Bind_Any_Word(v, relative, n);
        }
    }
    else if (ANY_ARRAY_OR_PATH_KIND(heart)) {

        // !!! Technically speaking it is not necessary for an array to
        // be marked relative if it doesn't contain any relative words
        // under it.  However, for uniformity in the near term, it's
        // easiest to debug if there is a clear mark on arrays that are
        // part of a deep copy of a function body either way.
        //
        INIT_SPECIFIER(v, relative);  // "incomplete func" (LETs gathering?)
    }

    Quotify_Core(v, num_quotes);  // Quotify() won't work on RELVAL*
}


//
//  Copy_And_Bind_Relative_Deep_Managed: C
//
// This routine is called by Make_Action in order to take the raw material
// given as a function body, and de-relativize any IS_RELATIVE(value)s that
// happen to be in it already (as any Copy does).  But it also needs to make
// new relative references to ANY-WORD! that are referencing function
// parameters, as well as to relativize the copies of ANY-ARRAY! that contain
// these relative words...so that they refer to the archetypal function
// to which they should be relative.
//
REBARR *Copy_And_Bind_Relative_Deep_Managed(
    const REBVAL *body,
    REBACT *relative,
    REBU64 bind_types,
    bool gather_lets
){
    struct Reb_Binder binder;
    INIT_BINDER(&binder);

    REBLEN param_num = 1;

  blockscope {  // Setup binding table from the argument word list
    const REBKEY *tail;
    const REBKEY *key = ACT_KEYS(&tail, relative);
    const REBPAR *param = ACT_PARAMS_HEAD(relative);
    for (; key != tail; ++key, ++param, ++param_num) {
        if (Is_Param_Sealed(param))
            continue;
        Add_Binder_Index(&binder, KEY_CANON(key), param_num);
    }
  }

    REBARR *copy;

  blockscope {
    const REBARR *original = VAL_ARRAY(body);
    REBLEN index = VAL_INDEX(body);
    REBSPC *specifier = VAL_SPECIFIER(body);
    REBLEN tail = VAL_LEN_AT(body);
    assert(tail <= ARR_LEN(original));

    if (index > tail)  // !!! should this be asserted?
        index = tail;

    REBFLGS flags = ARRAY_MASK_HAS_FILE_LINE | NODE_FLAG_MANAGED;
    REBU64 deep_types = (TS_SERIES | TS_PATH) & ~TS_NOT_COPIED;

    REBLEN len = tail - index;

    REBDSP dsp_orig = DSP;

    // Currently we start by making a shallow copy and then adjust it

    copy = Make_Array_For_Copy(len, flags, original);

    const RELVAL *src = ARR_AT(original, index);
    RELVAL *dest = ARR_HEAD(copy);
    REBLEN count = 0;
    for (; count < len; ++count, ++dest, ++src) {
        Clonify_And_Bind_Relative(
            Derelativize(dest, src, specifier),
            src,
            flags | NODE_FLAG_MANAGED,
            deep_types,
            &binder,
            relative,
            bind_types,
            gather_lets
                ? &param_num  // next bind index for a LET to use
                : nullptr
        );
    }

    SET_SERIES_LEN(copy, len);

    if (gather_lets) {
        //
        // Extend the paramlist with any LET variables we gathered...
        //
        REBLEN num_lets = DSP - dsp_orig;
        if (num_lets != 0) {
            //
            // !!! We can only clear this flag because Make_Paramlist_Managed()
            // created the array *without* SERIES_FLAG_FIXED_SIZE, but then
            // added it after the fact.  If at Make_Array() time you pass in the
            // flag, then the cells will be formatted such that the flag cannot
            // be taken off.
            //
            REBSER *keylist = ACT_KEYLIST(relative);

            REBARR *paramlist = ACT_PARAMLIST(relative);
            assert(GET_SERIES_FLAG(paramlist, FIXED_SIZE));
            CLEAR_SERIES_FLAG(paramlist, FIXED_SIZE);

            REBLEN old_keylist_len = SER_USED(keylist);
            EXPAND_SERIES_TAIL(keylist, num_lets);
            EXPAND_SERIES_TAIL(paramlist, num_lets);
            REBKEY *key = SER_AT(REBKEY, keylist, old_keylist_len);
            RELVAL *param = ARR_AT(paramlist, old_keylist_len + 1);

            REBDSP dsp = dsp_orig;
            while (dsp != DSP) {
                const REBCAN *canon = VAL_WORD_CANON(DS_AT(dsp + 1));
                Init_Key(key, canon);
                ++dsp;
                ++key;

                Init_Void(param, SYM_UNSET);
                SET_CELL_FLAG(param, VAR_MARKED_HIDDEN);
                ++param;

                // Will be removed from binder below
            }
            DS_DROP_TO(dsp_orig);

            SET_SERIES_LEN(keylist, old_keylist_len + num_lets);

            SET_SERIES_LEN(paramlist, old_keylist_len + num_lets + 1);
            SET_SERIES_FLAG(paramlist, FIXED_SIZE);
        }
    }
  }

  blockscope {  // Reset binding table
    const REBKEY *tail;
    const REBKEY *key = ACT_KEYS(&tail, relative);
    const REBPAR *param = ACT_PARAMS_HEAD(relative);
    for (; key != tail; ++key, ++param) {
        if (Is_Param_Sealed(param))
            continue;

        Remove_Binder_Index(&binder, KEY_CANON(key));
    }
  }

    SHUTDOWN_BINDER(&binder);
    return copy;
}


//
//  Rebind_Values_Deep: C
//
// Rebind all words that reference src target to dst target.
// Rebind is always deep.
//
void Rebind_Values_Deep(
    RELVAL *head,
    const RELVAL *tail,
    REBCTX *from,
    REBCTX *to,
    option(struct Reb_Binder*) binder
) {
    RELVAL *v = head;
    for (; v != tail; ++v) {
        if (ANY_ARRAY_OR_PATH(v)) {
            const RELVAL *sub_tail = VAL_ARRAY_TAIL(v);
            RELVAL *sub_at = VAL_ARRAY_AT_MUTABLE_HACK(v);
            Rebind_Values_Deep(sub_at, sub_tail, from, to, binder);
        }
        else if (ANY_WORD(v) and VAL_WORD_BINDING(v) == CTX_VARLIST(from)) {
            INIT_VAL_WORD_BINDING(v, CTX_VARLIST(to));

            if (binder) {
                REBLEN updated = Get_Binder_Index_Else_0(
                    unwrap(binder),
                    VAL_WORD_CANON(v)
                );
                Bind_Any_Word(v, to, updated);
            }
            else {
                REBLEN index = VAL_WORD_PRIMARY_INDEX_UNCHECKED(v);
                Bind_Any_Word(v, to, index);
            }
        }
        else if (IS_ACTION(v)) {
            //
            // !!! This is a new take on R3-Alpha's questionable feature of
            // deep copying function bodies and rebinding them when a
            // derived object was made.  Instead, if a function is bound to
            // a "base class" of the object we are making, that function's
            // binding pointer (in the function's value cell) is changed to
            // be this object.
            //
            REBCTX *stored = VAL_ACTION_BINDING(v);
            if (stored == UNBOUND) {
                //
                // Leave NULL bindings alone.  Hence, unlike in R3-Alpha, an
                // ordinary FUNC won't forward its references.  An explicit
                // BIND to an object must be performed, or METHOD should be
                // used to do it implicitly.
            }
            else if (REB_FRAME == CTX_TYPE(stored)) {
                //
                // Leave bindings to frame alone, e.g. RETURN's definitional
                // reference...may be an unnecessary optimization as they
                // wouldn't match any derivation since there are no "derived
                // frames" (would that ever make sense?)
            }
            else {
                if (Is_Overriding_Context(stored, to))
                    INIT_VAL_ACTION_BINDING(v, to);
                else {
                    // Could be bound to a reified frame context, or just
                    // to some other object not related to this derivation.
                }
            }
        }
    }
}


//
//  Virtual_Bind_Deep_To_New_Context: C
//
// Looping constructs which are parameterized by WORD!s to set each time
// through the loop must copy the body in R3-Alpha's model.  For instance:
//
//    for-each [x y] [1 2 3] [print ["this body must be copied for" x y]]
//
// The reason is because the context in which X and Y live does not exist
// prior to the execution of the FOR-EACH.  And if the body were destructively
// rebound, then this could mutate and disrupt bindings of code that was
// intended to be reused.
//
// (Note that R3-Alpha was somewhat inconsistent on the idea of being
// sensitive about non-destructively binding arguments in this way.
// MAKE OBJECT! purposefully mutated bindings in the passed-in block.)
//
// The context is effectively an ordinary object, and outlives the loop:
//
//     x-word: none
//     for-each x [1 2 3] [x-word: 'x, break]
//     get x-word  ; returns 3
//
// Ren-C adds a feature of letting LIT-WORD!s be used to indicate that the
// loop variable should be written into the existing bound variable that the
// LIT-WORD! specified.  If all loop variables are of this form, then no
// copy will be made.
//
// !!! Loops should probably free their objects by default when finished
//
void Virtual_Bind_Deep_To_New_Context(
    REBVAL *body_in_out, // input *and* output parameter
    REBCTX **context_out,
    const REBVAL *spec
){
    assert(IS_BLOCK(body_in_out) or IS_SYM_BLOCK(body_in_out));

    REBLEN num_vars = IS_BLOCK(spec) ? VAL_LEN_AT(spec) : 1;
    if (num_vars == 0)
        fail (spec);  // !!! should fail() take unstable?

    const RELVAL *item;

    REBSPC *specifier;
    bool rebinding;
    if (IS_BLOCK(spec)) {  // walk the block for errors BEFORE making binder
        specifier = VAL_SPECIFIER(spec);
        item = VAL_ARRAY_AT(spec);

        const RELVAL *tail;
        const RELVAL *check = VAL_ARRAY_AT_T(&tail, spec);

        rebinding = false;
        for (; check != tail; ++check) {
            if (IS_BLANK(check)) {
                // Will be transformed into dummy item, no rebinding needed
            }
            else if (IS_WORD(check))
                rebinding = true;
            else if (not IS_QUOTED_WORD(check)) {
                //
                // Better to fail here, because if we wait until we're in
                // the middle of building the context, the managed portion
                // (keylist) would be incomplete and tripped on by the GC if
                // we didn't do some kind of workaround.
                //
                fail (Error_Bad_Value_Core(check, specifier));
            }
        }
    }
    else {
        item = spec;
        specifier = SPECIFIED;
        rebinding = IS_WORD(item);
    }

    // Keylists are always managed, but varlist is unmanaged by default (so
    // it can be freed if there is a problem)
    //
    *context_out = Alloc_Context(REB_OBJECT, num_vars);

    REBCTX *c = *context_out; // for convenience...

    // We want to check for duplicates and a Binder can be used for that
    // purpose--but note that a fail() cannot happen while binders are
    // in effect UNLESS the BUF_COLLECT contains information to undo it!
    // There's no BUF_COLLECT here, so don't fail while binder in effect.
    //
    struct Reb_Binder binder;
    if (rebinding)
        INIT_BINDER(&binder);

    const REBSYM *duplicate = nullptr;

    SYMID dummy_sym = SYM_DUMMY1;

    REBLEN index = 1;
    while (index <= num_vars) {
        const REBCAN *canon;

        if (IS_BLANK(item)) {
            if (dummy_sym == SYM_DUMMY9)
                fail ("Current limitation: only up to 9 BLANK! keys");

            canon = Canon(dummy_sym);
            dummy_sym = cast(SYMID, cast(int, dummy_sym) + 1);

            REBVAL *var = Append_Context(c, nullptr, canon);
            Init_Blank(var);
            Hide_Param(var);
            SET_CELL_FLAG(var, BIND_NOTE_REUSE);
            SET_CELL_FLAG(var, PROTECTED);

            goto add_binding_for_check;
        }
        else if (IS_WORD(item)) {
            canon = VAL_WORD_CANON(item);
            REBVAL *var = Append_Context(c, nullptr, canon);

            // !!! For loops, nothing should be able to be aware of this
            // synthesized variable until the loop code has initialized it
            // with something.  However, in case any other code gets run,
            // it can't be left trash...so we'd need it to be at least an
            // unreadable void.  But since this code is also shared with USE,
            // it doesn't do any initialization...so go ahead and put void.
            //
            Init_Void(var, SYM_VOID);

            assert(rebinding); // shouldn't get here unless we're rebinding

            if (not Try_Add_Binder_Index(&binder, canon, index)) {
                //
                // We just remember the first duplicate, but we go ahead
                // and fill in all the keylist slots to make a valid array
                // even though we plan on failing.  Duplicates count as a
                // problem even if they are LIT-WORD! (negative index) as
                // `for-each [x 'x] ...` is paradoxical.
                //
                if (duplicate == nullptr)
                    duplicate = canon;
            }
        }
        else {
            assert(IS_QUOTED_WORD(item)); // checked previously

            // A LIT-WORD! indicates that we wish to use the original binding.
            // So `for-each 'x [1 2 3] [...]` will actually set that x
            // instead of creating a new one.
            //
            // !!! Enumerations in the code walks through the context varlist,
            // setting the loop variables as they go.  It doesn't walk through
            // the array the user gave us, so if it's a LIT-WORD! the
            // information is lost.  Do a trick where we put the LIT-WORD!
            // itself into the slot, and give it NODE_FLAG_MARKED...then
            // hide it from the context and binding.
            //
            canon = VAL_WORD_CANON(VAL_UNESCAPED(item));

          blockscope {
            REBVAL *var = Append_Context(c, nullptr, canon);
            Hide_Param(var);
            Derelativize(var, item, specifier);
            SET_CELL_FLAG(var, BIND_NOTE_REUSE);
            SET_CELL_FLAG(var, PROTECTED);
          }

          add_binding_for_check:

            // We don't want to stop `for-each ['x 'x] ...` necessarily,
            // because if we're saying we're using the existing binding they
            // could be bound to different things.  But if they're not bound
            // to different things, the last one in the list gets the final
            // assignment.  This would be harder to check against, but at
            // least allowing it doesn't make new objects with duplicate keys.
            // For now, don't bother trying to use a binder or otherwise to
            // stop it.
            //
            // However, `for-each [x 'x] ...` is intrinsically contradictory.
            // So we use negative indices in the binder, which the binding
            // process will ignore.
            //
            if (rebinding) {
                REBINT stored = Get_Binder_Index_Else_0(&binder, canon);
                if (stored > 0) {
                    if (duplicate == nullptr)
                        duplicate = canon;
                }
                else if (stored == 0) {
                    Add_Binder_Index(&binder, canon, -1);
                }
                else {
                    assert(stored == -1);
                }
            }
        }

        ++item;
        ++index;
    }

    // As currently written, the loop constructs which use these contexts
    // will hold pointers into the arrays across arbitrary user code running.
    // If the context were allowed to expand, then this can cause memory
    // corruption:
    //
    // https://github.com/rebol/rebol-issues/issues/2274
    //
    SET_SERIES_FLAG(CTX_VARLIST(c), DONT_RELOCATE);

    // !!! In virtual binding, there would not be a Bind_Values call below;
    // so it wouldn't necessarily be required to manage the augmented
    // information.  For now it's a requirement for any references that
    // might be found...and INIT_BINDING_MAY_MANAGE() won't auto-manage
    // things unless they are stack-based.  Virtual bindings will be, but
    // contexts like this won't.
    //
    Manage_Series(CTX_VARLIST(c));

    if (not rebinding)
        return; // nothing else needed to do

    if (not duplicate) {
        //
        // This is effectively `Bind_Values_Deep(ARR_HEAD(body_out), context)`
        // but we want to reuse the binder we had anyway for detecting the
        // duplicates.
        //
        Virtual_Bind_Deep_To_Existing_Context(
            body_in_out,
            c,
            &binder,
            REB_WORD
        );
    }

    // Must remove binder indexes for all words, even if about to fail
    //
  blockscope {
    const REBKEY *tail;
    const REBKEY *key = CTX_KEYS(&tail, c);
    REBVAL *var = CTX_VARS_HEAD(c); // only needed for debug, optimized out
    for (; key != tail; ++key, ++var) {
        REBINT stored = Remove_Binder_Index_Else_0(
            &binder, KEY_CANON(key)
        );
        if (stored == 0)
            assert(duplicate);
        else if (stored > 0)
            assert(NOT_CELL_FLAG(var, BIND_NOTE_REUSE));
        else
            assert(GET_CELL_FLAG(var, BIND_NOTE_REUSE));
    }
  }

    SHUTDOWN_BINDER(&binder);

    if (duplicate) {
        DECLARE_LOCAL (word);
        Init_Word(word, duplicate);
        fail (Error_Dup_Vars_Raw(word));
    }
}


//
//  Virtual_Bind_Deep_To_Existing_Context: C
//
void Virtual_Bind_Deep_To_Existing_Context(
    REBVAL *any_array,
    REBCTX *context,
    struct Reb_Binder *binder,
    enum Reb_Kind kind
){
    // Most of the time if the context isn't trivially small then it's
    // probably best to go ahead and cache bindings.
    //
    UNUSED(binder);

/*
    // Bind any SET-WORD!s in the supplied code block into the FRAME!, so
    // e.g. APPLY 'APPEND [VALUE: 10]` will set VALUE in exemplar to 10.
    //
    // !!! Today's implementation mutates the bindings on the passed-in block,
    // like R3-Alpha's MAKE OBJECT!.  See Virtual_Bind_Deep_To_New_Context()
    // for potential future directions.
    //
    Bind_Values_Inner_Loop(
        &binder,
        VAL_ARRAY_AT_MUTABLE_HACK(ARG(def)),  // mutates bindings
        exemplar,
        FLAGIT_KIND(REB_SET_WORD),  // types to bind (just set-word!),
        0,  // types to "add midstream" to binding as we go (nothing)
        BIND_DEEP
    );
 */

    Virtual_Bind_Patchify(any_array, context, kind);
}


//
//  Init_Interning_Binder: C
//
// The global "binding table" is actually now pieces of data that live on the
// series nodes that store UTF-8 data for words.  This creates a mapping from
// canon word spellings to signed integers.
//
// For the purposes of binding to the user and lib contexts relatively
// quickly, this sets up that global binding table for all lib context words
// at negative integers, and all user context words at positive ones.
//
void Init_Interning_Binder(
    struct Reb_Binder *binder,
    REBCTX *ctx // location to bind into (in addition to lib)
){
    INIT_BINDER(binder);

    // Use positive numbers for all the keys in the context.
    //
  blockscope {
    const REBKEY *tail;
    const REBKEY *key = CTX_KEYS(&tail, ctx);
    REBINT index = 1;
    for (; key != tail; ++key, ++index)
        Add_Binder_Index(binder, KEY_CANON(key), index);  // positives
  }

    // For all the keys that aren't in the supplied context but *are* in lib,
    // use a negative index to locate its position in lib.  Its meaning can be
    // "imported" from there to the context, and adjusted in the binder to the
    // new positive index.
    //
    if (ctx != VAL_CONTEXT(Lib_Context)) {
        const REBKEY *tail;
        const REBKEY *key = CTX_KEYS(&tail, VAL_CONTEXT(Lib_Context));
        REBINT index = 1;
        for (; key != tail; ++key, ++index) {
            const REBCAN *canon = KEY_CANON(key);
            REBINT n = Get_Binder_Index_Else_0(binder, canon);
            if (n == 0)
                Add_Binder_Index(binder, canon, - index);
        }
    }
}


//
//  Shutdown_Interning_Binder: C
//
// This will remove the bindings added in Init_Interning_Binder, along with
// any other bindings which were incorporated along the way to positives.
//
void Shutdown_Interning_Binder(struct Reb_Binder *binder, REBCTX *ctx)
{
    // All of the user context keys should be positive, and removable
    //
  blockscope {
    const REBKEY *tail;
    const REBKEY *key = CTX_KEYS(&tail, ctx);
    REBINT index = 1;
    for (; key != tail; ++key, ++index) {
        REBINT n = Remove_Binder_Index_Else_0(binder, KEY_CANON(key));
        assert(n == index);
        UNUSED(n);
    }
  }

    // The lib context keys may have been imported, so you won't necessarily
    // find them in the list any more.
    //
    if (ctx != VAL_CONTEXT(Lib_Context)) {
        const REBKEY *tail;
        const REBKEY *key = CTX_KEYS(&tail, VAL_CONTEXT(Lib_Context));
        REBINT index = 1;
        for (; key != tail; ++key, ++index) {
            REBINT n = Remove_Binder_Index_Else_0(binder, KEY_CANON(key));
            assert(n == 0 or n == -index);
            UNUSED(n);
        }
    }

    SHUTDOWN_BINDER(binder);
}
