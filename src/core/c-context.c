//
//  File: %c-context.c
//  Summary: "Management routines for ANY-CONTEXT! key/value storage"
//  Section: core
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
// See comments in %sys-context.h for details on how contexts work.
//

#include "sys-core.h"


//
//  Alloc_Context_Core: C
//
// Create context with capacity, allocating space for both words and values.
// Context will report actual CTX_LEN() of 0 after this call.
//
REBCTX *Alloc_Context_Core(enum Reb_Kind kind, REBLEN capacity, REBFLGS flags)
{
    assert(not (flags & ARRAY_FLAG_HAS_FILE_LINE_UNMASKED));  // LINK is taken

    REBARR *keylist = Make_Array_Core(
        capacity + 1,  // size + room for rootkey
        SERIES_MASK_KEYLIST | NODE_FLAG_MANAGED  // always shareable
    );
    RELVAL *rootkey = Alloc_Tail_Array(keylist);
    Init_Unreadable_Void(rootkey);  // reserved for future use
    LINK_ANCESTOR_NODE(keylist) = NOD(keylist);  // default to keylist itself

    REBARR *varlist = Make_Array_Core(
        capacity + 1,  // size + room for rootvar
        SERIES_MASK_VARLIST  // includes assurance of dynamic allocation
            | flags  // e.g. NODE_FLAG_MANAGED
    );
    MISC_META_NODE(varlist) = nullptr;  // GC sees meta object, must init
    INIT_CTX_KEYLIST_UNIQUE(CTX(varlist), keylist);  // starts out unique

    RELVAL *rootvar = STABLE(Alloc_Tail_Array(varlist));
    INIT_VAL_CONTEXT_ROOTVAR(rootvar, kind, varlist);

    return CTX(varlist);  // varlist pointer is context handle
}


//
//  Expand_Context_Keylist_Core: C
//
// Returns whether or not the expansion invalidated existing keys.
//
bool Expand_Context_Keylist_Core(REBCTX *context, REBLEN delta)
{
    REBARR *keylist = CTX_KEYLIST(context);

    assert(NOT_ARRAY_FLAG(keylist, IS_DETAILS)); // can't expand FRAME! list

    if (GET_SERIES_INFO(keylist, KEYLIST_SHARED)) {
        //
        // INIT_CTX_KEYLIST_SHARED was used to set the flag that indicates
        // this keylist is shared with one or more other contexts.  Can't
        // expand the shared copy without impacting the others, so break away
        // from the sharing group by making a new copy.
        //
        // (If all shared copies break away in this fashion, then the last
        // copy of the dangling keylist will be GC'd.)
        //
        // Keylists are only typesets, so no need for a specifier.

        REBARR *copy = Copy_Array_Extra_Shallow(keylist, SPECIFIED, delta);

        // Preserve link to ancestor keylist.  Note that if it pointed to
        // itself, we update this keylist to point to itself.
        //
        // !!! Any extant derivations to the old keylist will still point to
        // that keylist at the time the derivation was performed...it will not
        // consider this new keylist to be an ancestor match.  Hence expanded
        // objects are essentially all new objects as far as derivation are
        // concerned, though they can still run against ancestor methods.
        //
        if (LINK_ANCESTOR(keylist) == keylist)
            LINK_ANCESTOR_NODE(copy) = NOD(copy);
        else
            LINK_ANCESTOR_NODE(copy) = LINK_ANCESTOR_NODE(keylist);

        Manage_Array(copy);
        INIT_CTX_KEYLIST_UNIQUE(context, copy);

        return true;
    }

    if (delta == 0)
        return false;

    // INIT_CTX_KEYLIST_UNIQUE was used to set this keylist in the
    // context, and no INIT_CTX_KEYLIST_SHARED was used by another context
    // to mark the flag indicating it's shared.  Extend it directly.

    Extend_Series(SER(keylist), delta);
    TERM_ARRAY_LEN(keylist, ARR_LEN(keylist));

    return false;
}


//
//  Expand_Context: C
//
// Expand a context. Copy words if keylist is not unique.
//
void Expand_Context(REBCTX *context, REBLEN delta)
{
    // varlist is unique to each object--expand without making a copy.
    //
    Extend_Series(SER(CTX_VARLIST(context)), delta);
    TERM_ARRAY_LEN(CTX_VARLIST(context), ARR_LEN(CTX_VARLIST(context)));

    Expand_Context_Keylist_Core(context, delta);
}


