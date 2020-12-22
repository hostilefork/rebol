//
//  File: %sys-action.h
//  Summary: {action! defs AFTER %tmp-internals.h (see: %sys-rebact.h)}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012-2020 Ren-C Open Source Contributors
// Copyright 2012 REBOL Technologies
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
// Using a technique parallel to contexts, an action is a combination of an
// array of named keys (that is potentially shared) as well as an array that
// represents the identity of the action.  The 0th element of that array
// is an archetypal value of the ACTION!.
//
// The keylist for an action is referred to as a "paramlist", but it has the
// same form as a keylist so that it can be used -as- a keylist for FRAME!
// contexts, that represent the instantiated state of an action.  The [0]
// cell is currently unused, while the 1..NUM_PARAMS cells have REB_XXX types
// higher than REB_MAX (e.g. "pseudotypes").  These PARAM cells are not
// intended to be leaked to the user...they indicate the parameter type
// (normal, quoted, local).  The parameter cell's payload holds a typeset, and
// the extra holds the symbol.
//
// The identity array for an action is called its "details".  Beyond having
// an archetype in the [0] position, it is different from a varlist because
// the values have no correspondence with the keys.  Instead, this is the
// instance data used by the C native "dispatcher" function (which lives in
// LINK(details).dispatcher).
//
// What the details array holds varies by dispatcher.  Some examples:
//
//     USER FUNCTIONS: 1-element array w/a BLOCK!, the body of the function
//     GENERICS: 1-element array w/WORD! "verb" (OPEN, APPEND, etc)
//     SPECIALIZATIONS: no contents needed besides the archetype
//     ROUTINES/CALLBACKS: stylized array (REBRIN*)
//     TYPECHECKERS: the TYPESET! to check against
//
// See the comments in the %src/core/functionals/ directory for each function
// variation for descriptions of how they use their details arrays.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// NOTES:
//
// * Unlike contexts, an ACTION! does not have values of its own, only
//   parameter definitions (or "params").  The arguments ("args") come from an
//   action's instantiation on the stack, viewed as a context using a FRAME!.
//
// * Paramlists may contain hidden fields, if they are specializations...
//   because they have to have the right number of slots to line up with the
//   frame of the underlying function.
//
// * The `misc.meta` field of the details holds a meta object (if any) that
//   describes the function.  This is read by help.  A similar facility is
//   enabled by the `misc.meta` field of varlists.
//
// * By storing the C function dispatcher pointer in the `details` array node
//   instead of in the value cell itself, it also means the dispatcher can be
//   HIJACKed--or otherwise hooked to affect all instances of a function.
//



//=//// PSEUDOTYPES FOR RETURN VALUES /////////////////////////////////////=//
//
// An arbitrary cell pointer may be returned from a native--in which case it
// will be checked to see if it is thrown and processed if it is, or checked
// to see if it's an unmanaged API handle and released if it is...ultimately
// putting the cell into f->out.
//
// However, pseudotypes can be used to indicate special instructions to the
// evaluator.
//

// This signals that the evaluator is in a "thrown state".
//
#define R_THROWN \
    cast(REBVAL*, &PG_R_Thrown)

// It is also used by path dispatch when it has taken performing a SET-PATH!
// into its own hands, but doesn't want to bother saying to move the value
// into the output slot...instead leaving that to the evaluator (as a
// SET-PATH! should always evaluate to what was just set)
//
#define R_INVISIBLE \
    cast(REBVAL*, &PG_R_Invisible)

// If Eval_Core gets back an REB_R_REDO from a dispatcher, it will re-execute
// the f->phase in the frame.  This function may be changed by the dispatcher
// from what was originally called.
//
// If EXTRA(Any).flag is not set on the cell, then the types will be checked
// again.  Note it is not safe to let arbitrary user code change values in a
// frame from expected types, and then let those reach an underlying native
// who thought the types had been checked.
//
#define R_REDO_UNCHECKED \
    cast(REBVAL*, &PG_R_Redo_Unchecked)

#define R_REDO_CHECKED \
    cast(REBVAL*, &PG_R_Redo_Checked)


// Path dispatch used to have a return value PE_SET_IF_END which meant that
// the dispatcher itself should realize whether it was doing a path get or
// set, and if it were doing a set then to write the value to set into the
// target cell.  That means it had to keep track of a pointer to a cell vs.
// putting the bits of the cell into the output.  This is now done with a
// special REB_R_REFERENCE type which holds in its payload a RELVAL and a
// specifier, which is enough to be able to do either a read or a write,
// depending on the need.
//
// !!! See notes in %c-path.c of why the R3-Alpha path dispatch is hairier
// than that.  It hasn't been addressed much in Ren-C yet, but needs a more
// generalized design.
//
#define R_REFERENCE \
    cast(REBVAL*, &PG_R_Reference)

