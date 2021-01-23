//
//  File: %sys-quoted.h
//  Summary: {Definitions for QUOTED! Datatype}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2018-2021 Ren-C Open Source Contributors
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
// In Ren-C, any value can be "quote" escaped, any number of times.  The
// general case for adding information that it is escaped--as well as the
// amount it is escaped by--can't fit in a cell.  So a "pairing" array is used
// (a compact form with only a series tracking node, sizeof(REBVAL)*2).  This
// is the smallest size of a GC'able entity--the same size as a singular
// array, but a pairing is used so the GC picks up from a cell pointer that
// it is a pairing and be placed as a REBVAL* in the cell.
//
// The depth is the number of apostrophes, e.g. ''''X is a depth of 4.  It is
// stored in the cell payload and not pairing node, so that when you add or
// remove quote levels to the same value a new node isn't required...the cell
// just has a different count.
//
// HOWEVER... there is an efficiency trick, which uses the KIND3Q_BYTE() div 4
// as the "quote level" of a value.  Then the byte mod 4 becomes the actual
// type.  So only an actual REB_QUOTED at "apparent quote-level 0" has its own
// payload...as a last resort if the level exceeded what the type byte can
// encode.
//
// This saves on storage and GC load for small levels of quotedness, at the
// cost of making VAL_TYPE() do an extra comparison to clip all values above
// 64 to act as REB_QUOTED.  Operations like IS_WORD() are not speed affected,
// as they do not need to worry about the aliasing and can just test the byte
// against the unquoted REB_WORD value they are interested in.
//
// Binding is handled specially to mix the binding information into the
// QUOTED! cell instead of the cells that are being escaped.  This is because
// when there is a high level of quoting and the escaped cell is shared at
// a number of different places, those places may have different bindings.
// To pull this off, the escaped cell stores only *cached* binding information
// for virtual binding...leaving all primary binding in the quoted cell.
//
// Consequently, the quoting level is slipped into the virtual binding index
// location of the word.
//


//=//// WORD DEFINITION CODE //////////////////////////////////////////////=//
//
// !!! The code should get reorganized to not have these definitions in the
// quoting header.  But for the moment this untangles the dependencies so
// that it will compile.
//

inline static void Unbind_Any_Word(RELVAL *v);  // forward define

// The values that are "escaped", e.g. which have VAL_NODE1() pointing to a
// unbound REBCEL(const*), start at 192.  This includes not just REB_QUOTED
// but also things like (REB_WORD + REB_64 * 3), which signal that more than
// 3 spelling variations were needed...so it has to use the unbound form.
//
#define REB_192_ESCAPED 192

#define VAL_WORD_PRIMARY_INDEX_UNCHECKED(v) \
    (VAL_WORD_INDEXES_U32(v) & 0x000FFFFF)

#define VAL_WORD_VIRTUAL_MONDEX_UNCHECKED(v) \
    ((VAL_WORD_INDEXES_U32(v) & 0xFFF00000) >> 20)

inline static bool Is_Escaped_Value(const RELVAL *v)
{
    if (HEART3X_BYTE(v) < REB_192_ESCAPED)
        return false;

    if (HEART3X_BYTE(v) == REB_192_ESCAPED) {
        assert(VAL_WORD_VIRTUAL_MONDEX_UNCHECKED(v) > 3);  // depth

        // If a quoted has its heart byte as the base case escape, that means
        // it should *not* be a funny-spelled word (if it's a word), because
        // those words have their own escaping going on.
        //
        REBCEL(const*) unescaped = VAL(VAL_NODE1(v));
        if (ANY_WORD_KIND(CELL_KIND(unescaped))) {
            const REBSYM *symbol = SYM(BINDING(unescaped));
            assert(GET_SERIES_INFO(symbol, SYMBOL_IS_CANON));
            UNUSED(symbol);
        }
        UNUSED(unescaped);
    }
    else {  // a funny spelled word, that may or may not be a REB_QUOTED

        if (IS_QUOTED(v))
            assert(VAL_WORD_VIRTUAL_MONDEX_UNCHECKED(v) > 0);  // depth
        else {
            assert(ANY_WORD(v) or ANY_SEQUENCE(v));  // sequence w/word heart
            REBCEL(const*) unescaped = VAL(VAL_NODE1(v));
            const REBSYM *symbol = SYM(BINDING(unescaped));
            assert(NOT_SERIES_INFO(symbol, SYMBOL_IS_CANON));
            UNUSED(unescaped);
            UNUSED(symbol);
        }
    }

    return true;
}