//
//  Append_Context: C
//
// Append a word to the context word list. Expands the list if necessary.
// Returns the value cell for the word.  The new variable is unset by default.
//
// !!! Review if it would make more sense to use TRASH.
//
// If word is not NULL, use the word sym and bind the word value, otherwise
// use sym.  When using a word, it will be modified to be specifically bound
// to this context after the operation.
//
// !!! Should there be a clearer hint in the interface, with a REBVAL* out,
// to give a fully bound value as a result?  Given that the caller passed
// in the context and can get the index out of a relatively bound word,
// they usually likely don't need the result directly.
//
REBVAL *Append_Context(
    REBCTX *context,
    option(unstable RELVAL*) any_word,
    option(const REBSTR*) spelling
) {
    REBARR *keylist = CTX_KEYLIST(context);

    // Add the key to key list
    //
    // !!! This doesn't seem to consider the shared flag of the keylist (?)
    // though the callsites seem to pre-expand with consideration for that.
    // Review why this is expanding when the callers are expanding.  Should
    // also check that redundant keys aren't getting added here.
    //
    EXPAND_SERIES_TAIL(SER(keylist), 1);
    Init_Context_Key(
        ARR_LAST(keylist),
        spelling ? unwrap(spelling) : VAL_WORD_SPELLING(unwrap(any_word))
    );
    TERM_ARRAY_LEN(keylist, ARR_LEN(keylist));

    // Add a slot to the var list
    //
    EXPAND_SERIES_TAIL(SER(CTX_VARLIST(context)), 1);

    REBVAL *value = Init_Void(ARR_LAST(CTX_VARLIST(context)), SYM_UNSET);
    TERM_ARRAY_LEN(CTX_VARLIST(context), ARR_LEN(CTX_VARLIST(context)));

    if (not any_word)
        assert(spelling);
    else {
        // We want to not just add a key/value pairing to the context, but we
        // want to bind a word while we are at it.  Make sure symbol is valid.
        //
        assert(not spelling);

        REBLEN len = CTX_LEN(context); // length we just bumped
        INIT_VAL_WORD_BINDING(unwrap(any_word), context);
        INIT_WORD_INDEX(unwrap(any_word), len);
    }

    return value;  // location we just added (void cell)
}


//
//  Collect_Start: C
//
// Begin using a "binder" to start mapping canon symbol names to integer
// indices.  Use Collect_End() to free the map.
//
// WARNING: This routine uses the shared BUF_COLLECT rather than
// targeting a new series directly.  This way a context can be
// allocated at exactly the right length when contents are copied.
// Therefore do not call code that might call BIND or otherwise
// make use of the Bind_Table or BUF_COLLECT.
//
void Collect_Start(struct Reb_Collector* collector, REBFLGS flags)
{
    collector->flags = flags;
    collector->dsp_orig = DSP;
    collector->index = 1;
    INIT_BINDER(&collector->binder);

    assert(ARR_LEN(BUF_COLLECT) == 0); // should be empty
}


//
//  Grab_Collected_Array_Managed: C
//
REBARR *Grab_Collected_Array_Managed(
    struct Reb_Collector *collector,
    REBFLGS flags
){
    UNUSED(collector); // not needed at the moment

    // We didn't terminate as we were collecting, so terminate now.
    //
    TERM_ARRAY_LEN(BUF_COLLECT, ARR_LEN(BUF_COLLECT));

    // If no new words, prior context.  Note length must include the slot
    // for the rootkey...and note also this means the rootkey cell *may*
    // be shared between all keylists when you pass in a prior.
    //
    // All collected values should have been fully specified.
    //
    return Copy_Array_Shallow_Flags(
        BUF_COLLECT,
        SPECIFIED,
        flags | NODE_FLAG_MANAGED
    );
}


//
//  Collect_End: C
//
// Reset the bind markers in the canon series nodes so they can be reused,
// and empty the BUF_COLLECT.
//
void Collect_End(struct Reb_Collector *cl)
{
    // We didn't terminate as we were collecting, so terminate now.
    //
    TERM_ARRAY_LEN(BUF_COLLECT, ARR_LEN(BUF_COLLECT));

    // Reset binding table (note BUF_COLLECT may have expanded)
    //
    unstable RELVAL *v =
        (cl == NULL or (cl->flags & COLLECT_AS_TYPESET))
            ? ARR_HEAD(BUF_COLLECT) + 1
            : ARR_HEAD(BUF_COLLECT);
    for (; NOT_END(v); ++v) {
        const REBSTR *canon =
            (cl == NULL or (cl->flags & COLLECT_AS_TYPESET))
                ? VAL_KEY_SPELLING(v)
                : VAL_WORD_SPELLING(v);

        if (cl != NULL) {
            Remove_Binder_Index(&cl->binder, canon);
            continue;
        }

        // !!! This doesn't have a "binder" available to clear out the
        // keys with.  The nature of handling error states means that if
        // a thread-safe binding system was implemented, we'd have to know
        // which thread had the error to roll back any binding structures.
        // For now just zero it out based on the collect buffer.
        //
        assert(
            MISC(canon).bind_index.high != 0
            or MISC(canon).bind_index.low != 0
        );
        MISC(canon).bind_index.high = 0;
        MISC(canon).bind_index.low = 0;
    }

    SET_ARRAY_LEN_NOTERM(BUF_COLLECT, 0);

    if (cl != NULL)
        SHUTDOWN_BINDER(&cl->binder);
}


