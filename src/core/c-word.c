//
//  File: %c-word.c
//  Summary: "symbol table and word related functions"
//  Section: core
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
// In R3-Alpha, words were not garbage collected, and their UTF-8 data was
// kept in a separate table from the REBSERs.  In Ren-C, words use REBSERs,
// and are merely *indexed* by hashes of their canon forms via an external
// table.  This table grows and shrinks as canons are added and removed.
//

#include "sys-core.h"

#define WORD_TABLE_SIZE 1024  // initial size in words


//
// Prime numbers used for hash table sizes. Divide by 2 for
// number of words that can be held in the symbol table.
//
static REBLEN const Primes[] =
{
    7,
    13,
    31,
    61,
    127,
    251,
    509,
    1021,
    2039,
    4093,
    8191,
    16381,
    32749,
    65521,
    131071,
    262139,
    524287,
    1048573,
    2097143,
    4194301,
    8388593,
    16777213,
    33554393,
    67108859,
    134217689,
    268435399,
    536870909,
    1073741789,
    2147483647,
    0xFFFFFFFB, // 4294967291 = 2^32 - 5 (C89)
    0
// see https://primes.utm.edu/lists/2small/0bit.html
};


//
//  Try_Get_Hash_Prime: C
//
// Given a value, return a prime number that is larger or equal.
//
REBINT Try_Get_Hash_Prime(REBLEN minimum)
{
    REBINT n = 0;
    while (minimum > Primes[n]) {
        ++n;
        if (Primes[n] == 0)
            return 0;
    }

    return Primes[n];
}


//
//  Get_Hash_Prime_May_Fail: C
//
REBINT Get_Hash_Prime_May_Fail(REBLEN minimum)
{
    REBINT prime = Try_Get_Hash_Prime(minimum);
    if (prime == 0) {  // larger than hash prime table
        DECLARE_LOCAL (temp);
        Init_Integer(temp, minimum);
        fail (Error_Size_Limit_Raw(temp));
    }
    return prime;
}


// Removals from linear probing lists can be complex, because the same
// overflow slot may be visited through different initial hashes:
//
// http://stackoverflow.com/a/279812/211160
//
// Since it's not enough to simply NULL out the spot when an interned string
// is GC'd, a special pointer signaling "deletedness" is used.  It does not
// cause a linear probe to terminate, but it is reused on insertions.
//
static REBSTR PG_Deleted_Symbol;
#define DELETED_CANON &PG_Deleted_Symbol


//
//  Expand_Word_Table: C
//
// Expand the hash table part of the word_table by allocating
// the next larger table size and rehashing all the words of
// the current table.  Free the old hash array.
//
static void Expand_Word_Table(void)
{
    // The only full list of symbol words available is the old hash table.
    // Hold onto it while creating the new hash table.

    REBLEN old_num_slots = SER_USED(PG_Canons_By_Hash);
    REBSTR* *old_canons_by_hash = SER_HEAD(REBSTR*, PG_Canons_By_Hash);

    REBLEN num_slots = Get_Hash_Prime_May_Fail(old_num_slots + 1);
    assert(SER_WIDE(PG_Canons_By_Hash) == sizeof(REBSTR*));

    REBSER *ser = Make_Series_Core(
        num_slots, sizeof(REBSYM*), SERIES_FLAG_POWER_OF_2
    );
    Clear_Series(ser);
    SET_SERIES_LEN(ser, num_slots);

    // Rehash all the canon symbols:

    REBSTR **new_canons_by_hash = SER_HEAD(REBSTR*, ser);

    REBLEN old_slot;
    for (old_slot = 0; old_slot != old_num_slots; ++old_slot) {
        REBSTR *canon = old_canons_by_hash[old_slot];
        if (not canon)
            continue;

        if (canon == DELETED_CANON) {  // clean out deleted canon entries
            --PG_Num_Canon_Slots_In_Use;
          #if !defined(NDEBUG)
            --PG_Num_Canon_Deleteds;  // keep track for shutdown assert
          #endif
            continue;
        }

        REBLEN skip;
        REBLEN slot = First_Hash_Candidate_Slot(
            &skip,
            Hash_String(canon),
            num_slots
        );

        while (new_canons_by_hash[slot]) {  // skip occupied slots
            slot += skip;
            if (slot >= num_slots)
                slot -= num_slots;
        }
        new_canons_by_hash[slot] = canon;
    }

    Free_Unmanaged_Series(PG_Canons_By_Hash);
    PG_Canons_By_Hash = ser;
}