inline static REBLEN VAL_QUOTED_PAYLOAD_DEPTH(const RELVAL *v) {
    assert(Is_Escaped_Value(v));
    return VAL_WORD_VIRTUAL_MONDEX_UNCHECKED(v);
}

inline static REBVAL* VAL_QUOTED_PAYLOAD_CELL(const RELVAL *v) {
    assert(Is_Escaped_Value(v));
    return VAL(VAL_NODE1(v));
}

inline static REBLEN VAL_QUOTED_DEPTH(const RELVAL *v) {
    if (KIND3Q_BYTE(v) >= REB_64)  // shallow enough to use type byte trick...
        return KIND3Q_BYTE(v) / REB_64;  // ...see explanation above
    return VAL_QUOTED_PAYLOAD_DEPTH(v);
}

inline static REBLEN VAL_NUM_QUOTES(const RELVAL *v) {
    if (not IS_QUOTED(v))
        return 0;
    return VAL_QUOTED_DEPTH(v);
}


// It is necessary to be able to store relative values in escaped cells.
//
inline static RELVAL *Quotify_Core(
    RELVAL *v,
    REBLEN depth
){
    if (depth == 0)
        return v;  // make below code easier by assuming result always quotes

    if (Is_Escaped_Value(v)) {  // reuse payload
        assert(VAL_QUOTED_PAYLOAD_DEPTH(v) + depth <= MONDEX_MOD);  // limited
        VAL_WORD_INDEXES_U32(v) += (depth << MONDEX_SHIFT);

        // The pre-existing kind was either a word with a weird spelling
        // (hence it needed to escape) or something quoted > 3 times, or both.
        // Unconditionally force it to be a REB_QUOTED.
        //
        mutable_KIND3Q_BYTE(v) = REB_QUOTED;

        return v;
    }

    REBYTE kind = KIND3Q_BYTE_UNCHECKED(v) % REB_64;  // HEART_BYTE may differ
    assert(kind <= REB_MAX);

    depth += KIND3Q_BYTE_UNCHECKED(v) / REB_64;

    if (depth <= 3) { // can encode in a cell with no REB_QUOTED payload
        mutable_KIND3Q_BYTE(v) = kind + (REB_64 * depth);
    }
    else {
        // An efficiency trick here could point to VOID_VALUE, BLANK_VALUE,
        // NULLED_CELL, etc. in those cases, so long as GC knew.  (But how
        // efficient do 4-level-deep-quoted nulls need to be, really?)

        // This is an uncomfortable situation of moving values without a
        // specifier; but it needs to be done otherwise you could not have
        // literals in function bodies.  What it means is that you should
        // not be paying attention to the cell bits for making decisions
        // about specifiers and such.  The format bits of this cell are
        // essentially noise, and only the literal's specifier should be used.

        REBVAL *unquoted = Alloc_Pairing();
        Init_Unreadable_Void(PAIRING_KEY(unquoted));  // Key not used ATM

        Move_Value_Header(unquoted, v);
        mutable_KIND3Q_BYTE(unquoted) = kind;  // escaping only in literal

        unquoted->payload = v->payload;
 
        Manage_Pairing(unquoted);

        // We don't use RESET_VAL_HEADER() here because while the kind byte
        // shows REB_QUOTED (the lower end of the quoted range of kinds),
        // the heart byte needs to be higher to cluster with the high variant
        // spellings in the hearts needing escaping.
        //
        v->header.bits &= CELL_MASK_PERSIST;
        v->header.bits |= FLAG_KIND3Q_BYTE(REB_QUOTED)
                            | FLAG_HEART3X_BYTE(REB_192_ESCAPED)
                            | CELL_FLAG_FIRST_IS_NODE;

        INIT_VAL_NODE1(v, unquoted);  // v-- See VAL_QUOTED_DEPTH()
        VAL_WORD_INDEXES_U32(v) = depth << MONDEX_SHIFT;

        if (ANY_WORD_KIND(CELL_HEART(cast(REBCEL(const*), unquoted)))) {
            //
            // Words that are bound find their spellings by means of their
            // PRIMARY_INDEX.  If a word is shared by several QUOTED!
            // then that index can be different in each one.  So the shared
            // word is put in an unbound state, which means the binding is
            // to the spelling, which VAL_WORD_SYMBOL() of REBCEL can work.
            //
            VAL_WORD_INDEXES_U32(v) |=
                VAL_WORD_PRIMARY_INDEX_UNCHECKED(unquoted);
            unquoted->extra = v->extra;  // !!! for easier Unbind, review
            Unbind_Any_Word(unquoted);  // so that binding is a spelling
            // leave `v` binding as it was
        }
        else if (Is_Bindable(unquoted)) {
            mutable_BINDING(unquoted) = UNBOUND;  // must look unbound
            // leave `v` to hold the binding as it was
        }
        else {
            // We say all REB_QUOTED cells are bindable, so their binding gets
            // checked even if the contained cell isn't bindable.  By setting
            // the binding to UNBOUND if the contained cell isn't bindable, it
            // prevents needing to make Is_Bindable() a more complex check,
            // we can just say yes always but have it unbound if not.
            //
            unquoted->extra = v->extra;  // save the non-binding-related data
            mutable_BINDING(v) = UNBOUND;
        }

      #if !defined(NDEBUG)
        SET_CELL_FLAG(unquoted, PROTECTED); // maybe shared; can't change
      #endif
    }

    return v;
}