//
//  Collect_Context_Keys: C
//
// Collect keys from a prior context into BUF_COLLECT for a new context.
//
void Collect_Context_Keys(
    struct Reb_Collector *cl,
    REBCTX *context,
    bool check_dups // check for duplicates (otherwise assume unique)
){
    assert(cl->flags & COLLECT_AS_TYPESET);

    REBVAL *key = CTX_KEYS_HEAD(context);

    assert(cl->index >= 1); // 0 in bind table means "not present"

    // This is necessary so Blit_Relative() below isn't overwriting memory that
    // BUF_COLLECT does not own.  (It may make the buffer capacity bigger than
    // necessary if duplicates are found, but the actual buffer length will be
    // set correctly by the end.)
    //
    EXPAND_SERIES_TAIL(SER(BUF_COLLECT), CTX_LEN(context));
    SET_ARRAY_LEN_NOTERM(BUF_COLLECT, cl->index);

    unstable RELVAL *collect = ARR_TAIL(BUF_COLLECT);  // *after* expansion

    if (check_dups) {
        for (; NOT_END(key); key++) {
            const REBSTR *symbol = VAL_KEY_SPELLING(key);
            if (not Try_Add_Binder_Index(&cl->binder, symbol, cl->index))
                continue; // don't collect if already in bind table

            ++cl->index;

            Blit_Relative(collect, key); // fast copy, matching cell formats
            ++collect;
        }

        // Mark length of BUF_COLLECT by how far `collect` advanced
        // (would be 0 if all the keys were duplicates...)
        //
        SET_ARRAY_LEN_NOTERM(
            BUF_COLLECT,
            ARR_LEN(BUF_COLLECT) + (collect - ARR_TAIL(BUF_COLLECT))
        );
    }
    else {
        // Optimized add of all keys to bind table and collect buffer.
        //
        for (; NOT_END(key); ++key, ++collect, ++cl->index) {
            Blit_Relative(collect, key);
            Add_Binder_Index(&cl->binder, VAL_KEY_SPELLING(key), cl->index);
        }
        SET_ARRAY_LEN_NOTERM(
            BUF_COLLECT, ARR_LEN(BUF_COLLECT) + CTX_LEN(context)
        );
    }

    // BUF_COLLECT doesn't get terminated as its being built, but it gets
    // terminated in Collect_Keys_End()
}


//
//  Collect_Inner_Loop: C
//
// The inner recursive loop used for collecting context keys or ANY-WORD!s.
//
static void Collect_Inner_Loop(
    struct Reb_Collector *cl,
    unstable const RELVAL *head
){
    for (; NOT_END(head); ++head) {
        unstable REBCEL(const*) cell = VAL_UNESCAPED(head);  // X from ''''X
        enum Reb_Kind kind = CELL_KIND(cell);

        if (ANY_WORD_KIND(kind)) {
            if (kind != REB_SET_WORD and not (cl->flags & COLLECT_ANY_WORD))
                continue; // kind of word we're not interested in collecting

            const REBSTR *symbol = VAL_WORD_SPELLING(cell);
            if (not Try_Add_Binder_Index(&cl->binder, symbol, cl->index)) {
                if (cl->flags & COLLECT_NO_DUP) {
                    DECLARE_LOCAL (duplicate);
                    Init_Word(duplicate, VAL_WORD_SPELLING(cell));
                    fail (Error_Dup_Vars_Raw(duplicate)); // cleans bindings
                }
                continue; // tolerate duplicate
            }

            ++cl->index;

            EXPAND_SERIES_TAIL(SER(BUF_COLLECT), 1);
            if (cl->flags & COLLECT_AS_TYPESET)
                Init_Context_Key(
                    ARR_LAST(BUF_COLLECT),
                    VAL_WORD_SPELLING(cell)
                );
            else
                Init_Word(ARR_LAST(BUF_COLLECT), VAL_WORD_SPELLING(cell));

            continue;
        }

        if (not (cl->flags & COLLECT_DEEP))
            continue;

        // !!! Should this consider paths, or their embedded groups/arrays?
        // This is less certain as the purpose of collect words is not clear
        // given stepping away from SET-WORD! gathering as locals.
        // https://github.com/rebol/rebol-issues/issues/2276
        //
        if (ANY_ARRAY_KIND(kind))
            Collect_Inner_Loop(cl, VAL_ARRAY_AT(cell));
    }
}


//
//  Collect_Keylist_Managed: C
//
// Scans a block for words to extract and make into typeset keys to go in
// a context.  The Bind_Table is used to quickly determine duplicate entries.
//
// A `prior` context can be provided to serve as a basis; all the keys in
// the prior will be returned, with only new entries contributed by the
// data coming from the head[] array.  If no new values are needed (the
// array has no relevant words, or all were just duplicates of words already
// in prior) then then `prior`'s keylist may be returned.  The result is
// always pre-managed, because it may not be legal to free prior's keylist.
//
// Returns:
//     A block of typesets that can be used for a context keylist.
//     If no new words, the prior list is returned.
//
// !!! There was previously an optimization in object creation which bypassed
// key collection in the case where head[] was empty.  Revisit if it is worth
// the complexity to move handling for that case in this routine.
//
REBARR *Collect_Keylist_Managed(
    unstable const RELVAL *head,
    option(REBCTX*) prior,
    REBFLGS flags // see %sys-core.h for COLLECT_ANY_WORD, etc.
) {
    struct Reb_Collector collector;
    struct Reb_Collector *cl = &collector;

    assert(not (flags & COLLECT_AS_TYPESET)); // not optional, we add it
    Collect_Start(cl, flags | COLLECT_AS_TYPESET);

    // Leave the [0] slot blank while collecting (ROOTKEY/ROOTPARAM), but
    // valid (but "unreadable") bits so that the copy will still work.
    //
    Init_Unreadable_Void(ARR_HEAD(BUF_COLLECT));
    SET_ARRAY_LEN_NOTERM(BUF_COLLECT, 1);

    // Setup binding table with existing words, no need to check duplicates
    //
    if (prior)
        Collect_Context_Keys(cl, unwrap(prior), false);

    // Scan for words, adding them to BUF_COLLECT and bind table:
    Collect_Inner_Loop(cl, head);

    // If new keys were added to the collect buffer (as evidenced by a longer
    // collect buffer than the original keylist) then make a new keylist
    // array, otherwise reuse the original
    //
    REBARR *keylist;
    if (prior and ARR_LEN(CTX_KEYLIST(unwrap(prior))) == ARR_LEN(BUF_COLLECT))
        keylist = CTX_KEYLIST(unwrap(prior));
    else
        keylist = Grab_Collected_Array_Managed(cl, SERIES_MASK_KEYLIST);

    // !!! Usages of the rootkey for non-FRAME! contexts is open for future,
    // but it's set to an unreadable void at the moment just to make sure it
    // doesn't get used on accident.
    //
    ASSERT_UNREADABLE_IF_DEBUG(ARR_HEAD(keylist));

    Collect_End(cl);
    return keylist;
}


