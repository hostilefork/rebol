//
//  File: %sys-quoted.h
//  Summary: {Definitions for QUOTED! Datatype}
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2018 Revolt Open Source Contributors
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
// In Revolt, any value can be "quote" escaped, any number of times.  As there
// is no limit to how many levels of escaping there can be, the general case
// of the escaping cannot fit in a value cell, so a "pairing" array is used.
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
// HOWEVER... there is an efficiency trick, which uses the KIND_BYTE() div 4
// as the "lit level" of a value.  Then the byte mod 4 becomes the actual
// type.  So only an actual REB_QUOTED at "apparent lit-level 0" has its own
// payload...as a last resort if the level exceeded what the type byte can
// encode.
//
// This saves on storage and GC load for small levels of quotedness, at the
// cost of making VAL_TYPE() do an extra comparison to clip all values above
// 64 to act as REB_QUOTED.  Operations like IS_WORD() are not speed affected,
// as they do not need to worry about the aliasing and can just test the byte
// against the unquoted REB_WORD value they are interested in.
//

inline static REBVAL* VAL_QUOTED_PAYLOAD_CELL(const RELVAL *v) {
    assert(KIND_BYTE(v) == REB_QUOTED);
    assert(PAYLOAD(Any, v).second.u > 3);  // else quote fits entirely in cell
    return VAL(VAL_NODE(v));
}

inline static REBLEN VAL_QUOTED_PAYLOAD_DEPTH(const RELVAL *v) {
    assert(KIND_BYTE(v) == REB_QUOTED);
    assert(PAYLOAD(Any, v).second.u > 3);  // else quote fits entirely in cell
    return PAYLOAD(Any, v).second.u;
}

inline static REBLEN VAL_QUOTED_DEPTH(const RELVAL *v) {
    if (KIND_BYTE(v) >= REB_64)  // shallow enough to use type byte trick...
        return KIND_BYTE(v) / REB_64;  // ...see explanation above
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
    if (KIND_BYTE(v) == REB_QUOTED) {  // reuse payload, bump count
        assert(PAYLOAD(Any, v).second.u > 3);  // or should've used kind byte
        PAYLOAD(Any, v).second.u += depth;
        return v;
    }

    // Note: Not CELL_KIND(), may differ from what MIRROR_BYTE() says
    //
    enum Reb_Kind type = cast(enum Reb_Kind, KIND_BYTE(v) % REB_64);
    if (type >= REB_MAX)  // e.g. REB_P_XXX for params
        assert(depth == 0);

    depth += KIND_BYTE(v) / REB_64;

    if (depth <= 3) { // can encode in a cell with no REB_QUOTED payload
        mutable_KIND_BYTE(v) = type + (REB_64 * depth);
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

        REBVAL *paired = Alloc_Pairing();
        Move_Value_Header(paired, v);
        mutable_KIND_BYTE(paired) = type;  // escaping only in literal
        paired->extra = v->extra;
        paired->payload = v->payload;
 
        Init_Unreadable_Blank(PAIRING_KEY(paired));  // Key not used ATM

        Manage_Pairing(paired);

      #if !defined(NDEBUG)
        SET_CELL_FLAG(paired, PROTECTED); // maybe shared; can't change
      #endif

        RESET_VAL_HEADER(v, REB_QUOTED, CELL_FLAG_FIRST_IS_NODE);
        if (Is_Bindable(paired))
            v->extra = paired->extra; // must sync with cell (if binding)
        else {
            // We say all REB_QUOTED cells are bindable, so their binding gets
            // checked even if the contained cell isn't bindable.  By setting
            // the binding to UNBOUND if the contained cell isn't bindable, it
            // prevents needing to make Is_Bindable() a more complex check,
            // we can just say yes always but have it unbound if not.
            //
            EXTRA(Binding, v).node = UNBOUND;
        }
        PAYLOAD(Any, v).first.node = NOD(paired);
        PAYLOAD(Any, v).second.u = depth;
    }

    return v;
}

#if !defined(CPLUSPLUS_11)
    #define Quotify Quotify_Core