inline static REBSYM *Make_Canon(
    bool *was_lowercase,  // returns true if the input data itself was canon
    const REBYTE *utf8,
    REBSIZ size
){
    REBSYM *canon = SYM(Make_Binary_Core(
        size,  // always + 1, space included for terminator
        NODE_FLAG_MANAGED | SERIES_FLAG_IS_STRING
            | STRING_FLAG_IS_SYMBOL | SERIES_FLAG_FIXED_SIZE
    ));
    SET_SERIES_INFO(canon, SYMBOL_IS_CANON);

    // The incoming string isn't always null terminated, e.g. if you are
    // interning `foo` in `foo: bar + 1` it would be colon-terminated.  But
    // it was ostensibly required to be all valid UTF-8...validated by the
    // scanner.
    //
    // !!! This didn't seem to be actually true in R3-Alpha, so interning may
    // need to do double-duty with validation.  Review.
    //
    REBCHR(const*) tail = cast(const REBYTE*, utf8) + size;
    REBCHR(const*) cp = utf8;
    REBCHR(*) dest = BIN_HEAD(canon);

    *was_lowercase = true;  // start by assuming the input data is all lower

    REBUNI c;
    while (cp != tail) {
        cp = NEXT_CHR(&c, cp);

        REBUNI lower = LO_CASE(c);
        if (lower != c)
            *was_lowercase = false;

        dest = WRITE_CHR(dest, lower);
    }

    // In the current design, the canon does not have space for a length
    // field...the string is assumed to be short, and LINK()/MISC() have other
    // uses in symbols.
    //
    TERM_BIN_LEN(canon, size);

    // The UTF-8 series can be aliased with AS to become an ANY-STRING! or a
    // BINARY!.  If it is, then it should not be modified.
    //
    Freeze_Series(canon); 

    mutable_LINK(NextSynonym, canon) = SYM(canon);  // 1-item in circular list

    // leave header.bits as 0 for SYM_0 as answer to VAL_WORD_ID()
    // Startup_Symbols() tags values from %words.r after the fact.
    //
    // Words that aren't in the bootup %words.r list don't have integer
    // IDs defined that can be used in compiled C switch() cases (e.g.
    // SYM_ANY, SYM_INTEGER_X, etc.)  So if we didn't find a pre-existing
    // synonym, and none is added, it will remain at 0.
    //
    // !!! It is proposed that a pre-published dictionary of small words
    // could be agreed on, and then extensions using those words could
    // request the numbers for those words.  Inconsistent requests that
    // didn't follow the published list could cause an error.  This would
    // give more integer values without more strings in the core.
    //
    assert(SECOND_UINT16(canon->leader) == 0);

    // Canons use their MISC() to hold binding information.  Long term,
    // better answers are going to be needed that let multiple binds run
    // at once...and there may be a parallel table to PG_Canons_By_Hash
    // (or just make the canons table have more slots per canon) to hold
    // richer information.
    //
    // But for the moment only one bind runs at a time, so to paint a
    // picture of a future where there'd be more complexity that there's a
    // poor-man's demo wit high bits or low bits to suggest sharing (start
    // with 2, grow to N eventually).
    //
    canon->misc.bind_index.high = 0;
    canon->misc.bind_index.low = 0;

    return canon;
}


