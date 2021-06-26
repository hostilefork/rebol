//
//  File: %sys-bind.h
//  Summary: "System Binding Include"
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
// R3-Alpha had a per-thread "bind table"; a large and sparsely populated hash
// into which index numbers would be placed, for what index those words would
// have as keys or parameters.  Ren-C's strategy is that binding information
// is wedged into REBSER nodes that represent the canon words themselves.
//
// This would create problems if multiple threads were trying to bind at the
// same time.  While threading was never realized in R3-Alpha, Ren-C doesn't
// want to have any "less of a plan".  So the Reb_Binder is used by binding
// clients as a placeholder for whatever actual state would be used to augment
// the information in the canon word series about which client is making a
// request.  This could be coupled with some kind of lockfree adjustment
// strategy whereby a word that was contentious would cause a structure to
// "pop out" and be pointed to by some atomic thing inside the word.
//
// For the moment, a binder has some influence by saying whether the high 16
// bits or low 16 bits of the canon's misc.index are used.  If the index
// were atomic this would--for instance--allow two clients to bind at once.
// It's just a demonstration of where more general logic using atomics
// that could work for N clients would be.
//
// The debug build also adds another feature, that makes sure the clear count
// matches the set count.
//
// The binding will be either a REBACT (relative to a function) or a
// REBCTX (specific to a context), or simply a plain REBARR such as
// EMPTY_ARRAY which indicates UNBOUND.  The FLAVOR_BYTE() says what it is
//
//     ANY-WORD!: binding is the word's binding
//
//     ANY-ARRAY!: binding is the relativization or specifier for the REBVALs
//     which can be found inside of the frame (for recursive resolution
//     of ANY-WORD!s)
//
//     ACTION!: binding is the instance data for archetypal invocation, so
//     although all the RETURN instances have the same paramlist, it is
//     the binding which is unique to the REBVAL specifying which to exit
//
//     ANY-CONTEXT!: if a FRAME!, the binding carries the instance data from
//     the function it is for.  So if the frame was produced for an instance
//     of RETURN, the keylist only indicates the archetype RETURN.  Putting
//     the binding back together can indicate the instance.
//
//     VARARGS!: the binding identifies the feed from which the values are
//     coming.  It can be an ordinary singular array which was created with
//     MAKE VARARGS! and has its index updated for all shared instances.
//
// Due to the performance-critical nature of these routines, they are inline
// so that locations using them may avoid overhead in invocation.




// Tells whether when an ACTION! has a binding to a context, if that binding
// should override the stored binding inside of a WORD! being looked up.
//
//    o1: make object! [a: 10 f: does [print a]]
//    o2: make o1 [a: 20 b: 22]
//    o3: make o2 [b: 30]
//
// In the scenario above, when calling `f` bound to o2 stored in o2, or the
// call to `f` bound to o3 and stored in o3, the `a` in the relevant objects
// must be found from the override.  This is done by checking to see if a
// walk from the derived keylist makes it down to the keylist for a.
//
// Note that if a new keylist is not made, it's not possible to determine a
// "parent/child" relationship.  There is no information stored which could
// tell that o3 was made from o2 vs. vice-versa.  The only thing that happens
// is at MAKE-time, o3 put its binding into any functions bound to o2 or o1,
// thus getting its overriding behavior.
//
inline static bool Is_Overriding_Context(REBCTX *stored, REBCTX *override)
{
    REBNOD *stored_source = LINK(KeySource, CTX_VARLIST(stored));
    REBNOD *temp = LINK(KeySource, CTX_VARLIST(override));

    // FRAME! "keylists" are actually paramlists, and the LINK.underlying
    // field is used in paramlists (precluding a LINK.ancestor).  Plus, since
    // frames are tied to a function they invoke, they cannot be expanded.
    // For now, deriving from FRAME! is just disabled.
    //
    // Use a faster check for REB_FRAME than CTX_TYPE() == REB_FRAME, since
    // we were extracting keysources anyway.
    //
    // !!! Note that in virtual binding, something like a FOR-EACH would
    // wind up overriding words bound to FRAME!s, even though not "derived".
    //
    if (Is_Node_Cell(stored_source))
        return false;
    if (Is_Node_Cell(temp))
        return false;

    while (true) {
        if (temp == stored_source)
            return true;

        if (LINK(Ancestor, SER(temp)) == temp)
            break;

        temp = LINK(Ancestor, SER(temp));
    }

    return false;
}