// This is used in path dispatch, signifying that a SET-PATH! assignment
// resulted in the updating of an immediate expression in pvs->out, meaning
// it will have to be copied back into whatever reference cell it had been in.
//
#define R_IMMEDIATE \
    cast(REBVAL*, &PG_R_Immediate)

#define R_UNHANDLED \
    cast(REBVAL*, &PG_End_Node)


#define CELL_MASK_ACTION \
    (CELL_FLAG_FIRST_IS_NODE | CELL_FLAG_SECOND_IS_NODE)

#define VAL_ACTION_DETAILS_NODE(v) \
    PAYLOAD(Any, (v)).first.node  // lvalue, but a node

#define VAL_ACTION_SPECIALTY_OR_LABEL_NODE(v) \
    PAYLOAD(Any, (v)).second.node  // lvalue, but a node

#define VAL_ACTION_BINDING_NODE(v) \
    EXTRA(Any, (v)).node


inline static REBARR *ACT_DETAILS(REBACT *a) {
    assert(GET_ARRAY_FLAG(&a->details, IS_DETAILS));
    return &a->details;
}

inline static REBCTX *VAL_ACTION_BINDING(unstable REBCEL(const*) v) {
    assert(CELL_HEART(v) == REB_ACTION);
    REBNOD *binding = VAL_ACTION_BINDING_NODE(v);
    assert(
        binding == nullptr
        or (binding->header.bits & ARRAY_FLAG_IS_VARLIST)
    );
    return CTX(binding);  // !!! should do assert above, review build flags
}

inline static void INIT_VAL_ACTION_BINDING(
    unstable RELVAL *v,
    REBCTX *binding
){
    assert(IS_ACTION(v));
    VAL_ACTION_BINDING_NODE(v) = NOD(binding);
}


// An action's "archetype" is data in the head cell (index [0]) of the array
// that is the paramlist.  This is an ACTION! cell which must have its
// paramlist value match the paramlist it is in.  So when copying one array
// to make a new paramlist from another, you must ensure the new array's
// archetype is updated to match its container.

#define ACT_ARCHETYPE(a) \
    SPECIFIC(ARR_AT(ACT_DETAILS(a), 0))


#define ACT_SPECIALTY(a) \
    ARR(VAL_ACTION_SPECIALTY_OR_LABEL_NODE(ACT_ARCHETYPE(a)))

inline static REBARR *ACT_PARAMLIST(REBACT *a) {
    REBARR *specialty = ACT_SPECIALTY(a);
    if (GET_ARRAY_FLAG(specialty, IS_VARLIST))
        return ARR(LINK_KEYSOURCE(specialty));
    return specialty;
}

#define ACT_DISPATCHER(a) \
    (LINK(ACT_DETAILS(a)).dispatcher)

#define DETAILS_AT(a,n) \
    SPECIFIC(STABLE(ARR_AT((a), (n))))

#define IDX_DETAILS_1 1  // Common index used for code body location

// These are indices into the details array agreed upon by actions which have
// the PARAMLIST_FLAG_IS_NATIVE set.
//
#define IDX_NATIVE_BODY 1 // text string source code of native (for SOURCE)
#define IDX_NATIVE_CONTEXT 2 // libRebol binds strings here (and lib)
#define IDX_NATIVE_MAX (IDX_NATIVE_CONTEXT + 1)

inline static REBVAL *ACT_PARAM(REBACT *a, REBLEN n) {
    assert(n != 0 and n < ARR_LEN(ACT_PARAMLIST(a)));
    return SER_AT(REBVAL, SER(ACT_PARAMLIST(a)), n);
}

#define ACT_NUM_PARAMS(a) \
    (cast(REBSER*, ACT_PARAMLIST(a))->content.dynamic.used - 1) // dynamic


//=//// META OBJECT ///////////////////////////////////////////////////////=//
//
// ACTION! details and ANY-CONTEXT! varlists can store a "meta" object.  It's
// where information for HELP is saved, and it's how modules store out-of-band
// information that doesn't appear in their body.

#define ACT_META(a) \
    CTX(MISC_META_NODE(ACT_DETAILS(a)))


// An efficiency trick makes functions that do not have exemplars NOT store
// nullptr in the LINK_SPECIALTY(info) node in that case--instead the params.
// This makes Push_Action() slightly faster in assigning f->special.
//
inline static REBCTX *ACT_EXEMPLAR(REBACT *a) {
    REBARR *specialty = ACT_SPECIALTY(a);
    if (GET_ARRAY_FLAG(specialty, IS_VARLIST))
        return CTX(specialty);

    return nullptr;
}