//
//  Collect_Unique_Words_Managed: C
//
// Collect unique words from a block, possibly deeply...maybe just SET-WORD!s.
//
REBARR *Collect_Unique_Words_Managed(
    unstable const RELVAL *head,
    REBFLGS flags,  // See COLLECT_XXX
    const REBVAL *ignore  // BLOCK!, ANY-CONTEXT!, or BLANK! for none
){
    // We do not want to fail() during the bind at this point in time (the
    // system doesn't know how to clean up, and the only cleanup it does
    // assumes you were collecting for a keylist...it doesn't have access to
    // the "ignore" bindings.)  Do a pre-pass to fail first, if there are
    // any non-words in a block the user passed in.
    //
    if (not IS_NULLED(ignore)) {
        unstable const RELVAL *check = VAL_ARRAY_AT(ignore);
        for (; NOT_END(check); ++check) {
            if (not ANY_WORD_KIND(CELL_KIND(VAL_UNESCAPED(check))))
                fail (Error_Bad_Value_Core(check, VAL_SPECIFIER(ignore)));
        }
    }

    struct Reb_Collector collector;
    struct Reb_Collector *cl = &collector;

    assert(not (flags & COLLECT_AS_TYPESET)); // only used for making keylists
    Collect_Start(cl, flags);

    assert(ARR_LEN(BUF_COLLECT) == 0); // should be empty

    // The way words get "ignored" in the collecting process is to give them
    // dummy bindings so it appears they've "already been collected", but
    // not actually add them to the collection.  Then, duplicates don't cause
    // an error...so they will just be skipped when encountered.
    //
    if (IS_BLOCK(ignore)) {
        unstable const RELVAL *item = VAL_ARRAY_AT(ignore);
        for (; NOT_END(item); ++item) {
            unstable REBCEL(const*) cell = VAL_UNESCAPED(item);
            const REBSTR *symbol = VAL_WORD_SPELLING(cell);

            // A block may have duplicate words in it (this situation could
            // arise when `function [/test /test] []` calls COLLECT-WORDS
            // and tries to ignore both tests.  Have debug build count the
            // number (overkill, but helps test binders).
            //
            if (not Try_Add_Binder_Index(&cl->binder, symbol, -1)) {
              #if !defined(NDEBUG)
                REBINT i = Get_Binder_Index_Else_0(&cl->binder, symbol);
                assert(i < 0);
                Remove_Binder_Index_Else_0(&cl->binder, symbol);
                Add_Binder_Index(&cl->binder, symbol, i - 1);
              #endif
            }
        }
    }
    else if (ANY_CONTEXT(ignore)) {
        REBVAL *key = CTX_KEYS_HEAD(VAL_CONTEXT(ignore));
        for (; NOT_END(key); ++key) {
            //
            // Shouldn't be possible to have an object with duplicate keys,
            // use plain Add_Binder_Index.
            //
            Add_Binder_Index(&cl->binder, VAL_KEY_SPELLING(key), -1);
        }
    }
    else
        assert(IS_NULLED(ignore));

    Collect_Inner_Loop(cl, head);

    REBARR *array = Grab_Collected_Array_Managed(cl, SERIES_FLAGS_NONE);

    if (IS_BLOCK(ignore)) {
        unstable const RELVAL *item = VAL_ARRAY_AT(ignore);
        for (; NOT_END(item); ++item) {
            unstable REBCEL(const*) cell = VAL_UNESCAPED(item);
            const REBSTR *symbol = VAL_WORD_SPELLING(cell);

          #if !defined(NDEBUG)
            REBINT i = Get_Binder_Index_Else_0(&cl->binder, symbol);
            assert(i < 0);
            if (i != -1) {
                Remove_Binder_Index_Else_0(&cl->binder, symbol);
                Add_Binder_Index(&cl->binder, symbol, i + 1);
                continue;
            }
          #endif

            Remove_Binder_Index(&cl->binder, symbol);
        }
    }
    else if (ANY_CONTEXT(ignore)) {
        REBVAL *key = CTX_KEYS_HEAD(VAL_CONTEXT(ignore));
        for (; NOT_END(key); ++key)
            Remove_Binder_Index(&cl->binder, VAL_KEY_SPELLING(key));
    }
    else
        assert(IS_NULLED(ignore));

    Collect_End(cl);
    return array;
}


//
//  Rebind_Context_Deep: C
//
// Clone old context to new context knowing
// which types of values need to be copied, deep copied, and rebound.
//
void Rebind_Context_Deep(
    REBCTX *source,
    REBCTX *dest,
    option(struct Reb_Binder*) binder
){
    Rebind_Values_Deep(CTX_VARS_HEAD(dest), source, dest, binder);
}