//
//  Intern_UTF8_Managed: C
//
// Makes only one copy of each distinct character string:
//
// https://en.wikipedia.org/wiki/String_interning
//
// Interned UTF8 strings are stored as series, and are implicitly managed
// by the GC (because they are shared).
//
// Interning is case-sensitive, but a "synonym" linkage is established between
// instances that are just differently upper-or-lower-"cased".  They agree on
// one "canon" interning to use for fast case-insensitive compares.  If that
// canon form is GC'd, the agreed upon canon for the group will change.
//
// Created series must be managed, because if they were not there could be no
// clear contract on the return result--as it wouldn't be possible to know if
// a shared instance had been managed by someone else or not.
//
const REBSYM *Intern_UTF8_Managed(const REBYTE *utf8, size_t size)
{
    // The hashing technique used is called "linear probing":
    //
    // https://en.wikipedia.org/wiki/Linear_probing
    //
    // For the hash search to be guaranteed to terminate, the table must be
    // large enough that we are able to find a NULL if there's a miss.  (It's
    // actually kept larger than that, but to be on the right side of theory,
    // the table is always checked for expansion needs *before* the search.)
    //
    REBLEN num_slots = SER_USED(PG_Canons_By_Hash);
    if (PG_Num_Canon_Slots_In_Use > num_slots / 2) {
        Expand_Word_Table();
        num_slots = SER_USED(PG_Canons_By_Hash);  // got larger
    }

    REBSYM* *canons_by_hash = SER_HEAD(REBSYM*, PG_Canons_By_Hash);

    REBLEN skip; // how many slots to skip when occupied candidates found
    REBLEN slot = First_Hash_Candidate_Slot(
        &skip,
        Hash_UTF8(utf8, size),
        num_slots
    );

    // The hash table only indexes the canon form of each spelling.  So when
    // testing a slot to see if it's a match (or a collision that needs to
    // be skipped to try again) the search uses a comparison that is
    // case-insensitive...but reports if synonyms via > 0 results.
    //
    REBSYM *prev_synonym = nullptr;
    uint16_t intern_order;
    REBSYM **deleted_slot = nullptr;
    REBSYM* canon;
    while ((canon = canons_by_hash[slot])) {
        if (canon == DELETED_CANON) {
            deleted_slot = &canons_by_hash[slot];
            goto next_candidate_slot;
        }

        assert(GET_SERIES_INFO(canon, SYMBOL_IS_CANON));

      blockscope {
        REBINT cmp = Compare_UTF8(STR_HEAD(canon), utf8, size);
        if (cmp == 0)
            return canon;  // case-sensitive match, utf8 must be all lowercase
        if (cmp < 0)
            goto next_candidate_slot;  // wasn't an alternate casing

        // The > 0 result means that the canon found is a lowercase version of
        // the string we are looking for (hence `utf8` must *not* be all
        // lowercase).  Search the stored circularly linked list to see if
        // this is a spelling variation that we've seen before.
        //
        if (LINK(NextSynonym, canon) == canon) {
            intern_order = 1;
            prev_synonym = canon;
            goto new_synonym_symbol;
        }

        REBSYM *last = canon;
        intern_order = 0;
        do {
            REBSYM *synonym = LINK(NextSynonym, last);
            if (0 == Compare_UTF8(STR_HEAD(synonym), utf8, size))
                return synonym;  // already interned, return the symbol

            if (not prev_synonym) {
                uint16_t synonym_order = SECOND_UINT16(synonym->leader);
                ++intern_order;
                assert(intern_order <= synonym_order);
                if (intern_order < synonym_order)  // reclaim the lower number
                    prev_synonym = synonym;
            }

            last = synonym;
        } while (LINK(NextSynonym, last) != canon);

        // If we didn't find a lower index in the ordering to reclaim for
        // this synonym insertion, then put it at the tail of the synonyms
        // list (e.g. the canon in circular linkage will be its NextSynonym)
        //
        if (prev_synonym == nullptr) {
            prev_synonym = last;
            ++intern_order;
        }

        goto new_synonym_symbol;
      }

      next_candidate_slot:  // https://en.wikipedia.org/wiki/Linear_probing

        slot += skip;
        if (slot >= num_slots)
            slot -= num_slots;
    }

    assert(canon == nullptr);

    // If we get here, that means no canon for the symbol was found--
    // hence no alternate spellings can exist either.  A new canon is
    // thus needed.  The canon will be all-lowercase version of the data.

  blockscope {
    bool was_lowercase;
    canon = Make_Canon(&was_lowercase, utf8, size);

    if (deleted_slot) {
        *deleted_slot = canon;  // reuse the deleted slot
      #if !defined(NDEBUG)
        --PG_Num_Canon_Deleteds;  // note slot usage count stays constant
      #endif
    }
    else {
        canons_by_hash[slot] = canon;
        ++PG_Num_Canon_Slots_In_Use;
    }

    // If what we were being asked to intern was all lowercase, then
    // there's no need to intern another spelling.  The canon is a match.

    if (was_lowercase)
        return canon;

    // But if the canon was not all lowercase, we have to do a synonym
    // insertion.  Insert the new symbol before the canon itself.
    //
    prev_synonym = canon;
    intern_order = 1;
  }

  new_synonym_symbol: {  // `synonym` should hold current last synonym in list

    REBSYM *intern = SYM(Make_Series_Core(
        size + 1,  // if small, fits in a REBSER node (w/no data allocation)
        sizeof(REBYTE),
        NODE_FLAG_MANAGED | SERIES_FLAG_IS_STRING
            | STRING_FLAG_IS_SYMBOL | SERIES_FLAG_FIXED_SIZE
            | SERIES_FLAG_MISC_NODE_NEEDS_MARK  // see MISC(CanonOfSynonym)
    ));

    // The incoming string isn't always null terminated, e.g. if you are
    // interning `foo` in `foo: bar + 1` it would be colon-terminated.
    //
    memcpy(BIN_HEAD(intern), utf8, size);
    TERM_BIN_LEN(intern, size);

    // The UTF-8 series can be aliased with AS to become an ANY-STRING! or a
    // BINARY!.  If it is, then it should not be modified.
    //
    Freeze_Series(intern); 

    // This is a synonym for an existing canon.  Link it into the synonyms
    // circularly linked list, and direct link the canon form.
    //
    mutable_LINK(NextSynonym, intern) = LINK(NextSynonym, prev_synonym);
    mutable_LINK(NextSynonym, prev_synonym) = SYM(intern);

    // We want to make sure canons aren't GC'd so long as a synonym for them
    // exists, so keeping a pointer to the canon in the synonym plus the
    // MISC_NODE_NEEDS_MARK achieves that purpose.
    //
    mutable_MISC(CanonOfSynonym, intern) = canon;

    // The general case of a WORD! cell does not have enough bits to store
    // binding, virtual binding, and a unique spelling variation for the
    // word.  We'd have to increase the cell size...which would have broad
    // impacts on the system.
    //
    // What's done instead is that non-canon symbols store the index of the
    // synonym in the linked list, and sneak that information into the header.
    // 32-bit systems don't have enough space to store a full index...so
    // the trick only works for the first 3 synonyms, and if a word uses a
    // variation besides that the cell must expand (similar to QUOTED!)
    //
    // !!! It would be nice if this index could be folded in to somewhere
    // that it was 0 in the canon form so a separate bit flag test for the
    // canon status wasn't needed.
    //
    assert(NOT_SERIES_INFO(intern, SYMBOL_IS_CANON));
    assert(SECOND_UINT16(intern->leader) == 0);
    assert(intern_order != 0);
    SET_SECOND_UINT16(intern->leader, intern_order);

    return intern;
  }
}


