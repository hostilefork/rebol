//
//  File: %sys-roots.h
//  Summary: {Definitions for allocating REBVAL* API handles}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
//=////////////////////////////////////////////////////////////////////////=//
//
// API REBVALs live in singular arrays (which fit inside a REBSER node, that
// is the size of 2 REBVALs).  But they aren't kept alive by references from
// other values, like the way that a REBARR used by a BLOCK! is kept alive.
// They are kept alive by being roots (currently implemented with a flag
// NODE_FLAG_ROOT, but it could also mean living in a distinct pool from
// other series nodes).
//
// The API value content is in the single cell, with LINK().owner holding
// a REBCTX* of the FRAME! that controls its lifetime, or EMPTY_ARRAY.  This
// link field exists in the pointer immediately prior to the REBVAL*, which
// means it can be sniffed as a REBNOD* and distinguished from handles that
// were given back with rebMalloc(), so routines can discern them.
//
// MISC() is currently unused, but could serve as a reference count or other
// purpose.  It's not particularly necessary to have API handles use REBSER
// nodes--though the 2*sizeof(REBVAL) provides some optimality, and it
// means that REBSER nodes can be recycled for more purposes.  But it would
// potentially be better to have them in their own pools, because being
// roots could be discovered without a "pre-pass" in the GC.
//


//=//// ARRAY_FLAG_SINGULAR_API_RELEASE //////////////////////////////////=//
//
// The rebR() function can be used with an API handle to tell a variadic
// function to release that handle after encountering it.
//
// !!! API handles are singular arrays, because there is already a stake in
// making them efficient.  However it means they have to share header and
// info bits, when most are not applicable to them.  This is a tradeoff, and
// contention for bits may become an issue in the future.
//
#define ARRAY_FLAG_SINGULAR_API_RELEASE \
    ARRAY_FLAG_23


//=//// ARRAY_FLAG_INSTRUCTION_ADJUST_QUOTING /////////////////////////////=//
//
// This is used by rebQ() and rebU() to either add a quoting level of splices
// or to remove one.  Today these arrays are always singular and contain
// one value, but in the future they might contain more.
//
#define ARRAY_FLAG_INSTRUCTION_ADJUST_QUOTING \
    ARRAY_FLAG_24


// When Push_Action() happens, it sets f->original, but it's guaranteed to be
// null if an action is not running.  This is tested via a macro because the
// debug build doesn't do any inlining, and it's called often.
//
#define Is_Action_Frame(f) \
    ((f)->original != nullptr)


// While a function frame is fulfilling its arguments, the `f->param` will
// be pointing to a typeset.  The invariant that is maintained is that
// `f->param` will *not* be a typeset when the function is actually in the
// process of running.  (So no need to set/clear/test another "mode".)
//
// Some cases in debug code call this all the way up the call stack, and when
// the debug build doesn't inline functions it's best to use as a macro.

#define Is_Action_Frame_Fulfilling_Unchecked(f) \
    NOT_END((f)->param)

inline static bool Is_Action_Frame_Fulfilling(REBFRM *f) {
    assert(Is_Action_Frame(f));
    return Is_Action_Frame_Fulfilling_Unchecked(f);
}



//
//  Context_For_Frame_May_Manage: C
//
inline static REBCTX *Context_For_Frame_May_Manage(REBFRM *f) {
    assert(not Is_Action_Frame_Fulfilling(f));
    SET_SERIES_FLAG(f->varlist, MANAGED);
    return CTX(f->varlist);
}


// What distinguishes an API value is that it has both the NODE_FLAG_CELL and
// NODE_FLAG_ROOT bits set.
//
inline static bool Is_Api_Value(const RELVAL *v) {
    assert(v->header.bits & NODE_FLAG_CELL);
    return did (v->header.bits & NODE_FLAG_ROOT);
}