//
//  Make_Context_Detect_Managed: C
//
// Create a context by detecting top-level set-words in an array of values.
// So if the values were the contents of the block `[a: 10 b: 20]` then the
// resulting context would be for two words, `a` and `b`.
//
// Optionally a parent context may be passed in, which will contribute its
// keylist of words to the result if provided.
//
REBCTX *Make_Context_Detect_Managed(
    enum Reb_Kind kind,
    unstable const RELVAL *head,
    option(REBCTX*) parent
) {
    REBARR *keylist = Collect_Keylist_Managed(
        head,
        parent,
        COLLECT_ONLY_SET_WORDS
    );

    REBLEN len = ARR_LEN(keylist);
    REBARR *varlist = Make_Array_Core(
        len,
        SERIES_MASK_VARLIST
            | NODE_FLAG_MANAGED // Note: Rebind below requires managed context
    );
    TERM_ARRAY_LEN(varlist, len);
    MISC_META_NODE(varlist) = nullptr;  // clear meta object (GC sees this)

    REBCTX *context = CTX(varlist);

    // This isn't necessarily the clearest way to determine if the keylist is
    // shared.  Note Collect_Keylist_Managed() isn't called from anywhere
    // else, so it could probably be inlined here and it would be more
    // obvious what's going on.
    //
    if (not parent) {
        INIT_CTX_KEYLIST_UNIQUE(context, keylist);
        LINK_ANCESTOR_NODE(keylist) = NOD(keylist);
    }
    else {
        if (keylist == CTX_KEYLIST(unwrap(parent))) {
            INIT_CTX_KEYLIST_SHARED(context, keylist);

            // We leave the ancestor link as-is in the shared keylist--so
            // whatever the parent had...if we didn't have to make a new
            // keylist.  This means that an object may be derived, even if you
            // look at its keylist and its ancestor link points at itself.
        }
        else {
            INIT_CTX_KEYLIST_UNIQUE(context, keylist);
            LINK_ANCESTOR_NODE(keylist) = NOD(CTX_KEYLIST(unwrap(parent)));
        }
    }

    RELVAL *var = STABLE(ARR_HEAD(varlist));
    INIT_VAL_CONTEXT_ROOTVAR(var, kind, varlist);

    ++var;

    for (; len > 1; --len, ++var) // [0] is rootvar (context), already done
        Init_Nulled(var);

    if (parent) {
        //
        // Copy parent values, and for values we copied that were blocks and
        // strings, replace their series components with deep copies.
        //
        REBVAL *dest = CTX_VARS_HEAD(context);
        REBVAL *src = CTX_VARS_HEAD(unwrap(parent));
        for (; NOT_END(src); ++dest, ++src) {
            REBFLGS flags = NODE_FLAG_MANAGED;  // !!! Review, what flags?
            Move_Value(dest, src);
            Clonify(dest, flags, TS_CLONE);
        }
    }

    if (parent)  // v-- passing in nullptr to indicate no more binds
        Rebind_Context_Deep(unwrap(parent), context, nullptr);

    ASSERT_CONTEXT(context);

#if !defined(NDEBUG)
    PG_Reb_Stats->Objects++;
#endif

    return context;
}


//
//  Construct_Context_Managed: C
//
// Construct an object without evaluation.
// Parent can be null. Values are rebound.
//
// In R3-Alpha the CONSTRUCT native supported a mode where the following:
//
//      [a: b: 1 + 2 d: a e:]
//
// ...would have `a` and `b` will be set to 1, while `+` and `2` will be
// ignored, `d` will be the word `a` (where it knows to be bound to the a
// of the object) and `e` would be left as it was.
//
// Ren-C retakes the name CONSTRUCT to be the arity-2 object creation
// function with evaluation, and makes "raw" construction (via /ONLY on both
// 1-arity HAS and CONSTRUCT) more regimented.  The requirement for a raw
// construct is that the fields alternate SET-WORD! and then value, with
// no evaluation--hence it is possible to use any value type (a GROUP! or
// another SET-WORD!, for instance) as the value.
//
// !!! Because this is a work in progress, set-words would be gathered if
// they were used as values, so they are not currently permitted.
//
REBCTX *Construct_Context_Managed(
    enum Reb_Kind kind,
    unstable RELVAL *head,  // !!! Warning: modified binding
    REBSPC *specifier,
    option(REBCTX*) parent
){
    REBCTX *context = Make_Context_Detect_Managed(
        kind, // type
        head, // values to scan for toplevel set-words
        parent // parent
    );

    if (not head)
        return context;

    Bind_Values_Shallow(head, CTX_ARCHETYPE(context));

    unstable const RELVAL *value = head;
    for (; NOT_END(value); value += 2) {
        if (not IS_SET_WORD(value))
            fail (Error_Invalid_Type(VAL_TYPE(value)));

        if (IS_END(value + 1))
            fail ("Unexpected end in context spec block.");

        if (IS_SET_WORD(value + 1))
            fail (Error_Invalid_Type(VAL_TYPE(value + 1))); // TBD: support

        REBVAL *var = Sink_Word_May_Fail(value, specifier);
        Derelativize(var, value + 1, specifier);
    }

    return context;
}