// Modes allowed by Bind related functions:
enum {
    BIND_0 = 0, // Only bind the words found in the context.
    BIND_DEEP = 1 << 1 // Recurse into sub-blocks.
};


struct Reb_Binder {
  #if !defined(NDEBUG)
    REBLEN count;
  #endif

  #if defined(CPLUSPLUS_11)
    //
    // The C++ debug build can help us make sure that no binder ever fails to
    // get an INIT_BINDER() and SHUTDOWN_BINDER() pair called on it, which
    // would leave lingering binding values on REBSER nodes.
    //
    bool initialized;
    Reb_Binder () { initialized = false; }
    ~Reb_Binder () { assert(not initialized); }
  #endif
};


inline static void INIT_BINDER(struct Reb_Binder *binder) {
  #if !defined(NDEBUG)
    binder->count = 0;

    #ifdef CPLUSPLUS_11
        binder->initialized = true;
    #endif
  #endif
}


inline static void SHUTDOWN_BINDER(struct Reb_Binder *binder) {
  #if !defined(NDEBUG)
    assert(binder->count == 0);

    #ifdef CPLUSPLUS_11
        binder->initialized = false;
    #endif
  #endif

    UNUSED(binder);
}


// Tries to set the binder index, but return false if already there.
//
inline static bool Try_Add_Binder_Index(
    struct Reb_Binder *binder,
    const REBSYM *sym,
    REBINT index
){
    REBSTR *s = m_cast(REBSYM*, sym);
    assert(index != 0);
    REBSER *old_hitch = MISC(Hitch, s);
    if (old_hitch != s and GET_SERIES_FLAG(old_hitch, BLACK))
        return false;  // already has a mapping

    // Not actually managed...but GC doesn't run while binders are active,
    // and we don't want to pay for putting this in the manual tracking list.
    //
    REBARR *new_hitch = Alloc_Singular(
        NODE_FLAG_MANAGED | SERIES_FLAG_BLACK | FLAG_FLAVOR(HITCH)
    );
    CLEAR_SERIES_FLAG(new_hitch, MANAGED);
    Init_Integer(ARR_SINGLE(new_hitch), index);
    node_MISC(Hitch, new_hitch) = old_hitch;

    mutable_MISC(Hitch, s) = new_hitch;

  #if !defined(NDEBUG)
    ++binder->count;
  #endif
    return true;
}


inline static void Add_Binder_Index(
    struct Reb_Binder *binder,
    const REBSYM *s,
    REBINT index
){
    bool success = Try_Add_Binder_Index(binder, s, index);
    assert(success);
    UNUSED(success);
}


inline static REBINT Get_Binder_Index_Else_0( // 0 if not present
    struct Reb_Binder *binder,
    const REBSYM *s
){
    UNUSED(binder);
    REBSER *hitch = MISC(Hitch, s);

    // Only unmanaged hitches are used for binding.
    //
    if (hitch == s or NOT_SERIES_FLAG(hitch, BLACK))
        return 0;
    return VAL_INT32(ARR_SINGLE(ARR(hitch)));
}


inline static REBINT Remove_Binder_Index_Else_0( // return old value if there
    struct Reb_Binder *binder,
    const REBSYM *str
){
    REBSTR *s = m_cast(REBSYM*, str);
    if (MISC(Hitch, s) == s or NOT_SERIES_FLAG(MISC(Hitch, s), BLACK))
        return 0;

    REBARR *hitch = ARR(MISC(Hitch, s));

    REBINT index = VAL_INT32(ARR_SINGLE(hitch));
    mutable_MISC(Hitch, s) = ARR(node_MISC(Hitch, hitch));
    SET_SERIES_FLAG(hitch, MANAGED);  // we didn't manuals track it
    GC_Kill_Series(hitch);

  #if !defined(NDEBUG)
    assert(binder->count > 0);
    --binder->count;
  #endif
    return index;
}


inline static void Remove_Binder_Index(
    struct Reb_Binder *binder,
    const REBSYM *s
){
    REBINT old_index = Remove_Binder_Index_Else_0(binder, s);
    assert(old_index != 0);
    UNUSED(old_index);
}


// Modes allowed by Collect keys functions:
enum {
    COLLECT_ONLY_SET_WORDS = 0,
    COLLECT_ANY_WORD = 1 << 1,
    COLLECT_DEEP = 1 << 2,
    COLLECT_NO_DUP = 1 << 3  // Do not allow dups during collection (for specs)
};

