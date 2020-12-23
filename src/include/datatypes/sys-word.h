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
#define LINK_KEYSOURCE(s)       LINK(s).custom.node

inline static void INIT_LINK_KEYSOURCE(REBARR *varlist, REBNOD *keysource) {
    if (not Is_Node_Cell(keysource))
        assert(KIND3Q_BYTE_UNCHECKED(ARR_HEAD(ARR(keysource))) == REB_VOID);
    LINK_KEYSOURCE(varlist) = keysource;
}

// For a *read-only* REBSTR, circularly linked list of othEr-CaSed string
// forms.  It should be relatively quick to find the canon form on
// average, since many-cased forms are somewhat rare.
//
// Note: String series using this don't have SERIES_FLAG_LINK_NODE_NEEDS_MARK.
// One synonym need not keep another alive, because the process of freeing
// string nodes unlinks them from the list.  (Hence the canon can change!)
//
#define LINK_SYNONYM_NODE(s)    LINK(s).custom.node
#define LINK_SYNONYM(s)         STR(LINK_SYNONYM_NODE(s))


//=//// SAFE COMPARISONS WITH BUILT-IN SYMBOLS ////////////////////////////=//
//
// A SYM refers to one of the built-in words and can be used in C switch
// statements.  A canon STR is used to identify everything else.
// 
// R3-Alpha's concept was that all words got persistent integer values, which
// prevented garbage collection.  Ren-C only gives built-in words integer
// values--or SYMs--while others must be compared by pointers to their
// name or canon-name pointers.  A non-built-in symbol will return SYM_0 as
// its symbol, allowing it to fall through to defaults in case statements.
//
// Though it works fine for switch statements, it creates a problem if someone
// writes `VAL_WORD_SYM(a) == VAL_WORD_SYM(b)`, because all non-built-ins
// will appear to be equal.  It's a tricky enough bug to catch to warrant an
// extra check in C++ that disallows comparing SYMs with ==

#if defined(NDEBUG) || !defined(CPLUSPLUS_11)
    //
    // Trivial definition for C build or release builds: symbols are just a C
    // enum value and an OPT_REBSYM acts just like a REBSYM.
    //
    typedef enum Reb_Symbol REBSYM;
    typedef enum Reb_Symbol OPT_REBSYM;
#else
    struct REBSYM;

    struct OPT_REBSYM {  // may only be converted to REBSYM, no comparisons
        enum Reb_Symbol n;
        OPT_REBSYM (const REBSYM& sym);
        bool operator==(enum Reb_Symbol other) const
          { return n == other; }
        bool operator!=(enum Reb_Symbol other) const
          { return n != other; }

        bool operator==(OPT_REBSYM &&other) const = delete;
        bool operator!=(OPT_REBSYM &&other) const = delete;

        operator unsigned int() const  // so it works in switch() statements
          { return cast(unsigned int, n); }

        explicit operator enum Reb_Symbol()  // must be an *explicit* cast
          { return n; }
    };

    struct REBSYM {  // acts like a REBOL_Symbol with no OPT_REBSYM compares
        enum Reb_Symbol n;
        REBSYM () {}
        REBSYM (int n) : n (cast(enum Reb_Symbol, n)) {}
        REBSYM (OPT_REBSYM opt_sym) : n (opt_sym.n) {}

        operator unsigned int() const  // so it works in switch() statements
          { return cast(unsigned int, n); }

        explicit operator enum Reb_Symbol() {  // must be an *explicit* cast
            assert(n != SYM_0);
            return n;
        }

        bool operator>=(enum Reb_Symbol other) const {
            assert(other != SYM_0);
            return n >= other;
        }
        bool operator<=(enum Reb_Symbol other) const {
            assert(other != SYM_0);
            return n <= other;
        }
        bool operator>(enum Reb_Symbol other) const {
            assert(other != SYM_0);
            return n > other;
        }
        bool operator<(enum Reb_Symbol other) const {
            assert(other != SYM_0);
            return n < other;
        }
        bool operator==(enum Reb_Symbol other) const
          { return n == other; }
        bool operator!=(enum Reb_Symbol other) const
          { return n != other; }

        bool operator==(REBSYM &other) const = delete;  // may be SYM_0
        void operator!=(REBSYM &other) const = delete;  // ...same
        bool operator==(const OPT_REBSYM &other) const = delete;  // ...same
        void operator!=(const OPT_REBSYM &other) const = delete;  // ...same
    };

    inline OPT_REBSYM::OPT_REBSYM(const REBSYM &sym) : n (sym.n) {}