//
//  Context_To_Array: C
//
// Return a block containing words, values, or set-word: value
// pairs for the given object. Note: words are bound to original
// object.
//
// Modes:
//     1 for word
//     2 for value
//     3 for words and values
//
REBARR *Context_To_Array(const RELVAL *context, REBINT mode)
{
    REBDSP dsp_orig = DSP;

    bool always = false;  // default to not always showing hidden things
    if (IS_FRAME(context))
        always = IS_FRAME_PHASED(context);

    REBVAL *key = VAL_CONTEXT_KEYS_HEAD(context);
    REBVAL *var = VAL_CONTEXT_VARS_HEAD(context);

    assert(!(mode & 4));

    REBLEN n = 1;
    for (; NOT_END(key); n++, key++, var++) {
        if (Is_Param_Sealed(key))
            continue;

        if (not always and Is_Param_Hidden(key, var))
            continue;

        if (mode & 1) {
            Init_Any_Word_Bound(
                DS_PUSH(),
                (mode & 2) ? REB_SET_WORD : REB_WORD,
                VAL_CONTEXT(context),
                n
            );

            if (mode & 2)
                SET_CELL_FLAG(DS_TOP, NEWLINE_BEFORE);
        }

        if (mode & 2) {
            //
            // Context might have voids, which denote the value have not
            // been set.  These contexts cannot be converted to blocks,
            // since user arrays may not contain void.
            //
            if (IS_NULLED(var))
                fail (Error_Null_Object_Block_Raw());

            Move_Value(DS_PUSH(), var);
        }
    }

    return Pop_Stack_Values_Core(
        dsp_orig,
        did (mode & 2) ? ARRAY_FLAG_NEWLINE_AT_TAIL : 0
    );
}


//
//  Merge_Contexts_Managed: C
//
// Create a child context from two parent contexts. Merge common fields.
// Values from the second parent take precedence.
//
// Deep copy and rebind the child.
//
REBCTX *Merge_Contexts_Managed(REBCTX *parent1, REBCTX *parent2)
{
    if (parent2 != NULL) {
        assert(CTX_TYPE(parent1) == CTX_TYPE(parent2));
        fail ("Multiple inheritance of object support removed from Ren-C");
    }

    // Merge parent1 and parent2 words.
    // Keep the binding table.

    struct Reb_Collector collector;
    Collect_Start(
        &collector,
        COLLECT_ANY_WORD | COLLECT_AS_TYPESET
    );

    // Leave the [0] slot blank while collecting (ROOTKEY/ROOTPARAM), but
    // valid (but "unreadable") bits so that the copy will still work.
    //
    Init_Unreadable_Void(ARR_HEAD(BUF_COLLECT));
    SET_ARRAY_LEN_NOTERM(BUF_COLLECT, 1);

    // Setup binding table and BUF_COLLECT with parent1 words.  Don't bother
    // checking for duplicates, buffer is empty.
    //
    Collect_Context_Keys(&collector, parent1, false);

    // Add parent2 words to binding table and BUF_COLLECT, and since we know
    // BUF_COLLECT isn't empty then *do* check for duplicates.
    //
    Collect_Context_Keys(&collector, parent2, true);

    // Collect_Keys_End() terminates, but Collect_Context_Inner_Loop() doesn't.
    //
    TERM_ARRAY_LEN(BUF_COLLECT, ARR_LEN(BUF_COLLECT));

    // Allocate child (now that we know the correct size).  Obey invariant
    // that keylists are always managed.  The BUF_COLLECT contains only
    // typesets, so no need for a specifier in the copy.
    //
    // !!! Review: should child start fresh with no meta information, or get
    // the meta information held by parents?
    //
    REBARR *keylist = Copy_Array_Shallow_Flags(
        BUF_COLLECT,
        SPECIFIED,
        NODE_FLAG_MANAGED
    );
    Init_Unreadable_Void(ARR_HEAD(keylist)); // Currently no rootkey usage

    if (parent1 == NULL)
        LINK_ANCESTOR_NODE(keylist) = NOD(keylist);
    else
        LINK_ANCESTOR_NODE(keylist) = NOD(CTX_KEYLIST(parent1));

    REBARR *varlist = Make_Array_Core(
        ARR_LEN(keylist),
        SERIES_MASK_VARLIST
            | NODE_FLAG_MANAGED // rebind below requires managed context
    );
    MISC_META_NODE(varlist) = nullptr;  // GC sees, it must be initialized

    REBCTX *merged = CTX(varlist);
    INIT_CTX_KEYLIST_UNIQUE(merged, keylist);

    // !!! Currently we assume the child will be of the same type as the
    // parent...so if the parent was an OBJECT! so will the child be, if
    // the parent was an ERROR! so will the child be.  This is a new idea,
    // so review consequences.
    //
    RELVAL *rootvar = STABLE(ARR_HEAD(varlist));
    INIT_VAL_CONTEXT_ROOTVAR(rootvar, CTX_TYPE(parent1), varlist);

    // Copy parent1 values.  (Can't use memcpy() because it would copy things
    // like protected bits...)
    //
    REBVAL *copy_dest = CTX_VARS_HEAD(merged);
    const REBVAL *copy_src = CTX_VARS_HEAD(parent1);
    for (; NOT_END(copy_src); ++copy_src, ++copy_dest)
        Move_Var(copy_dest, copy_src);

    // Update the child tail before making calls to CTX_VAR(), because the
    // debug build does a length check.
    //
    TERM_ARRAY_LEN(varlist, ARR_LEN(keylist));

    // Copy parent2 values:
    REBVAL *key = CTX_KEYS_HEAD(parent2);
    REBVAL *value = CTX_VARS_HEAD(parent2);
    for (; NOT_END(key); key++, value++) {
        // no need to search when the binding table is available
        REBINT n = Get_Binder_Index_Else_0(
            &collector.binder, VAL_KEY_SPELLING(key)
        );
        assert(n != 0);

        // Deep copy the child.
        // Context vars are REBVALs, already fully specified
        //
        REBFLGS flags = NODE_FLAG_MANAGED;  // !!! Review, which flags?
        Clonify(
            Move_Value(CTX_VAR(merged, n), value),
            flags,
            TS_CLONE
        );
    }

    // Rebind the child
    //
    Rebind_Context_Deep(parent1, merged, NULL);
    Rebind_Context_Deep(parent2, merged, &collector.binder);

    // release the bind table
    //
    Collect_End(&collector);

    return merged;
}