struct Reb_Collector {
    REBFLGS flags;
    REBDSP dsp_orig;
    struct Reb_Binder binder;
};

#define Collector_Index_If_Pushed(collector) \
    ((DSP - (collector)->dsp_orig) + 1)  // index of *next* item to add


// The process of derelativization will resolve a relative value with a
// specific one--storing frame references into cells.  But once that has
// happened, the cell may outlive the frame...but the binding override that
// the frame contributed might still matter.
//
// !!! The functioning of Decay_Series() should be reviewed to see if it
// actually needs to preserve the CTX_ARCHETYPE().  It's not entirely clear
// if the scenarios are meaningful--but Derelativize cannot fail(), and
// it would without this.  It might also put in some "fake" element that
// would fail later, but given that the REBFRM's captured binding can outlive
// the frame that might lose important functionality.
//
inline static REBSER *SPC_BINDING(REBSPC *specifier)
{
    assert(specifier != UNBOUND);
    const REBVAL *rootvar = CTX_ARCHETYPE(CTX(specifier));  // ok if Decay()'d
    assert(IS_FRAME(rootvar));
    return BINDING(rootvar);
}


// If the cell we're writing into is a stack cell, there's a chance that
// management/reification of the binding can be avoided.
//
// Payload and header should be valid prior to making this call.
//
inline static void INIT_BINDING_MAY_MANAGE(
    RELVAL *out,
    const REBSER* binding
){
    mutable_BINDING(out) = binding;

    if (not binding or GET_SERIES_FLAG(binding, MANAGED))
        return;  // unbound or managed already (frame OR object context)

    REBFRM *f = FRM(LINK(KeySource, binding));  // unmanaged only frame
    assert(f->key == f->key_tail);  // cannot manage varlist in mid fulfill!
    UNUSED(f);

    m_cast(REBSER*, binding)->leader.bits |= NODE_FLAG_MANAGED;  // GC sees...
}


// The unbound state for an ANY-WORD! is to hold its spelling.  Once bound,
// the spelling is derived by indexing into the keylist of the binding (if
// bound directly to a context) or into the paramlist (if relative to an
// action, requiring a frame specifier to fully resolve).
//
inline static bool IS_WORD_UNBOUND(const RELVAL *v) {
    assert(ANY_WORD_KIND(CELL_HEART(VAL_UNESCAPED(v))));
    return BINDING(v) == UNBOUND;
}

#define IS_WORD_BOUND(v) \
    (not IS_WORD_UNBOUND(v))


inline static REBLEN VAL_WORD_INDEX(const RELVAL *v) {
    assert(IS_WORD_BOUND(v));
    uint32_t i = VAL_WORD_PRIMARY_INDEX_UNCHECKED(v);
    assert(i > 0);
    return cast(REBLEN, i);
}

inline static REBARR *VAL_WORD_BINDING(const RELVAL *v) {
    assert(ANY_WORD_KIND(CELL_HEART(VAL_UNESCAPED(v))));
    return ARR(BINDING(v));  // could be nullptr / UNBOUND
}

inline static void INIT_VAL_WORD_BINDING(RELVAL *v, const REBSER *binding) {
    assert(ANY_WORD_KIND(CELL_HEART(VAL_UNESCAPED(v))));

    mutable_BINDING(v) = binding;

  #if !defined(NDEBUG)
    if (binding == nullptr)
        return;  // e.g. UNBOUND (words use strings to indicate unbounds)

    if (binding->leader.bits & NODE_FLAG_MANAGED) {
        assert(
            IS_DETAILS(binding)  // relative
            or IS_VARLIST(binding)  // specific
            or IS_PATCH(binding)  // let
        );
    }
    else
        assert(IS_VARLIST(binding));
  #endif
}


// While ideally error messages would give back data that is bound exactly to
// the context that was applicable, threading the specifier into many cases
// can overcomplicate code.  We'd break too many invariants to just say a
// relativized value is "unbound", so make an expired frame if necessary.
//
inline static REBVAL* Unrelativize(RELVAL* out, const RELVAL* v) {
    if (not Is_Bindable(v) or IS_SPECIFIC(v))
        Copy_Cell(out, SPECIFIC(v));
    else {  // must be bound to a function
        REBACT *binding = ACT(VAL_WORD_BINDING(v));
        REBCTX *expired = Make_Expired_Frame_Ctx_Managed(binding);

        Copy_Cell_Header(out, v);
        out->payload = v->payload;
        mutable_BINDING(out) = expired;
    }
    return cast(REBVAL*, out);
}