#else
    inline static REBVAL *Quotify(REBVAL *v, REBLEN depth)
        { return KNOWN(Quotify_Core(v, depth)); }

    inline static RELVAL *Quotify(RELVAL *v, REBLEN depth)
        { return Quotify_Core(v, depth); }
#endif


// Only works on small escape levels that fit in a cell (<=3).  So it can
// do '''X -> ''X, ''X -> 'X or 'X -> X.  Use Unquotify() for the more
// generic routine, but this is needed by the evaluator most commonly.
//
inline static RELVAL *Unquotify_In_Situ(RELVAL *v, REBLEN unquotes)
{
    assert(KIND_BYTE(v) >= REB_64); // not an in-situ quoted value otherwise
    assert(cast(REBLEN, KIND_BYTE(v) / REB_64) >= unquotes);
    mutable_KIND_BYTE(v) -= REB_64 * unquotes;
    assert(KIND_BYTE(v) % 64 == MIRROR_BYTE(v));
    return v;
}


// Turns 'X into X, or '''''[1 + 2] into '''(1 + 2), etc.
//
// Works on escape levels that fit in the cell (<= 3) as well as those that
// require a second cell to point at in a REB_QUOTED payload.
//
inline static RELVAL *Unquotify_Core(RELVAL *v, REBLEN unquotes) {
    if (unquotes == 0)
        return v;

    if (KIND_BYTE(v) != REB_QUOTED)
        return Unquotify_In_Situ(v, unquotes);

    REBLEN depth = VAL_QUOTED_PAYLOAD_DEPTH(v);
    assert(depth > 3 and depth >= unquotes);
    depth -= unquotes;

    REBVAL *cell = VAL_QUOTED_PAYLOAD_CELL(v);
    assert(
        KIND_BYTE(cell) != REB_0
        and KIND_BYTE(cell) != REB_QUOTED
        and KIND_BYTE(cell) < REB_MAX
    );

    if (depth > 3) // still can't do in-situ escaping within a single cell
        PAYLOAD(Any, v).second.u = depth;
    else {
        Move_Value_Header(v, cell);
        mutable_KIND_BYTE(v) += (REB_64 * depth);
        assert(
            not Is_Bindable(cell) or
            EXTRA(Binding, v).node == EXTRA(Binding, cell).node // must sync
        );
        EXTRA(Binding, v) = EXTRA(Binding, cell);  // in case it's unbindable
        v->payload = cell->payload;
    }
    return v;
}

#if !defined(CPLUSPLUS_11)
    #define Unquotify Unquotify_Core
#else
    inline static REBVAL *Unquotify(REBVAL *v, REBLEN depth)
        { return KNOWN(Unquotify_Core(v, depth)); }

    inline static RELVAL *Unquotify(RELVAL *v, REBLEN depth)
        { return Unquotify_Core(v, depth); }
#endif


inline static const REBCEL *VAL_UNESCAPED(const RELVAL *v) {
    if (KIND_BYTE_UNCHECKED(v) != REB_QUOTED)  // allow unreadable blanks
        return v;  // Note: kind byte may be > 64

    // The reason this routine returns `const` is because you can't modify
    // the contained value without affecting other views of it, if it is
    // shared in an escaping.  Modifications must be done with awareness of
    // the original RELVAL, and that it might be a QUOTED!.
    //
    return VAL_QUOTED_PAYLOAD_CELL(v);
}


inline static REBLEN Dequotify(RELVAL *v) {
    if (KIND_BYTE(v) != REB_QUOTED) {
        REBLEN depth = KIND_BYTE(v) / REB_64;
        mutable_KIND_BYTE(v) %= REB_64;
        return depth;
    }

    REBLEN depth = VAL_QUOTED_PAYLOAD_DEPTH(v);
    RELVAL *cell = VAL_QUOTED_PAYLOAD_CELL(v);
    assert(KIND_BYTE(cell) != REB_QUOTED and KIND_BYTE(cell) < REB_64);

    Move_Value_Header(v, cell);
  #if !defined(NDEBUG)
    if (Is_Bindable(cell))
        assert(EXTRA(Binding, v).node == EXTRA(Binding, cell).node);
    else
        assert(not EXTRA(Binding, v).node);
  #endif
    v->extra = cell->extra;
    v->payload = cell->payload;
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