//
//  Resolve_Context: C
//
// Only_words can be a block of words or an index in the target
// (for new words).
//
void Resolve_Context(
    REBCTX *target,
    REBCTX *source,
    REBVAL *only_words,
    bool all,
    bool expand
) {
    FAIL_IF_READ_ONLY_SER(SER(CTX_VARLIST(target))); // !!! should heed CONST

    REBLEN i;
    if (IS_INTEGER(only_words)) { // Must be: 0 < i <= tail
        i = VAL_INT32(only_words);
        if (i == 0)
            i = 1;
        if (i > CTX_LEN(target))
            return;
    }
    else
        i = 0;

    struct Reb_Binder binder;
    INIT_BINDER(&binder);

  blockscope {
    REBINT n = 0;

    // If limited resolve, tag the word ids that need to be copied:
    if (i != 0) {
        // Only the new words of the target:
        REBVAL *key = CTX_KEY(target, i);
        for (; NOT_END(key); key++)
            Add_Binder_Index(&binder, VAL_KEY_SPELLING(key), -1);
        n = CTX_LEN(target);
    }
    else if (IS_BLOCK(only_words)) {
        // Limit exports to only these words:
        unstable const RELVAL *word = VAL_ARRAY_AT(only_words);
        for (; NOT_END(word); word++) {
            if (IS_WORD(word) or IS_SET_WORD(word)) {
                Add_Binder_Index(&binder, VAL_WORD_SPELLING(word), -1);
                n++;
            }
            else {
                // !!! There was no error here.  :-/  Should it be one?
            }
        }
    }

    // Expand target as needed:
    if (expand and n > 0) {
        // Determine how many new words to add:
        REBVAL *key = CTX_KEYS_HEAD(target);
        for (; NOT_END(key); key++)
            if (Get_Binder_Index_Else_0(&binder, VAL_KEY_SPELLING(key)) != 0)
                --n;

        // Expand context by the amount required:
        if (n > 0)
            Expand_Context(target, n);
        else
            expand = false;
    }
  }

    // Maps a word to its value index in the source context.
    // Done by marking all source words (in bind table):
    //
  blockscope {
    REBVAL *key = CTX_KEYS_HEAD(source);
    REBINT n = 1;
    for (; NOT_END(key); n++, key++) {
        const REBSTR *symbol = VAL_KEY_SPELLING(key);
        if (IS_NULLED(only_words))
            Add_Binder_Index(&binder, symbol, n);
        else {
            if (Get_Binder_Index_Else_0(&binder, symbol) != 0) {
                Remove_Binder_Index(&binder, symbol);
                Add_Binder_Index(&binder, symbol, n);
            }
        }
    }
  }

    // Foreach word in target, copy the correct value from source:
    //
  blockscope {
    REBVAL *var = i != 0 ? CTX_VAR(target, i) : CTX_VARS_HEAD(target);
    REBVAL *key = i != 0 ? CTX_KEY(target, i) : CTX_KEYS_HEAD(target);
    for (; NOT_END(key); key++, var++) {
        REBINT m = Remove_Binder_Index_Else_0(&binder, VAL_KEY_SPELLING(key));
        if (m != 0) {
            // "the remove succeeded, so it's marked as set now" (old comment)

            if (NOT_CELL_FLAG(var, PROTECTED) and (all or IS_VOID(var))) {
                if (m < 0)
                    Init_Void(var, SYM_UNSET);  // not in source context
                else
                    Move_Var(var, CTX_VAR(source, m));  // preserves flags
            }
        }
    }
  }

    // Add any new words and values:
    if (expand) {
        REBVAL *key = CTX_KEYS_HEAD(source);
        REBINT n = 1;
        for (; NOT_END(key); n++, key++) {
            const REBSTR *canon = VAL_KEY_SPELLING(key);
            if (Remove_Binder_Index_Else_0(&binder, canon) != 0) {
                //
                // Note: no protect check is needed here
                //
                REBVAL *var = Append_Context(target, 0, canon);
                Move_Var(var, CTX_VAR(source, n));  // preserves flags
            }
        }
    }
    else {
        // Reset bind table.
        //
        // !!! Whatever this is doing, it doesn't appear to be able to assure
        // that the keys are there.  Hence doesn't use Remove_Binder_Index()
        // but the fault-tolerant Remove_Binder_Index_Else_0()
        //
        if (i != 0) {
            REBVAL *key = CTX_KEY(target, i);
            for (; NOT_END(key); key++)
                Remove_Binder_Index_Else_0(&binder, VAL_KEY_SPELLING(key));
        }
        else if (IS_BLOCK(only_words)) {
            unstable const RELVAL *word = VAL_ARRAY_AT(only_words);
            for (; NOT_END(word); word++) {
                if (IS_WORD(word) or IS_SET_WORD(word))
                    Remove_Binder_Index_Else_0(&binder, VAL_WORD_SPELLING(word));
            }
        }
        else {
            REBVAL *key = CTX_KEYS_HEAD(source);
            for (; NOT_END(key); key++)
                Remove_Binder_Index_Else_0(&binder, VAL_KEY_SPELLING(key));
        }
    }

    SHUTDOWN_BINDER(&binder);
}