// This is a super lazy version of unrelativization, which can be used to
// hand a relative value to something like fail(), since fail will clean up
// the stray alloc.
//
#define rebUnrelativize(v) \
    Unrelativize(Alloc_Value(), (v))

inline static void Unbind_Any_Word(RELVAL *v) {
    INIT_VAL_WORD_PRIMARY_INDEX(v, 0);
    INIT_VAL_WORD_BINDING(v, nullptr);
}

inline static REBCTX *VAL_WORD_CONTEXT(const REBVAL *v) {
    assert(IS_WORD_BOUND(v));
    REBARR *binding = VAL_WORD_BINDING(v);
    if (SER_FLAVOR(binding) == FLAVOR_PATCH)
        binding = CTX_VARLIST(LINK(PatchContext, binding));
    assert(
        GET_SERIES_FLAG(binding, MANAGED) or
        FRM(LINK(KeySource, binding))->key
            == FRM(LINK(KeySource, binding))->key_tail  // not fulfilling
    );
    binding->leader.bits |= NODE_FLAG_MANAGED;  // !!! review managing needs
    REBCTX *c = CTX(binding);
    FAIL_IF_INACCESSIBLE_CTX(c);
    return c;
}


//=////////////////////////////////////////////////////////////////////////=//
//
//  VARIABLE ACCESS
//
//=////////////////////////////////////////////////////////////////////////=//
//
// When a word is bound to a context by an index, it becomes a means of
// reading and writing from a persistent storage location.  We use "variable"
// or just VAR to refer to REBVAL slots reached via binding in this way.
// More narrowly, a VAR that represents an argument to a function invocation
// may be called an ARG (and an ARG's "persistence" is only as long as that
// function call is on the stack).
//
// All variables can be put in a CELL_FLAG_PROTECTED state.  This is a flag
// on the variable cell itself--not the key--so different instances of
// the same object sharing the keylist don't all have to be protected just
// because one instance is.  This is not one of the flags included in the
// CELL_MASK_COPIED, so it shouldn't be able to leak out of the varlist.
//
// The Lookup_Word_May_Fail() function takes the conservative default that
// only const access is needed.  A const pointer to a REBVAL is given back
// which may be inspected, but the contents not modified.  While a bound
// variable that is not currently set will return a REB_NULL value,
// Lookup_Word_May_Fail() on an *unbound* word will raise an error.
//
// Lookup_Mutable_Word_May_Fail() offers a parallel facility for getting a
// non-const REBVAL back.  It will fail if the variable is either unbound
// -or- marked with OPT_TYPESET_LOCKED to protect against modification.
//


static inline const REBVAL *Lookup_Word_May_Fail(
    const RELVAL *any_word,
    REBSPC *specifier
){
    REBLEN index;
    REBSER *s = try_unwrap(
        Get_Word_Container(&index, any_word, specifier, ATTACH_READ)
    );
    if (not s)
        fail (Error_Not_Bound_Raw(SPECIFIC(any_word)));
    if (IS_PATCH(s))
        return SPECIFIC(ARR_SINGLE(ARR(s)));
    REBCTX *c = CTX(s);
    if (GET_SERIES_FLAG(CTX_VARLIST(c), INACCESSIBLE))
        fail (Error_No_Relative_Core(any_word));

    return CTX_VAR(c, index);
}

static inline option(const REBVAL*) Lookup_Word(
    const RELVAL *any_word,
    REBSPC *specifier
){
    REBLEN index;
    REBSER *s = try_unwrap(
        Get_Word_Container(&index, any_word, specifier, ATTACH_READ)
    );
    if (not s)
        return nullptr;
    if (IS_PATCH(s))
        return SPECIFIC(ARR_SINGLE(ARR(s)));
    REBCTX *c = CTX(s);
    if (GET_SERIES_FLAG(CTX_VARLIST(c), INACCESSIBLE))
        return nullptr;

    return CTX_VAR(c, index);
}

static inline const REBVAL *Get_Word_May_Fail(
    RELVAL *out,
    const RELVAL* any_word,
    REBSPC *specifier
){
    const REBVAL *var = Lookup_Word_May_Fail(any_word, specifier);
    if (IS_BAD_WORD(var))
        fail (Error_Bad_Word_Get_Core(
            cast(const REBVAL*, any_word), specifier,
            var
        ));

    return Copy_Cell(out, var);
}