inline static REBVAL *ACT_SPECIALTY_HEAD(REBACT *a) {
    REBSER *s = SER(ACT_SPECIALTY(a));
    return cast(REBVAL*, s->content.dynamic.data) + 1; // skip archetype/root
}


// There is no binding information in a function parameter (typeset) so a
// REBVAL should be okay.
//
#define ACT_PARAMS_HEAD(a) \
    (cast(REBVAL*, SER(ACT_PARAMLIST(a))->content.dynamic.data) + 1)

inline static REBACT *VAL_ACTION(unstable REBCEL(const*) v) {
    assert(CELL_KIND(v) == REB_ACTION); // so it works on literals
    REBSER *s = SER(VAL_ACTION_DETAILS_NODE(v));
    if (GET_SERIES_INFO(s, INACCESSIBLE))
        fail (Error_Series_Data_Freed_Raw());
    return ACT(s);
}

#define VAL_ACTION_PARAMLIST(v) \
    ACT_PARAMLIST(VAL_ACTION(v))


//=//// ACTION LABELING ///////////////////////////////////////////////////=//
//
// When an ACTION! is stored in a cell (e.g. not an "archetype"), it can
// contain a label of the ANY-WORD! it was taken from.  If it is an array
// node, it is presumed an archetype and has no label.
//
// !!! Theoretically, longer forms like `.not.equal?` for PREDICATE! could
// use an array node here.  But since CHAINs store ACTION!s that can cache
// the words, you get the currently executing label instead...which may
// actually make more sense.

inline static const REBSTR *VAL_ACTION_LABEL(unstable const RELVAL *v) {
    assert(IS_ACTION(v));
    REBSER *s = SER(VAL_ACTION_SPECIALTY_OR_LABEL_NODE(v));
    if (IS_SER_ARRAY(s))
        return ANONYMOUS;  // archetype (e.g. may live in paramlist[0] itself)
    return STR(s);
}

inline static void INIT_ACTION_LABEL(unstable RELVAL *v, const REBSTR *label)
{
    // !!! How to be certain this isn't an archetype node?  The GC should
    // catch any violations when a paramlist[0] isn't an array...
    //
    ASSERT_CELL_WRITABLE_EVIL_MACRO(v, __FILE__, __LINE__);
    assert(label != nullptr);  // avoid needing to worry about null case
    VAL_ACTION_SPECIALTY_OR_LABEL_NODE(v) = NOD(m_cast(REBSTR*, label));
}


//=//// ANCESTRY / FRAME COMPATIBILITY ////////////////////////////////////=//
//
// On the keylist of an object, LINK_ANCESTOR points at a keylist which has
// the same number of keys or fewer, which represents an object which this
// object is derived from.  Note that when new object instances are
// created which do not require expanding the object, their keylist will
// be the same as the object they are derived from.
//
// Paramlists have the same relationship, with each expansion (e.g. via
// AUGMENT) having larger frames pointing to the potentially shorter frames.
// (Something that reskins a paramlist might have the same size frame, with
// members that have different properties.)
//
// When you build a frame for an expanded action (e.g. with an AUGMENT) then
// it can be used to run phases that are from before it in the ancestry chain.
// This informs low-level asserts inside of the specific binding machinery, as
// well as determining whether higher-level actions can be taken (like if a
// sibling tail call would be legal, or if a certain HIJACK would be safe).
//
// !!! When ancestors were introduced, it was prior to AUGMENT and so frames
// did not have a concept of expansion.  So they only applied to keylists.
// The code for processing derivation is slightly different; it should be
// unified more if possible.

#define LINK_ANCESTOR(s)            ARR(LINK_ANCESTOR_NODE(s))

inline static bool Action_Is_Base_Of(REBACT *base, REBACT *derived) {
    if (derived == base)
        return true;  // fast common case (review how common)

    REBARR *paramlist_test = ACT_PARAMLIST(derived);
    REBARR *paramlist_base = ACT_PARAMLIST(base);
    while (true) {
        if (paramlist_test == paramlist_base)
            return true;

        REBARR *ancestor = LINK_ANCESTOR(paramlist_test);
        if (ancestor == paramlist_test)
            return false;  // signals end of the chain, no match found

        paramlist_test = ancestor;
    }
}

inline static REBVAL *Voidify_Rootparam(REBARR *paramlist) {
    //
    // !!! Since the voidification is to comply with systemic rules, we also
    // comply with the rule that the ancestor can't be trash here.  Review.
    //
    assert(IS_POINTER_SAFETRASH_DEBUG(LINK_ANCESTOR_NODE(paramlist)));
    LINK_ANCESTOR_NODE(paramlist) = NOD(paramlist);

    return Init_Unreadable_Void(ARR_HEAD(paramlist)); 
}