//
//  Find_Symbol_In_Context: C
//
// Search a context looking for the given symbol.  Return the index or 0 if
// not found.
//
// Note that since contexts like FRAME! can have multiple keys with the same
// name, the VAL_FRAME_PHASE() of the context has to be taken into account.
//
// !!! Review adding a case-insensitive search option.
//
REBLEN Find_Symbol_In_Context(
    const RELVAL *context,
    const REBSTR *symbol,
    bool strict
){
    bool always = true;
    if (IS_FRAME(context))
        always = IS_FRAME_PHASED(context);
    else {
        // !!! Defaulting to TRUE means that you can find things like SELF
        // even though they are not displayed.  But you can also find things
        // that are hidden with PROTECT/HIDE.  SELF needs a lot of review.
    }

    REBVAL *key = VAL_CONTEXT_KEYS_HEAD(context);
    REBVAL *var = VAL_CONTEXT_VARS_HEAD(context);

    REBLEN n;
    for (n = 1; NOT_END(key); ++n, ++key, ++var) {
        if (strict) {
            if (symbol != VAL_KEY_SPELLING(key))
                continue;
        }
        else {
            if (not SAME_STR(symbol, VAL_KEY_SPELLING(key)))
                continue;
        }

        if (Is_Param_Sealed(key))
            continue;  // pretend this parameter is not there

        if (not always and Is_Param_Hidden(key, var))
            return 0;

        return n;
    }

    return 0;
}


//
//  Select_Symbol_In_Context: C
//
// Search a context's keylist looking for the given symbol, and return the
// value for the word.  Return NULL if the symbol is not found.
//
REBVAL *Select_Symbol_In_Context(const RELVAL *context, const REBSTR *symbol)
{
    const bool strict = false;
    REBLEN n = Find_Symbol_In_Context(context, symbol, strict);
    if (n == 0)
        return nullptr;

    return CTX_VAR(VAL_CONTEXT(context), n);
}


//
//  Obj_Value: C
//
// Return pointer to the nth VALUE of an object.
// Return NULL if the index is not valid.
//
// !!! All cases of this should be reviewed...mostly for getting an indexed
// field out of a port.  If the port doesn't have the index, should it always
// be an error?
//
REBVAL *Obj_Value(REBVAL *value, REBLEN index)
{
    REBCTX *context = VAL_CONTEXT(value);

    if (index > CTX_LEN(context)) return 0;
    return CTX_VAR(context, index);
}


//
//  Startup_Collector: C
//
void Startup_Collector(void)
{
    // Temporary block used while scanning for words.
    //
    // Note that the logic inside Collect_Keylist managed assumes it's at
    // least long enough to hold the rootkey (SYM_0).
    //
    TG_Buf_Collect = Make_Array_Core(1 + 99, SERIES_FLAGS_NONE);
}


//
//  Shutdown_Collector: C
//
void Shutdown_Collector(void)
{
    Free_Unmanaged_Array(TG_Buf_Collect);
    TG_Buf_Collect = NULL;
}


#ifndef NDEBUG

//
//  Assert_Context_Core: C
//
void Assert_Context_Core(REBCTX *c)
{
    REBARR *varlist = CTX_VARLIST(c);

    if (
        (SER(varlist)->header.bits & SERIES_MASK_VARLIST)
        != SERIES_MASK_VARLIST
    ){
        panic (varlist);
    }

    REBARR *keylist = CTX_KEYLIST(c);
    if (keylist == NULL)
        panic (c);

    REBVAL *rootvar = CTX_ROOTVAR(c);
    if (not ANY_CONTEXT(rootvar))
        panic (rootvar);

    REBLEN keys_len = ARR_LEN(keylist);
    REBLEN vars_len = ARR_LEN(varlist);

    if (keys_len < 1)
        panic (keylist);

    if (keys_len != vars_len)
        panic (c);

    if (VAL_CONTEXT(rootvar) != c)
        panic (rootvar);

    if (GET_SERIES_INFO(c, INACCESSIBLE)) {
        //
        // !!! For the moment, don't check inaccessible stack frames any
        // further.  This includes varless reified frames and those reified
        // frames that are no longer on the stack.
        //
        return;
    }

    // Note that in the future the rootkey for OBJECT!/ERROR!/FRAME!/PORT!
    // may be more used.  Reserved as unreadable for now.
    //
    REBVAL *rootkey = CTX_ROOTKEY(c);
    assert(IS_UNREADABLE_DEBUG(rootkey));

    REBVAL *key = CTX_KEYS_HEAD(c);
    REBVAL *var = CTX_VARS_HEAD(c);

    REBLEN n;
    for (n = 1; n < keys_len; n++, var++, key++) {
        if (IS_END(key)) {
            printf("** Early key end at index: %d\n", cast(int, n));
            panic (c);
        }

        if (not IS_PARAM(key))
            panic (key);

        if (IS_END(var)) {
            printf("** Early var end at index: %d\n", cast(int, n));
            panic (c);
        }
    }

    if (NOT_END(key)) {
        printf("** Missing key end at index: %d\n", cast(int, n));
        panic (key);
    }

    if (NOT_END(var)) {
        printf("** Missing var end at index: %d\n", cast(int, n));
        panic (var);
    }
}

#endif