#endif

inline static bool SAME_SYM_NONZERO(REBSYM a, REBSYM b) {
    assert(a != SYM_0 and b != SYM_0);
    return cast(REBLEN, a) == cast(REBLEN, b);
}

inline static OPT_REBSYM STR_SYMBOL(const REBSTR *s) {
    assert(IS_STR_SYMBOL(s));
    return cast(REBSYM, SECOND_UINT16(SER(s)->header));
}

inline static const REBSTR *Canon(REBSYM sym) {
    assert(cast(REBLEN, sym) != 0);
    assert(cast(REBLEN, sym) < SER_USED(PG_Symbol_Canons));  // null if boot!
    return *SER_AT(const REBSTR*, PG_Symbol_Canons, cast(REBLEN, sym));
}

inline static bool SAME_STR(const REBSTR *s1, const REBSTR *s2) {
    assert(IS_STR_SYMBOL(s1));
    assert(IS_STR_SYMBOL(s2));

    const REBSTR *temp = s1;
    do {
        if (temp == s2)
            return true;
    } while ((temp = LINK_SYNONYM(temp)) != s1);

    return false;  // stopped when circularly linked list loops back to self
}

inline static OPT_REBSYM VAL_WORD_SYM(unstable REBCEL(const*) v) {
    assert(PG_Symbol_Canons);  // all syms are 0 prior to Init_Symbols()
    return STR_SYMBOL(VAL_WORD_SPELLING(v));
}

inline static void INIT_WORD_INDEX(unstable RELVAL *v, REBLEN i) {
    PAYLOAD(Any, v).second.i32 = cast(REBINT, i);
}

inline static REBVAL *Init_Any_Word(
    unstable RELVAL *out,
    enum Reb_Kind kind,
    const REBSTR *spelling
){
    RESET_CELL(out, kind, CELL_FLAG_FIRST_IS_NODE);
    INIT_VAL_WORD_BINDING(out, NOD(spelling));
  #if !defined(NDEBUG)
    INIT_WORD_INDEX(out, -1);  // index not heeded if no binding
  #endif

    PAYLOAD(Any, out).first.node = nullptr;
    /*INIT_SPECIFIER(out, UNBOUND);*/

    return cast(REBVAL*, out);
}

#define Init_Word(out,str)          Init_Any_Word((out), REB_WORD, (str))
#define Init_Get_Word(out,str)      Init_Any_Word((out), REB_GET_WORD, (str))
#define Init_Set_Word(out,str)      Init_Any_Word((out), REB_SET_WORD, (str))
#define Init_Sym_Word(out,str)      Init_Any_Word((out), REB_SYM_WORD, (str))

inline static REBVAL *Init_Any_Word_Bound(
    unstable RELVAL *out,
    enum Reb_Kind type,
    REBCTX *context,  // spelling determined by context and index
    REBLEN index
){
    RESET_CELL(out, type, CELL_FLAG_FIRST_IS_NODE);
    INIT_VAL_WORD_BINDING(out, context);
    INIT_WORD_INDEX(out, index);

    PAYLOAD(Any, out).first.node = nullptr;
    /*INIT_SPECIFIER(out, UNBOUND);*/

    return cast(REBVAL*, out);
}


// Helper calls strsize() so you can more easily use literals at callsite.
// (Better to call Intern_UTF8_Managed() with the size if you know it.)
//
inline static const REBSTR *Intern_Unsized_Managed(const char *utf8)
  { return Intern_UTF8_Managed(cb_cast(utf8), strsize(utf8)); }


#ifdef DEBUG_UNSTABLE_CELLS
    inline static unstable REBVAL *Plainify(unstable REBVAL *out)
      { return Plainify(STABLE(out)); }

    inline static unstable REBVAL *Getify(unstable REBVAL *out)
      { return Getify(STABLE(out)); }

    inline static unstable REBVAL *Setify(unstable REBVAL *out)
      { return Setify(STABLE(out)); }

    inline static unstable REBVAL *Symify(unstable REBVAL *out)
      { return Symify(STABLE(out)); }
#endif