//=//// RETURN HANDLING (WIP) /////////////////////////////////////////////=//
//
// The well-understood and working part of definitional return handling is
// that function frames have a local slot named RETURN.  This slot is filled
// by the dispatcher before running the body, with a function bound to the
// executing frame.  This way it knows where to return to.
//
// !!! Lots of other things are not worked out (yet):
//
// * How do function derivations share this local cell (or do they at all?)
//   e.g. if an ADAPT has prelude code, that code runs before the original
//   dispatcher would fill in the RETURN.  Does the cell hold a return whose
//   phase meaning changes based on which phase is running (which the user
//   could not do themselves)?  Or does ADAPT need its own RETURN?  Or do
//   ADAPTs just not have returns?
//
// * The typeset in the RETURN local key is where legal return types are
//   stored (in lieu of where a parameter would store legal argument types).
//   Derivations may wish to change this.  Needing to generate a whole new
//   paramlist just to change the return type seems excessive.
//
// * To make the position of RETURN consistent and easy to find, it is moved
//   to the first parameter slot of the paramlist (regardless of where it
//   is declared).  This complicates the paramlist building code, and being
//   at that position means it often needs to be skipped over (e.g. by a
//   GENERIC which wants to dispatch on the type of the first actual argument)
//   The ability to create functions that don't have a return complicates
//   this mechanic as well.
//
// The only bright idea in practice right now is that parameter lists which
// have a definitional return in the first slot have a flag saying so.  Much
// more design work on this is needed.

#define ACT_HAS_RETURN(a) \
    (did (SER(ACT_PARAMLIST(a))->header.bits & PARAMLIST_FLAG_HAS_RETURN))


//=//// NATIVE ACTION ACCESS //////////////////////////////////////////////=//
//
// Native values are stored in an array at boot time.  These are convenience
// routines for accessing them, which should compile to be as efficient as
// fetching any global pointer.

#define NATIVE_ACT(name) \
    Natives[N_##name##_ID]

#define NATIVE_VAL(name) \
    ACT_ARCHETYPE(NATIVE_ACT(name))


// A fully constructed action can reconstitute the ACTION! REBVAL
// that is its canon form from a single pointer...the REBVAL sitting in
// the 0 slot of the action's details.  That action has no binding and
// no label.
//
static inline REBVAL *Init_Action(
    unstable RELVAL *out,
    REBACT *a,
    option(const REBSTR*) label,  // allowed to be ANONYMOUS
    REBCTX *binding  // allowed to be UNBOUND
){
  #if !defined(NDEBUG)
    Extra_Init_Action_Checks_Debug(a);
  #endif
    Force_Array_Managed(ACT_DETAILS(a));
    Move_Value(out, ACT_ARCHETYPE(a));
    if (label)
        INIT_ACTION_LABEL(out, unwrap(label));
    else {
        // leave as the array from the archetype (array means not a label)
    }
    assert(VAL_ACTION_BINDING(out) == UNBOUND);
    VAL_ACTION_BINDING_NODE(out) = NOD(binding);
    return cast(REBVAL*, out);
}


inline static REB_R Run_Generic_Dispatch(
    const REBVAL *first_arg,  // !!! Is this always same as FRM_ARG(f, 1)?
    REBFRM *f,
    const REBVAL *verb
){
    assert(IS_WORD(verb));

    GENERIC_HOOK *hook = IS_QUOTED(first_arg)
        ? &T_Quoted  // a few things like COPY are supported by QUOTED!
        : Generic_Hook_For_Type_Of(first_arg);

    REB_R r = hook(f, verb);  // Note that QUOTED! has its own hook & handling
    if (r == R_UNHANDLED) {
        //
        // !!! Improve this error message when used with REB_CUSTOM (right now
        // will just say "cannot use verb with CUSTOM!", regardless of if it
        // is an IMAGE! or VECTOR! or GOB!...)
        //
        fail (Error_Cannot_Use_Raw(
            verb,
            Datatype_From_Kind(VAL_TYPE(first_arg))
        ));
    }

    return r;
}


// The action frame run dispatchers, which get to take over the STATE_BYTE()
// of the frame for their own use.  But before then, the state byte is used
// by action dispatch itself.
//
// So if f->param is END, then this state is not meaningful.
//
enum {
    ST_ACTION_INITIAL_ENTRY = 0,  // is separate "fulfilling" state needed?
    ST_ACTION_TYPECHECKING,
    ST_ACTION_DISPATCHING
};


inline static bool Process_Action_Throws(REBFRM *f) {
    Init_Empty_Nulled(f->out);
    SET_CELL_FLAG(f->out, OUT_MARKED_STALE);
    bool threw = Process_Action_Maybe_Stale_Throws(f);
    CLEAR_CELL_FLAG(f->out, OUT_MARKED_STALE);
    return threw;
}