static inline REBVAL *Lookup_Mutable_Word_May_Fail(
    const RELVAL* any_word,
    REBSPC *specifier
){
    REBLEN index;
    REBSER *s = try_unwrap(
        Get_Word_Container(&index, any_word, specifier, ATTACH_WRITE)
    );
    if (not s)
        fail (Error_Not_Bound_Raw(SPECIFIC(any_word)));

    REBVAL *var;
    if (IS_PATCH(s))
        var = SPECIFIC(ARR_SINGLE(ARR(s)));
    else {
        REBCTX *c = CTX(s);

        // A context can be permanently frozen (`lock obj`) or temporarily
        // protected, e.g. `protect obj | unprotect obj`.  A native will
        // use SERIES_FLAG_HOLD on a FRAME! context in order to prevent
        // setting values to types with bit patterns the C might crash on.
        //
        // Lock bits are all in SER->info and checked in the same instruction.
        //
        FAIL_IF_READ_ONLY_SER(CTX_VARLIST(c));

        var = CTX_VAR(c, index);
    }

    // The PROTECT command has a finer-grained granularity for marking
    // not just contexts, but individual fields as protected.
    //
    if (GET_CELL_FLAG(var, PROTECTED)) {
        DECLARE_LOCAL (unwritable);
        Init_Word(unwritable, VAL_WORD_SYMBOL(any_word));
        fail (Error_Protected_Word_Raw(unwritable));
    }

    return var;
}

inline static REBVAL *Sink_Word_May_Fail(
    const RELVAL* any_word,
    REBSPC *specifier
){
    REBVAL *var = Lookup_Mutable_Word_May_Fail(any_word, specifier);
    REFORMAT_CELL_IF_DEBUG(var);
    return var;
}


//=////////////////////////////////////////////////////////////////////////=//
//
//  COPYING RELATIVE VALUES TO SPECIFIC
//
//=////////////////////////////////////////////////////////////////////////=//
//
// This can be used to turn a RELVAL into a REBVAL.  If the RELVAL is indeed
// relative and needs to be made specific to be put into the target, then the
// specifier is used to do that.
//
// It is nearly as fast as just assigning the value directly in the release
// build, though debug builds assert that the function in the specifier
// indeed matches the target in the relative value (because relative values
// in an array may only be relative to the function that deep copied them, and
// that is the only kind of specifier you can use with them).
//
// Interface designed to line up with Copy_Cell()
//
// !!! At the moment, there is a fair amount of overlap in this code with
// Get_Context_Core().  One of them resolves a value's real binding and then
// fetches it, while the other resolves a value's real binding but then stores
// that back into another value without fetching it.  This suggests sharing
// a mechanic between both...TBD.
//


#ifdef CPLUSPLUS_11
    REBSPC *Derive_Specifier(
        REBSPC *parent,
        const REBVAL* any_array
    ) = delete;
#endif

