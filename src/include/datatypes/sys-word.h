//
//  File: %sys-word.h
//  Summary: {Definitions for the ANY-WORD! Datatypes}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Ren-C Open Source Contributors
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
// The ANY-WORD! is the fundamental symbolic concept of Rebol.  It is
// implemented as a REBSTR UTF-8 string (see %sys-string.h), but rather than
// hold "bookmark" caches of indexing positions into its data (which is
// generally quite short and not iterated), it stores links to "synonyms"
// of alternate spellings which share the same symbol ID.
//
// ANY-WORD! can act as a variable when bound specifically to a context
// (see %sys-context.h) or bound relatively to an action (see %sys-action.h).
//
// For routines that manage binding, see %sys-bind.h.
//



// REBCTX types use this field of their varlist (which is the identity of
// an ANY-CONTEXT!) to find their "keylist".  It is stored in the REBSER
// node of the varlist REBARR vs. in the REBVAL of the ANY-CONTEXT! so
// that the keylist can be changed without needing to update all the
// REBVALs for that object.
//
// It may be a simple REBARR* -or- in the case of the varlist of a running
// FRAME! on the stack, it points to a REBFRM*.  If it's a FRAME! that
// is not running on the stack, it will be the function paramlist of the
// actual phase that function is for.  Since REBFRM* all start with a
// REBVAL cell, this means NODE_FLAG_CELL can be used on the node to
// discern the case where it can be cast to a REBFRM* vs. REBARR*.
//
// (Note: FRAME!s used to use a field `misc.f` to track the associated
// frame...but that prevented the ability to SET-META on a frame.  While
// that feature may not be essential, it seems awkward to not allow it
// since it's allowed for other ANY-CONTEXT!s.  Also, it turns out that
// heap-based FRAME! values--such as those that come from MAKE FRAME!--
// have to get their keylist via the specifically applicable ->phase field
// anyway, and it's a faster test to check this for NODE_FLAG_CELL than to
// separately extract the CTX_TYPE() and treat frames differently.)
//
// It is done as a base-class REBNOD* as opposed to a union in order to
// not run afoul of C's rules, by which you cannot assign one member of
// a union and then read from another.
//
#define LINK_KeySource_TYPE         REBNOD*
#define LINK_KeySource_CAST         NOD

inline static void INIT_LINK_KEYSOURCE(REBARR *varlist, REBNOD *keysource) {
    if (not Is_Node_Cell(keysource))
        assert(GET_SERIES_FLAG(SER(keysource), IS_KEYLIKE));
    mutable_LINK(KeySource, varlist) = keysource;
}

inline static REBLEN VAL_WORD_PRIMARY_INDEX(RELVAL *v) {
    assert(ANY_WORD_KIND(CELL_KIND(VAL_UNESCAPED(v))));
    return VAL_WORD_PRIMARY_INDEX_UNCHECKED(v);
}

inline static void INIT_VAL_WORD_PRIMARY_INDEX(RELVAL *v, REBLEN i) {
    assert(ANY_WORD_KIND(CELL_HEART(VAL_UNESCAPED(v))));
    assert(i < 1048576);  // 20 bit number for physical indices
    VAL_WORD_INDEXES_U32(v) &= 0xFFF00000;
    VAL_WORD_INDEXES_U32(v) |= i;
}

inline static void INIT_VAL_WORD_VIRTUAL_MONDEX(
    REBCEL(const*) v,  // mutation allowed on cached property
    REBLEN mondex  // index mod 4095 (hence invented name "mondex")
){
    assert(ANY_WORD_KIND(HEART3X_BYTE(v)));
    assert(mondex <= MONDEX_MOD);  // 12 bit number for virtual indices
    VAL_WORD_INDEXES_U32(m_cast(RELVAL*, cast(const RELVAL*, v))) &= 0x000FFFFF;
    VAL_WORD_INDEXES_U32(m_cast(RELVAL*, cast(const RELVAL*, v))) |= mondex << 20;
}

#ifdef CPLUSPLUS_11
    inline static void INIT_VAL_WORD_VIRTUAL_MONDEX(
        const RELVAL *v,  // virtual binding only in unescaped forms
        REBLEN mondex
    ) = delete;
#endif

inline static REBVAL *Init_Any_Word_Untracked(
    RELVAL *out,
    enum Reb_Kind kind,
    const REBSYM *symbol
){
    RESET_VAL_HEADER(out, kind, CELL_FLAG_FIRST_IS_NODE);
    mutable_BINDING(out) = symbol;
    VAL_WORD_INDEXES_U32(out) = 0;
    INIT_VAL_WORD_CACHE(cast(REBCEL(const*), out), SPECIFIED);

    // For each word there is a lowercase spelling, and up to two alternate
    // case variations that are encoded in the heart byte (TBD).  But if you
    // use a third spelling, that gives the cell a heart byte that counts in
    // the "escaped" range...while still encoding the cell is an ANY-WORD!.
    //
    // !!! The two alternate spellings concept is pending, but close.
    //
    // !!! Technically, the escaping is not necessary unless the word is
    // bound.  However, bouncing things from expanded to unexpanded makes
    // the code more complicated...and requires dereferencing the symbol
    // often.  This allows the heart byte to be set once and cue the necessary
    // behaviors from then on.
    //
    if (NOT_SERIES_INFO(symbol, SYMBOL_IS_CANON)) {
        Quotify_Core(out, 4);  // !!! trigger expansion, reuse code for now
        mutable_KIND3Q_BYTE(out) = kind;
        mutable_HEART3X_BYTE(out) = kind + (REB_64 * 3);  // signal escaping
        assert(VAL_WORD_VIRTUAL_MONDEX_UNCHECKED(out) == 4);  // quote level
        VAL_WORD_INDEXES_U32(out) &= 0x000FFFFF;  // zero out quote level
        assert(VAL_WORD_VIRTUAL_MONDEX_UNCHECKED(out) == 0);  // non quoted
    }

    return cast(REBVAL*, out);
}

#define Init_Any_Word(out,kind,spelling) \
    Init_Any_Word_Untracked(TRACK_CELL_IF_DEBUG(out), (kind), (spelling))

#define Init_Word(out,str)          Init_Any_Word((out), REB_WORD, (str))
#define Init_Get_Word(out,str)      Init_Any_Word((out), REB_GET_WORD, (str))
#define Init_Set_Word(out,str)      Init_Any_Word((out), REB_SET_WORD, (str))
#define Init_Sym_Word(out,str)      Init_Any_Word((out), REB_SYM_WORD, (str))


// Helper calls strsize() so you can more easily use literals at callsite.
// (Better to call Intern_UTF8_Managed() with the size if you know it.)
//
inline static const REBSTR *Intern_Unsized_Managed(const char *utf8)
  { return Intern_UTF8_Managed(cb_cast(utf8), strsize(utf8)); }