//
//  GC_Kill_Interning: C
//
// Unlink this spelling out of the circularly linked list of synonyms.
// Further, if it happens to be canon, we need to re-point everything in the
// chain to a new entry.  Choose the synonym as a new canon if so.
//
void GC_Kill_Interning(REBSYM *intern)
{
    REBSYM *canon = nullptr;
    if (GET_SERIES_INFO(intern, SYMBOL_IS_CANON))
        canon = intern;

    REBSYM *synonym = LINK(NextSynonym, intern);  // may be the same as intern
    assert(synonym != nullptr);
    while (LINK(NextSynonym, synonym) != intern) {
        if (GET_SERIES_INFO(synonym, SYMBOL_IS_CANON))
            canon = synonym;
        synonym = LINK(NextSynonym, synonym);
    }

    // A GC pass can free the canon before it frees synonyms... so you can
    // be at a point here where `canon == nullptr`.

    // We remove the interning from the linked list of synonyms.  If the
    // interning is not canon, that's all this routine needs to do.
    //
    mutable_LINK(NextSynonym, synonym) = LINK(NextSynonym, intern);
    if (intern != canon)
        return;

    // If we're GC'ing a canon, we need to remove it from the hash table

    // It is currently illegal to GC during binds.
    //
    assert(canon->misc.bind_index.high == 0);
    assert(canon->misc.bind_index.low == 0);

    REBLEN num_slots = SER_USED(PG_Canons_By_Hash);
    REBSTR* *canons_by_hash = SER_HEAD(REBSTR*, PG_Canons_By_Hash);

    REBLEN skip;
    REBLEN slot = First_Hash_Candidate_Slot(
        &skip,
        Hash_String(canon),
        num_slots
    );

    // We *will* find the canon form in the hash table.
    //
    while (canons_by_hash[slot] != canon) {
        slot += skip;
        if (slot >= num_slots)
            slot -= num_slots;
    }

    // This canon form must be removed from the hash table.  Ripple the
    // collision slots back until a NULL is found, to reduce search times.
    //
    REBLEN previous_slot = slot;
    while (canons_by_hash[slot]) {
        slot += skip;
        if (slot >= num_slots)
            slot -= num_slots;
        canons_by_hash[previous_slot] = canons_by_hash[slot];
    }

    // Signal that the hash slot is "deleted" via a special pointer.
    // See notes on DELETED_SLOT for why the final slot in the collision
    // chain can't just be left NULL:
    //
    // http://stackoverflow.com/a/279812/211160
    //
    canons_by_hash[previous_slot] = DELETED_CANON;

  #if !defined(NDEBUG)
    ++PG_Num_Canon_Deleteds;  // total use same (PG_Num_Canon_Or_Deleteds)
  #endif
}


