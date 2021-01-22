//
//  File: %sys-quoted.h
//  Summary: {Definitions for QUOTED! Datatype}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2018 Ren-C Open Source Contributors
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

#define VAL_WORD_PRIMARY_INDEX_UNCHECKED(v) \
    (VAL_WORD_INDEXES_U32(v) & 0x000FFFFF)

#define VAL_WORD_VIRTUAL_MONDEX_UNCHECKED(v) \
    ((VAL_WORD_INDEXES_U32(v) & 0xFFF00000) >> 20)



inline static REBLEN VAL_QUOTED_PAYLOAD_DEPTH(const RELVAL *v) {
    assert(IS_QUOTED(v));
    REBLEN depth = VAL_WORD_VIRTUAL_MONDEX_UNCHECKED(v);
    assert(depth > 3);  // else quote fits entirely in cell
    return depth;
}

inline static REBVAL* VAL_QUOTED_PAYLOAD_CELL(const RELVAL *v) {
    assert(VAL_QUOTED_PAYLOAD_DEPTH(v) > 3);  // else quote fits in one cell
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
    if (KIND3Q_BYTE_UNCHECKED(v) == REB_QUOTED) {  // reuse payload
        assert(VAL_QUOTED_PAYLOAD_DEPTH(v) + depth <= MONDEX_MOD);  // limited
        VAL_WORD_INDEXES_U32(v) += (depth << 20);
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

        RESET_VAL_HEADER(v, REB_QUOTED, CELL_FLAG_FIRST_IS_NODE);
        INIT_VAL_NODE1(v, unquoted);
        VAL_WORD_INDEXES_U32(v) = depth << 20;  // see VAL_QUOTED_DEPTH()

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


// Turns 'X into X, or '''''[1 + 2] into '''(1 + 2), etc.
//
// Works on escape levels that fit in the cell (<= 3) as well as those that
// require a second cell to point at in a REB_QUOTED payload.
//
inline static RELVAL *Unquotify_Core(RELVAL *v, REBLEN unquotes) {
    if (unquotes == 0)
        return v;

    if (KIND3Q_BYTE(v) != REB_QUOTED)
        return Unquotify_In_Situ(v, unquotes);

    REBLEN depth = VAL_QUOTED_PAYLOAD_DEPTH(v);
    assert(depth > 3 and depth >= unquotes);
    depth -= unquotes;

    if (depth > 3) // still can't do in-situ escaping within a single cell
        VAL_WORD_INDEXES_U32(v) -= (unquotes << 20);
    else {
        Collapse_Quoted_Internal(v);
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
    if (HEART3X_BYTE(v) != REB_QUOTED)
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
    Collapse_Quoted_Internal(v);
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