inline static REBVAL *Derelativize(
    RELVAL *out,  // relative dest overwritten w/specific value
    const RELVAL *v,
    REBSPC *specifier
){
    Copy_Cell_Header(out, v);
    out->payload = v->payload;
    if (not Is_Bindable(v)) {
        out->extra = v->extra;
        return cast(REBVAL*, out);
    }

    enum Reb_Kind heart = CELL_HEART(VAL_UNESCAPED(v));

    // The specifier is not going to have a say in the derelativized cell.
    // This means any information it encodes must be taken into account now.
    //
    //
    if (ANY_WORD_KIND(heart)) {
        REBLEN index;
        REBSER *s = try_unwrap(
            Get_Word_Container(&index, v, specifier, ATTACH_COPY)
        );
        if (not s) {
            // Getting back NULL here could mean that it's actually unbound,
            // or that it's bound to a "sea" context like User or Lib and
            // there's nothing there...yet.
            //
            out->extra = v->extra;
        }
        else {
            INIT_BINDING_MAY_MANAGE(out, s);
            INIT_VAL_WORD_PRIMARY_INDEX(out, index);
        }

        // When we resolve a word specifically, we clear out the specifier
        // cache.  The same virtual specifier is unlikely to be used with it
        // again (as any new series are pulled out of the "wave" of binding).
        //
        // We don't want to do this with REB_QUOTED since the cache is shared.
        //
        if (KIND3Q_BYTE_UNCHECKED(v) != REB_QUOTED) {
            INIT_VAL_WORD_VIRTUAL_MONDEX(out, MONDEX_MOD);  // necessary?
        }
        return cast(REBVAL*, out);
    }
    else if (ANY_ARRAY_KIND(heart)) {
        //
        // The job of an array in a derelativize operation is to carry along
        // the specifier.  However, it cannot lose any prior existing info
        // that's in the specifier it holds.
        //
        // THE BINDING IN ARRAYS MAY BE UNMANAGED...due to an optimization
        // for passing things to natives that is probably not needed any
        // longer.  Review.
        //
        // The mechanism otherwise is shared with specifier derivation.
        // That includes the case of if specifier==SPECIFIED.
        //
        INIT_BINDING_MAY_MANAGE(out, Derive_Specifier(specifier, v));
    }
    else if (heart == REB_TEXT) {
        //
        // !!! This is the beginning of an idea where strings carry bindings.
        // The information could be taken advantage of by string interpolation.
        // It's not clear which string types should receive this feature, as
        // it carries the risk of making a lot of stray bindings.  For now
        // just try TEXT! and see where it goes.
        //
        INIT_BINDING_MAY_MANAGE(out, specifier);
    }
    else {
        // Things like contexts and varargs are not affected by specifiers,
        // at least not currently.
        //
        out->extra = v->extra;
    }

    return cast(REBVAL*, out);
}


// In the C++ build, defining this overload that takes a REBVAL* instead of
// a RELVAL*, and then not defining it...will tell you that you do not need
// to use Derelativize.  Juse Copy_Cell() if your source is a REBVAL!
//
#ifdef CPLUSPLUS_11
    REBVAL *Derelativize(RELVAL *dest, const REBVAL *v, REBSPC *specifier);
#endif


//=////////////////////////////////////////////////////////////////////////=//
//
//  DETERMINING SPECIFIER FOR CHILDREN IN AN ARRAY
//
//=////////////////////////////////////////////////////////////////////////=//
//
// A relative array must be combined with a specifier in order to find the
// actual context instance where its values can be found.  Since today's
// specifiers are always nothing or a FRAME!'s context, this is fairly easy...
// if you find a specific child value living inside a relative array then
// it's that child's specifier that overrides the specifier in effect.
//
// With virtual binding this could get more complex, since a specifier may
// wish to augment or override the binding in a deep way on read-only blocks.
// That means specifiers may need to be chained together.  This would create
// needs for GC or reference counting mechanics, which may defy a simple
// solution in C89.
//
// But as a first step, this function locates all the places in the code that
// would need such derivation.
//

// A specifier can be a FRAME! context for fulfilling relative words.  Or it
// may be a chain of virtual binds where the last link in the chain is to
// a frame context.
//
// It's Derive_Specifier()'s job to make sure that if specifiers get linked on
// top of each other, the chain always bottoms out on the same FRAME! that
// the original specifier was pointing to.
//
inline static REBNOD** SPC_FRAME_CTX_ADDRESS(REBSPC *specifier)
{
    assert(IS_PATCH(specifier));
    while (
        NextPatch(specifier) != nullptr
        and not IS_VARLIST(NextPatch(specifier))
    ){
        specifier = NextPatch(specifier);
    }
    return &node_INODE(NextPatch, specifier);
}

inline static option(REBCTX*) SPC_FRAME_CTX(REBSPC *specifier)
{
    if (specifier == UNBOUND)  // !!! have caller check?
        return nullptr;
    if (IS_VARLIST(specifier))
        return CTX(specifier);
    return CTX(*SPC_FRAME_CTX_ADDRESS(specifier));
}