//
//  Startup_Interning: C
//
// Get the engine ready to do Intern_UTF8_Managed(), which is required to
// get REBSTR* pointers generated during a scan of ANY-WORD!s.  Words of the
// same spelling currently look up and share the same REBSTR*, this process
// is referred to as "string interning":
//
// https://en.wikipedia.org/wiki/String_interning
//
void Startup_Interning(void)
{
    PG_Num_Canon_Slots_In_Use = 0;
  #if !defined(NDEBUG)
    PG_Num_Canon_Deleteds = 0;
  #endif

    // Start hash table out at a fixed size.  When collisions occur, it
    // causes a skipping pattern that continues until it finds the desired
    // slot.  The method is known as linear probing:
    //
    // https://en.wikipedia.org/wiki/Linear_probing
    //
    // It must always be at least as big as the total number of words, in order
    // for it to uniquely be able to locate each symbol pointer.  But to
    // reduce long probing chains, it should be significantly larger than that.
    // R3-Alpha used a heuristic of 4 times as big as the number of words.

    REBLEN n;
  #if defined(NDEBUG)
    n = Get_Hash_Prime_May_Fail(WORD_TABLE_SIZE * 4);  // *4 reduces rehashing
  #else
    n = 1; // forces exercise of rehashing logic in debug build
  #endif

    PG_Canons_By_Hash = Make_Series_Core(
        n, sizeof(REBSTR*), SERIES_FLAG_POWER_OF_2
    );
    Clear_Series(PG_Canons_By_Hash);  // all slots start as nullptr
    SET_SERIES_LEN(PG_Canons_By_Hash, n);
}