#if !defined(CPLUSPLUS_11)
    #define Quotify Quotify_Core
#else
    inline static REBVAL *Quotify(REBVAL *v, REBLEN depth)
        { return cast(REBVAL*, Quotify_Core(v, depth)); }

    inline static RELVAL *Quotify(RELVAL *v, REBLEN depth)
        { return Quotify_Core(v, depth); }
#endif


// Only works on small escape levels that fit in a cell (<=3).  So it can
// do '''X -> ''X, ''X -> 'X or 'X -> X.  Use Unquotify() for the more
// generic routine, but this is needed by the evaluator most commonly.
//
// Note: Strangely pretentious name is on purpose, to discourage general use.
//
inline static RELVAL *Unquotify_In_Situ(RELVAL *v, REBLEN unquotes)
{
    assert(KIND3Q_BYTE(v) >= REB_64);  // not an in-situ quoted value otherwise
    assert(cast(REBLEN, KIND3Q_BYTE(v) / REB_64) >= unquotes);
    mutable_KIND3Q_BYTE(v) -= REB_64 * unquotes;
    return v;
}


inline static void Collapse_Quoted_Internal(RELVAL *v)
{
    assert(HEART3X_BYTE(v) == REB_192_ESCAPED);

    REBCEL(const*) unquoted = VAL_QUOTED_PAYLOAD_CELL(v);
    assert(
        KIND3Q_BYTE_UNCHECKED(unquoted) != REB_0
        and KIND3Q_BYTE_UNCHECKED(unquoted) != REB_QUOTED
        and KIND3Q_BYTE_UNCHECKED(unquoted) < REB_MAX
    );

    Move_Value_Header(v, unquoted);
    if (ANY_WORD_KIND(CELL_HEART(unquoted))) {
        //
        // `v` needs to retain the primary binding index (which was
        // kept in its QUOTED! form), but sync with the virtual binding
        // information in the escaped form.
        //
        VAL_WORD_INDEXES_U32(v) &= 0x000FFFFF;  // wipe out quote depth
        VAL_WORD_INDEXES_U32(v) |=
            (VAL_WORD_INDEXES_U32(unquoted) & 0xFFF00000);
        INIT_VAL_WORD_CACHE(cast(REBCEL(const*), v), VAL_WORD_CACHE(unquoted));
    }
    else {
        v->payload = unquoted->payload;
        if (not Is_Bindable(v))  // non-bindable types need the extra data
            v->extra = unquoted->extra;
    }
}


// Turns 'X into X, or '''''[1 + 2] into '''[1 + 2], etc.
//
// Works on escape levels that fit in the cell (<= 3) as well as those that
// require a second cell to point at in a REB_QUOTED payload.
//
inline static RELVAL *Unquotify_Core(RELVAL *v, REBLEN unquotes) {
    if (unquotes == 0)
        return v;

    if (KIND3Q_BYTE(v) != REB_QUOTED)
        return Unquotify_In_Situ(v, unquotes);

    assert(Is_Escaped_Value(v));

    REBLEN depth = VAL_QUOTED_PAYLOAD_DEPTH(v);
    assert(depth >= unquotes);
    depth -= unquotes;

    if (
        depth > 3  // still can't do in-situ escaping within a single cell
        or HEART3X_BYTE(v) > REB_192_ESCAPED  // weird words stay escaped
    ){
        if (depth == 0)
            mutable_KIND3Q_BYTE(v)
                = KIND3Q_BYTE_UNCHECKED(VAL_QUOTED_PAYLOAD_CELL(v));

        VAL_WORD_INDEXES_U32(v) -= (unquotes << MONDEX_SHIFT);
    }
    else {
        Collapse_Quoted_Internal(v);  // sets depth to 0
        mutable_KIND3Q_BYTE(v) += (REB_64 * depth);
    }
    return v;
}