// !!! The return cell from this allocation is a trash cell which has had some
// additional bits set.  This means it is not "canonized" trash that can be
// detected as distinct from UTF-8 strings, so don't call IS_TRASH_DEBUG() or
// Detect_Rebol_Pointer() on it until it has been further initialized.
//
// Ren-C manages by default.
//
inline static REBVAL *Alloc_Value(void)
{
    REBARR *a = Alloc_Singular(NODE_FLAG_ROOT | NODE_FLAG_MANAGED);

    // Giving the cell itself NODE_FLAG_ROOT lets a REBVAL* be discerned as
    // either an API handle or not.  The flag is not copied by Move_Value().
    //
    REBVAL *v = KNOWN(ARR_SINGLE(a));
    v->header.bits |= NODE_FLAG_ROOT; // it's trash (can't use SET_CELL_FLAGS)

    REBFRM *f = FS_TOP;
    while (not Is_Action_Frame(f)) // e.g. a path fulfillment
        f = f->prior; // FS_BOTTOM is a dummy action, should always stop

    LINK(a).owner = NOD(Context_For_Frame_May_Manage(f));
    return v;
}

inline static void Free_Value(REBVAL *v)
{
    assert(Is_Api_Value(v));

    REBARR *a = Singular_From_Cell(v);
    TRASH_CELL_IF_DEBUG(ARR_SINGLE(a));
    GC_Kill_Series(SER(a));
}


// "Instructions" are singular arrays; they are intended to be used directly
// with a variadic API call, and will be freed automatically by an enumeration
// to the va_end() point--whether there is an error, throw, or completion.
//
// They are not GC managed, in order to avoid taxing the garbage collector
// (and tripping assert mechanisms).  So they can leak if used incorrectly.
//
// Instructions should be returned as a const void *, in order to discourage
// using these anywhere besides as arguments to a variadic API like rebValue().
//
inline static REBARR *Alloc_Instruction(enum Reb_Api_Opcode opcode) {
    REBSER *s = Alloc_Series_Node(
        SERIES_FLAG_FIXED_SIZE // not tracked as stray manual, but unmanaged
        /* ... */
    );
    s->info = Endlike_Header(
        FLAG_WIDE_BYTE_OR_0(0) // signals array, also implicit terminator
            | FLAG_LEN_BYTE_OR_255(1) // signals singular
    );
    MISC(s).opcode = opcode;
    SER_CELL(s)->header.bits =
        CELL_MASK_NON_STACK_END | NODE_FLAG_ROOT;
    TRACK_CELL_IF_DEBUG(SER_CELL(s), "<<instruction>>", 0);
    return ARR(s);
}

inline static void Free_Instruction(REBARR *a) {
    assert(IS_SER_ARRAY(SER(a)));
    TRASH_CELL_IF_DEBUG(ARR_SINGLE(a));
    Free_Node(SER_POOL, NOD(a));
}


// If you're going to just fail() anyway, then loose API handles are safe to
// GC.  It's mildly inefficient to do so compared to generating a local cell:
//
//      DECLARE_LOCAL (specific);
//      Derelativize(specific, relval, specifier);
//      fail (Error_Something(specific));
//
// But assuming errors don't happen that often, it's cleaner to have one call.
//
inline static REBVAL *rebSpecific(const RELVAL *v, REBSPC *specifier)
    { return Derelativize(Alloc_Value(), v, specifier);}


// The evaluator accepts API handles back from action dispatchers, and the
// path evaluator accepts them from path dispatch.  This code does common
// checking used by both, which includes automatic release of the handle
// so the dispatcher can write things like `return rebValue(...);` and not
// encounter a leak.
//
// !!! There is no protocol in place yet for the external API to throw,
// so that is something to think about.  At the moment, only f->out can
// hold thrown returns, and these API handles are elsewhere.
//
inline static void Handle_Api_Dispatcher_Result(REBFRM *f, const REBVAL* r) {
    //
    // NOTE: Evaluations are performed directly into API handles as the output
    // slot of the evaluation.  Clearly you don't want to release the cell
    // you're evaluating into, so checks against the frame's output cell
    // should be done before calling this routine!
    //
    assert(r != f->out);

  #if !defined(NDEBUG)
    if (NOT_CELL_FLAG(r, ROOT)) {
        printf("dispatcher returned non-API value not in D_OUT\n");
        printf("during ACTION!: %s\n", f->label_utf8);
        printf("`return D_OUT;` or use `RETURN (non_api_cell);`\n");
        panic(r);
    }
  #endif

    if (IS_NULLED(r))
        assert(!"Dispatcher returned nulled cell, not C nullptr for API use");

    Move_Value(f->out, r);
    if (NOT_CELL_FLAG(r, MANAGED))
        rebRelease(r);
}