//
//  Startup_Early_Symbols: C
//
// It's very desirable to have `/`, `/foo`, `/foo/`, `/foo/(bar)` etc. be
// instances of the same datatype of PATH!.  In this scheme, `/` would act
// like a "root path" and be achieved with `to path! [_ _]`.
//
// But with limited ASCII symbols, there is strong demand for `/` to be able
// to act like division in evaluative contexts, or to be overrideable for
// other things in a way not too dissimilar from `+`.
//
// The compromise used is to make `/` be a cell whose VAL_TYPE() is REB_PATH,
// but whose CELL_KIND() is REB_WORD with the special spelling `-1-SLASH-`.
// Binding mechanics and evaluator behavior are based on this unusual name.
// But when inspected by the user, it appears to be a PATH! with 2 blanks.
//
// This duality is imperfect, as any routine with semantics like COLLECT-WORDS
// would have to specifically account for it, or just see an empty path.
// But it is intended to give some ability to configure the behavior easily.
//
// The trick which allows the `/` to be a 2-element PATH! and yet act like a
// WORD! when used in evaluative contexts requires that word's spelling to be
// available during scanning.  But scanning is what loads the %words.r symbol
// list!  Break the Catch-22 by manually interning the symbol used.
//
// (Same issue applies to the symbol in ~unreadable~ in release builds, used
// e.g. by the data stack initialization.  In debug builds NULL is used to
// detect the errors on reads.)
//
void Startup_Early_Symbols(void)
{
    const char *slash1 = "-slash-1-";
    assert(PG_Slash_1_Canon == nullptr);
    PG_Slash_1_Canon = Intern_UTF8_Managed(cb_cast(slash1), strsize(slash1));

    const char *dot1 = "-dot-1-";
    assert(PG_Dot_1_Canon == nullptr);
    PG_Dot_1_Canon = Intern_UTF8_Managed(cb_cast(dot1), strsize(dot1));

    const char *unreadable = "unreadable";
    assert(PG_Unreadable_Canon == nullptr);
    PG_Unreadable_Canon = Intern_UTF8_Managed(
        cb_cast(unreadable),
        strsize(unreadable)
    );
}