#if !defined(CPLUSPLUS_11)
    #define Unquotify Unquotify_Core
#else
    inline static REBVAL *Unquotify(REBVAL *v, REBLEN depth)
        { return cast(REBVAL*, Unquotify_Core(v, depth)); }

    inline static RELVAL *Unquotify(RELVAL *v, REBLEN depth)
        { return Unquotify_Core(v, depth); }
#endif


inline static REBCEL(const*) VAL_UNESCAPED(const RELVAL *v) {
    if (HEART3X_BYTE(v) < REB_192_ESCAPED)
        return v;  // Note: kind byte may be > 64

    // The reason this routine returns `const` is because you can't modify
    // the contained value without affecting other views of it, if it is
    // shared in an escaping.  Modifications must be done with awareness of
    // the original RELVAL, and that it might be a QUOTED!.
    //
    return VAL_QUOTED_PAYLOAD_CELL(v);
}


inline static REBLEN Dequotify(RELVAL *v) {
    if (KIND3Q_BYTE(v) != REB_QUOTED) {
        REBLEN depth = KIND3Q_BYTE(v) / REB_64;
        mutable_KIND3Q_BYTE(v) %= REB_64;
        return depth;
    }

    REBLEN depth = VAL_QUOTED_PAYLOAD_DEPTH(v);
    if (HEART3X_BYTE(v) == REB_192_ESCAPED)
        Collapse_Quoted_Internal(v);
    else {
        // Will still be escaped if weird word, zero out the quote depth
        //
        mutable_KIND3Q_BYTE(v)
            = KIND3Q_BYTE_UNCHECKED(VAL_QUOTED_PAYLOAD_CELL(v));
        VAL_WORD_INDEXES_U32(v) &= 0x000FFFFF;
    }
    return depth;
}


// !!! Temporary workaround for what was IS_LIT_WORD() (now not its own type)
//
inline static bool IS_QUOTED_WORD(const RELVAL *v) {
    return IS_QUOTED(v)
        and VAL_QUOTED_DEPTH(v) == 1
        and CELL_KIND(VAL_UNESCAPED(v)) == REB_WORD;
}

// !!! Temporary workaround for what was IS_LIT_PATH() (now not its own type)
//
inline static bool IS_QUOTED_PATH(const RELVAL *v) {
    return IS_QUOTED(v)
        and VAL_QUOTED_DEPTH(v) == 1
        and CELL_KIND(VAL_UNESCAPED(v)) == REB_PATH;
}

// Binding aside, the cell bits in the unescaped cell should match the bits
// of the escaped cell.  In the case of a REB_QUOTED, the escaped cell is the
// only place to find the KIND and HEART information.  But with a "weird word"
// that information has to be in sync...if the outer says it's a REB_PATH
// kind, then the VAL_UNESCAPED() cell must also convey that.
//
// Hence anything that does tweaking of the bits must consier the shared case,
// and unshare the cell before tweaking.  This gives rare mutable acces to
// the content.
//
inline static RELVAL *Unshare_Unescaped(RELVAL *v) {
    if (not Is_Escaped_Value(v))
        return nullptr;  // one less call if you're not sure

    REBVAL *paired = Alloc_Pairing();
    Move_Value(paired, VAL(VAL_NODE1(v)));
    Init_Unreadable_Void(PAIRING_KEY(paired));
    Manage_Pairing(paired);
    INIT_VAL_NODE1(v, paired);
    assert(KIND3Q_BYTE_UNCHECKED(paired) < REB_QUOTED);
    assert(HEART3X_BYTE(paired) < REB_QUOTED);
    return paired;
}


// This combines the unsharing logic along with doing the tweak of the bytes
// in both the shared and unshared cell.
//
inline static void Update_Kind_Heart(RELVAL *v, REBYTE kind, REBYTE heart)
{
    assert(kind < REB_QUOTED);
    assert(heart < REB_QUOTED);

    RELVAL *unescaped = Unshare_Unescaped(v);

    if (kind != 0) {
        if (KIND3Q_BYTE_UNCHECKED(v) != REB_QUOTED) {
            mutable_KIND3Q_BYTE(v) &= 0xC0;
            mutable_KIND3Q_BYTE(v) |= kind;
        }
        if (unescaped)
            mutable_KIND3Q_BYTE(unescaped) = kind;
    }

    if (heart != 0) {
        if (HEART3X_BYTE(v) != REB_192_ESCAPED) {
            mutable_HEART3X_BYTE(v) &= 0xC0;
            mutable_HEART3X_BYTE(v) |= heart;
        }
        if (unescaped)
            mutable_HEART3X_BYTE(unescaped) = heart;
    }
}