// This routine will merge virtual binding patches, returning one where the
// child is at the beginning of the chain.  This will preserve the child's
// frame resolving context (if any) that terminates it.
//
// If the returned chain manages to reuse an existing case, then the result
// will have ARRAY_FLAG_PATCH_REUSED set.  This can inform higher levels of
// whether it's worth searching their patchlist or not...as newly created
// patches can't appear in their prior create list.
//
inline static REBARR *Merge_Patches_May_Reuse(
    REBARR *parent,
    REBARR *child
){
    assert(IS_PATCH(parent));
    assert(IS_PATCH(child));

    // Case of already incorporating.  Came up with:
    //
    //    1 then x -> [2 also y -> [3]]
    //
    // A virtual link for Y is added on top of the virtual link for X that
    // resides on the [3] block.  But then feed generation for [3] tries to
    // apply the Y virtual link again.  !!! Review if that's just inefficient.
    //
    if (NextPatch(parent) == child) {
        SET_SUBCLASS_FLAG(PATCH, parent, REUSED);
        return parent;
    }

    // If we get to the end of the merge chain and don't find the child, then
    // we're going to need a patch that incorporates it.
    //
    REBARR *next;
    bool was_next_reused;
    if (NextPatch(parent) == nullptr or IS_VARLIST(NextPatch(parent))) {
        next = child;
        was_next_reused = true;
    }
    else {
        next = Merge_Patches_May_Reuse(NextPatch(parent), child);
        was_next_reused = GET_SUBCLASS_FLAG(PATCH, next, REUSED);
    }

    // If we have to make a new patch due to non-reuse, then we cannot make
    // one out of a LET, since the patch *is* the variable.  It's actually
    // uncommon for this to happen, but here's an example of how to force it:
    //
    //     block1: do [let x: 10, [x + y]]
    //     block2: do compose/deep [let y: 20, [(block1)]]
    //     30 = do first block2
    //
    // So we have to make a new patch that points to the LET, or promote it
    // (using node-identity magic) into an object.  We point at the LET.
    //
    REBARR *binding;
    REBLEN limit;
    enum Reb_Kind kind;
    if (GET_SUBCLASS_FLAG(PATCH, parent, LET)) {
        binding = parent;
        limit = 1;

        // !!! LET bindings do not have anywhere to put the subclass info of
        // whether they only apply to SET-WORD!s or things like that, so they
        // are always assumed to be "universal bindings".  More granular
        // forms of LET would need to get more bits somehow...either by
        // being a different "flavor" or by making a full object.  We might
        // have just gone ahead and done that here, but having to make an
        // object would bloat things considerably.  Try allowing LET patches
        // to act as the storage to point at by other patches for now.
        //
        kind = REB_WORD;
    }
    else {
        binding = ARR(BINDING(ARR_SINGLE(parent)));
        limit = VAL_WORD_INDEX(ARR_SINGLE(parent));
        kind = VAL_TYPE(ARR_SINGLE(parent));
    }

    return Make_Patch_Core(
        binding,
        limit,
        next,
        kind,
        was_next_reused
    );
}


//
// BINDING CONVENIENCE MACROS
//
// WARNING: Don't pass these routines something like a singular REBVAL* (such
// as a REB_BLOCK) which you wish to have bound.  You must pass its *contents*
// as an array...as the plural "values" in the name implies!
//
// So don't do this:
//
//     REBVAL *block = ARG(block);
//     REBVAL *something = ARG(next_arg_after_block);
//     Bind_Values_Deep(block, context);
//
// What will happen is that the block will be treated as an array of values
// and get incremented.  In the above case it would reach to the next argument
// and bind it too (likely crashing at some point not too long after that).
//
// Instead write:
//
//     Bind_Values_Deep(ARR_HEAD(VAL_ARRAY(block)), context);
//
// That will pass the address of the first value element of the block's
// contents.  You could use a later value element, but note that the interface
// as written doesn't have a length limit.  So although you can control where
// it starts, it will keep binding until it hits an end marker.
//

#define Bind_Values_Deep(at,tail,context) \
    Bind_Values_Core((at), (tail), (context), TS_WORD, 0, BIND_DEEP)

#define Bind_Values_All_Deep(at,tail,context) \
    Bind_Values_Core((at), (tail), (context), TS_WORD, TS_WORD, BIND_DEEP)

#define Bind_Values_Shallow(at,tail,context) \
    Bind_Values_Core((at), (tail), (context), TS_WORD, 0, BIND_0)

// Gave this a complex name to warn of its peculiarities.  Calling with
// just BIND_SET is shallow and tricky because the set words must occur
// before the uses (to be applied to bindings of those uses)!
//
#define Bind_Values_Set_Midstream_Shallow(at,tail,context) \
    Bind_Values_Core( \
        (at), (tail), (context), TS_WORD, FLAGIT_KIND(REB_SET_WORD), BIND_0)

#define Unbind_Values_Deep(at,tail) \
    Unbind_Values_Core((at), (tail), nullptr, true)