//
//  Startup_Symbols: C
//
// By this point in the boot, the canon words have already been interned for
// everything in %words.r.
//
// This goes through the name series for %words.r words and tags them with
// SYM_XXX constants.  This allows the small number to be quickly extracted to
// use with VAL_WORD_ID() in C switch statements.  These are the only words
// that have fixed symbol numbers--others are only managed and compared
// through their pointers.
//
// It also creates a table for mapping from SYM_XXX => REBSTR series.  This
// is used e.g. by Canon(SYM_XXX) to get the string name for a symbol.
//
void Startup_Symbols(REBARR *words)
{
    assert(PG_Symbol_Canons == nullptr);
    PG_Symbol_Canons = Make_Series_Core(
        1 + ARR_LEN(words), // 1 + => extra trash at head for SYM_0
        sizeof(REBSTR*),
        SERIES_FLAG_FIXED_SIZE // can't ever add more SYM_XXX lookups
    );

    // All words that not in %words.r will get back VAL_WORD_ID(w) == SYM_0
    // Hence, SYM_0 cannot be canonized.  Allowing Canon(SYM_0) to return NULL
    // and try and use that meaningfully is too risky, so it is simply
    // prohibited to canonize SYM_0, and trash the REBSTR* in the [0] slot.
    //
    SYMID id = SYM_0;
    TRASH_POINTER_IF_DEBUG(
        *SER_AT(REBCAN*, PG_Symbol_Canons, cast(REBLEN, id))
    );

    RELVAL *word = ARR_HEAD(words);
    for (; NOT_END(word); ++word) {
        assert(IS_WORD(word));  // real word, not fake (e.g. `/` as -slash-0-)
        REBCAN *canon = m_cast(REBCAN*, VAL_WORD_CANON(word));

        id = cast(SYMID, cast(REBLEN, id) + 1);
        *SER_AT(REBCAN*, PG_Symbol_Canons, cast(REBLEN, id)) = canon;

        if (id == SYM__SLASH_1_)
            assert(canon == PG_Slash_1_Canon);  // make sure it lined up!
        else if (id == SYM__DOT_1_)
            assert(canon == PG_Dot_1_Canon);
        else if (id == SYM_UNREADABLE)
            assert(canon == PG_Unreadable_Canon);

        // Canon symbols store SYMID in the header's second uint16_t bits.
        // Could probably use less than 16 bits, but 8 is insufficient.
        // (length %words.r > 256)
        //
        assert(SECOND_UINT16(canon->leader) == 0);
        SET_SECOND_UINT16(canon->leader, id);
        assert(Same_Nonzero_Symid(ID_OF_CANON(canon), id));
    }

    // !!! Terminate the series...but is it necessary to do so?
    //
    *SER_AT(REBCAN*, PG_Symbol_Canons, cast(REBLEN, id)) = nullptr;

    SET_SERIES_USED(PG_Symbol_Canons, 1 + cast(REBLEN, id));
    assert(SER_USED(PG_Symbol_Canons) == 1 + ARR_LEN(words));

    // Do some sanity checks.  !!! Fairly critical, is debug-only appropriate?

    if (0 != strcmp("blank!", STR_UTF8(Canon(SYM_BLANK_X))))
        panic (Canon(SYM_BLANK_X));

    if (0 != strcmp("true", STR_UTF8(Canon(SYM_TRUE))))
        panic (Canon(SYM_TRUE));

    if (0 != strcmp("open", STR_UTF8(Canon(SYM_OPEN))))
        panic (Canon(SYM_OPEN));

    PG_Bar_Canon = Canon(SYM_BAR);  // used by PARSE for speedup
}


//
//  Shutdown_Symbols: C
//
void Shutdown_Symbols(void)
{
    Free_Unmanaged_Series(PG_Symbol_Canons);
    PG_Symbol_Canons = nullptr;

    PG_Slash_1_Canon = nullptr;
    PG_Dot_1_Canon = nullptr;
    PG_Unreadable_Canon = nullptr;
}


//
//  Shutdown_Interning: C
//
void Shutdown_Interning(void)
{
  #if !defined(NDEBUG)
    if (PG_Num_Canon_Slots_In_Use - PG_Num_Canon_Deleteds != 0) {
        //
        // !!! There needs to be a more user-friendly output for this,
        // and to detect if it really was an API problem or something else
        // that needs to be paid attention to in the core.  Right now the
        // two scenarios are conflated into this one panic.
        //
        printf(
            "!!! %d leaked canons found in shutdown\n",
            cast(int, PG_Num_Canon_Slots_In_Use - PG_Num_Canon_Deleteds)
        );
        printf("!!! LIKELY rebUnmanage() without a rebRelease() in API\n");

        fflush(stdout);

        REBLEN slot;
        for (slot = 0; slot < SER_USED(PG_Canons_By_Hash); ++slot) {
            REBSYM *canon = *SER_AT(REBSYM*, PG_Canons_By_Hash, slot);
            if (canon and canon != DELETED_CANON)
                panic (canon);
        }
    }
  #endif

    Free_Unmanaged_Series(PG_Canons_By_Hash);
}
