//
//  File: %sys-frame.h
//  Summary: {Accessors and Argument Pushers/Poppers for Function Call Frames}
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Revolt Open Source Contributors
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
// A single FRAME! can go through multiple phases of evaluation, some of which
// should expose more fields than others.  For instance, when you specialize
// a function that has 10 parameters so it has only 8, then the specialization
// frame should not expose the 2 that have been removed.  It's as if the
// KEYS OF the spec is shorter than the actual length which is used.
//
// Hence, each independent value that holds a frame must remember the function
// whose "view" it represents.  This field is only applicable to frames, and
// so it could be used for something else on other types
//
// Note that the binding on a FRAME! can't be used for this purpose, because
// it's already used to hold the binding of the function it represents.  e.g.
// if you have a definitional return value with a binding, and try to
// MAKE FRAME! on it, the paramlist alone is not enough to remember which
// specific frame that function should exit.
//

// !!! Find a better place for this!
//
inline static bool IS_QUOTABLY_SOFT(const RELVAL *v) {
    return IS_GROUP(v) or IS_GET_WORD(v) or IS_GET_PATH(v);
}


//=////////////////////////////////////////////////////////////////////////=//
//
//  LOW-LEVEL FRAME ACCESSORS
//
//=////////////////////////////////////////////////////////////////////////=//


inline static bool FRM_IS_VALIST(REBFRM *f) {
    return f->feed->vaptr != nullptr;
}

inline static REBARR *FRM_ARRAY(REBFRM *f) {
    assert(IS_END(f->feed->value) or not FRM_IS_VALIST(f));
    return f->feed->array;
}

// !!! Though the evaluator saves its `index`, the index is not meaningful
// in a valist.  Also, if `opt_head` values are used to prefetch before an
// array, those will be lost too.  A true debugging mode would need to
// convert these cases to ordinary arrays before running them, in order
// to accurately present any errors.
//
inline static REBLEN FRM_INDEX(REBFRM *f) {
    if (IS_END(f->feed->value))
        return ARR_LEN(f->feed->array);

    assert(not FRM_IS_VALIST(f));
    return f->feed->index - 1;
}

inline static REBLEN FRM_EXPR_INDEX(REBFRM *f) {
    assert(not FRM_IS_VALIST(f));
    return f->expr_index - 1;
}

inline static REBSTR* FRM_FILE(REBFRM *f) { // https://trello.com/c/K3vntyPx
    if (not f->feed->array)
        return nullptr;
    if (NOT_ARRAY_FLAG(f->feed->array, HAS_FILE_LINE_UNMASKED))
        return nullptr;
    return STR(LINK(f->feed->array).custom.node);
}

inline static const char* FRM_FILE_UTF8(REBFRM *f) {
    //
    // !!! Note: Too early in boot at the moment to use Canon(__ANONYMOUS__).
    //
    REBSTR *str = FRM_FILE(f);
    return str ? STR_UTF8(str) : "(anonymous)"; 
}

inline static int FRM_LINE(REBFRM *f) {
    if (not f->feed->array)
        return 0;
    if (NOT_ARRAY_FLAG(f->feed->array, HAS_FILE_LINE_UNMASKED))
        return 0;
    return MISC(SER(f->feed->array)).line;
}

#define FRM_OUT(f) \
    (f)->out


// Note about FRM_NUM_ARGS: A native should generally not detect the arity it
// was invoked with, (and it doesn't make sense as most implementations get
// the full list of arguments and refinements).  However, ACTION! dispatch
// has several different argument counts piping through a switch, and often
// "cheats" by using the arity instead of being conditional on which action
// ID ran.  Consider when reviewing the future of ACTION!.
//
#define FRM_NUM_ARGS(f) \
    (cast(REBSER*, (f)->varlist)->content.dynamic.used - 1) // minus rootvar

#define FRM_SPARE(f) \
    cast(REBVAL*, &(f)->spare)

#define FRM_PRIOR(f) \
    ((f)->prior + 0) // prevent assignment via this macro

#define FRM_PHASE(f) \
    VAL_PHASE_UNCHECKED((f)->rootvar)  // shoud be valid--unchecked for speed

#define INIT_FRM_PHASE(f,phase) \
    INIT_VAL_CONTEXT_PHASE((f)->rootvar, (phase))

#define FRM_BINDING(f) \
    EXTRA(Binding, (f)->rootvar).node

#define FRM_UNDERLYING(f) \
    ACT_UNDERLYING((f)->original)

#define FRM_DSP_ORIG(f) \
    ((f)->dsp_orig + 0) // prevent assignment via this macro

#define F_VALUE(f) \
    ((f)->feed->value)


// ARGS is the parameters and refinements
// 1-based indexing into the arglist (0 slot is for FRAME! value)

#define FRM_ARGS_HEAD(f) \
    ((f)->rootvar + 1)

#ifdef NDEBUG
    #define FRM_ARG(f,n) \
        ((f)->rootvar + (n))
#else
    inline static REBVAL *FRM_ARG(REBFRM *f, REBLEN n) {
        assert(n != 0 and n <= FRM_NUM_ARGS(f));

        REBVAL *var = f->rootvar + n; // 1-indexed
        assert(not IS_RELATIVE(cast(RELVAL*, var)));
        return var;
    }
#endif


inline static void Get_Frame_Label_Or_Blank(RELVAL *out, REBFRM *f) {
    assert(Is_Action_Frame(f));
    if (f->opt_label != NULL)
        Init_Word(out, f->opt_label); // invoked via WORD! or PATH!
    else
        Init_Blank(out); // anonymous invocation
}

inline static const char* Frame_Label_Or_Anonymous_UTF8(REBFRM *f) {
    assert(Is_Action_Frame(f));
    if (f->opt_label != NULL)
        return STR_UTF8(f->opt_label);
    return "[anonymous]";
}


//=////////////////////////////////////////////////////////////////////////=//
//
//  DO's LOWEST-LEVEL EVALUATOR HOOKING
//
//=////////////////////////////////////////////////////////////////////////=//
//
// This API is used internally in the implementation of Eval_Core.  It does
// not speak in terms of arrays or indices, it works entirely by setting
// up a call frame (f), and threading that frame's state through successive
// operations, vs. setting it up and disposing it on each EVALUATE step.
//
// Like higher level APIs that move through the input series, this low-level
// API can move at full EVALUATE intervals.  Unlike the higher APIs, the
// possibility exists to move by single elements at a time--regardless of
// if the default evaluation rules would consume larger expressions.  Also
// making it different is the ability to resume after an EVALUATE on value
// sources that aren't random access (such as C's va_arg list).
//
// One invariant of access is that the input may only advance.  Before any
// operations are called, any low-level client must have already seeded
// f->value with a valid "fetched" REBVAL*.
//
// This privileged level of access can be used by natives that feel they can
// optimize performance by working with the evaluator directly.

inline static void Free_Frame_Internal(REBFRM *f) {
    if (GET_EVAL_FLAG(f, ALLOCATED_FEED))
        Free_Feed(f->feed);  // didn't inherit from parent, and not END_FRAME

    Free_Node(FRM_POOL, NOD(f));

    TRASH_POINTER_IF_DEBUG(f->executor);

  #if !defined(NDEBUG)
    f->initial_flags = 0;  // help tell it's free (no EVAL_MASK_DEFAULT)
  #endif
}


inline static void Reuse_Varlist_If_Available(REBFRM *f) {
    assert(IS_POINTER_TRASH_DEBUG(f->varlist));
    if (not TG_Reuse)
        f->varlist = nullptr;
    else {
        f->varlist = TG_Reuse;
        TG_Reuse = LINK(TG_Reuse).reuse;
        f->rootvar = cast(REBVAL*, SER(f->varlist)->content.dynamic.data);
        LINK_KEYSOURCE(f->varlist) = NOD(f);
    }
}

inline static void Push_Frame_No_Varlist(REBVAL *out, REBFRM *f)
{
    assert(f->feed->value != nullptr);

    // All calls through to Eval_Core() are assumed to happen at the same C
    // stack level for a pushed frame (though this is not currently enforced).
    // Hence it's sufficient to check for C stack overflow only once, e.g.
    // not on each Eval_Step_Throws() for `reduce [a | b | ... | z]`.
    //
    // !!! This method is being replaced by "stackless", as there is no
    // reliable platform independent method for detecting stack overflows.
    //
    if (C_STACK_OVERFLOWING(&f)) {
        Free_Frame_Internal(f);  // not in stack, feed + frame wouldn't free
        Fail_Stack_Overflow();
    }

    // Frames are pushed to reuse for several sequential operations like
    // ANY, ALL, CASE, REDUCE.  It is allowed to change the output cell for
    // each evaluation.  But the GC expects initialized bits in the output
    // slot at all times; use null until first eval call if needed
    //
    f->out = out;

    // Though we can protect the value written into the target pointer 'out'
    // from GC during the course of evaluation, we can't protect the
    // underlying value from relocation.  Technically this would be a problem
    // for any series which might be modified while this call is running, but
    // most notably it applies to the data stack--where output used to always
    // be returned.
    //
    // !!! A non-contiguous data stack which is not a series is a possibility.
    //
  #ifdef STRESS_CHECK_DO_OUT_POINTER
    REBNOD *containing;
    if (
        did (containing = Try_Find_Containing_Node_Debug(f->out))
        and not (containing->header.bits & NODE_FLAG_CELL)
        and NOT_SERIES_FLAG(containing, DONT_RELOCATE)
    ){
        printf("Request for ->out location in movable series memory\n");
        panic (containing);
    }
  #else
    assert(not IN_DATA_STACK_DEBUG(f->out));
  #endif

  #ifdef DEBUG_EXPIRED_LOOKBACK
    f->stress = nullptr;
  #endif

    // The arguments to functions in their frame are exposed via FRAME!s
    // and through WORD!s.  This means that if you try to do an evaluation
    // directly into one of those argument slots, and run arbitrary code
    // which also *reads* those argument slots...there could be trouble with
    // reading and writing overlapping locations.  So unless a function is
    // in the argument fulfillment stage (before the variables or frame are
    // accessible by user code), it's not legal to write directly into an
    // argument slot.  :-/
    //
  #if !defined(NDEBUG)
    REBFRM *ftemp = FS_TOP;
    for (; ftemp != FS_BOTTOM; ftemp = ftemp->prior) {
        if (not Is_Action_Frame(ftemp))
            continue;
        if (Is_Action_Frame_Fulfilling_Unchecked(ftemp))
            continue;
        if (GET_SERIES_INFO(ftemp->varlist, INACCESSIBLE))
            continue; // Encloser_Dispatcher() reuses args from up stack
        assert(
            f->out < FRM_ARGS_HEAD(ftemp)
            or f->out >= FRM_ARGS_HEAD(ftemp) + FRM_NUM_ARGS(ftemp)
        );
    }
  #endif

    // Some initialized bit pattern is needed to check to see if a
    // function call is actually in progress, or if eval_type is just
    // REB_ACTION but doesn't have valid args/state.  The original action is a
    // good choice because it is only affected by the function call case,
    // see Is_Action_Frame_Fulfilling().
    //
    f->original = nullptr;

    TRASH_POINTER_IF_DEBUG(f->opt_label);
  #if defined(DEBUG_FRAME_LABELS)
    TRASH_POINTER_IF_DEBUG(f->label_utf8);
  #endif

  #if !defined(NDEBUG)
    //
    // !!! TBD: the relevant file/line update when f->feed->array changes
    //
    f->file = FRM_FILE_UTF8(f);
    f->line = FRM_LINE(f);
  #endif

    f->prior = TG_Top_Frame;
    TG_Top_Frame = f;

    // If the source for the frame is a REBARR*, then we want to temporarily
    // lock that array against mutations.  
    //
    if (IS_END(f->feed->value)) {  // don't take hold on empty feeds
        assert(IS_POINTER_TRASH_DEBUG(f->feed->pending));
        assert(NOT_FEED_FLAG(f->feed, TOOK_HOLD));
    }
    else if (FRM_IS_VALIST(f)) {
        //
        // There's nothing to put a hold on while it's a va_list-based frame.
        // But a GC might occur and "Reify" it, in which case the array
        // which is created will have a hold put on it to be released when
        // the frame is finished.
        //
        assert(NOT_FEED_FLAG(f->feed, TOOK_HOLD));
    }
    else {
        if (GET_SERIES_INFO(f->feed->array, HOLD))
            NOOP; // already temp-locked
        else {
            SET_SERIES_INFO(f->feed->array, HOLD);
            SET_FEED_FLAG(f->feed, TOOK_HOLD);
        }
    }

  #if defined(DEBUG_BALANCE_STATE)
    SNAP_STATE(&f->state); // to make sure stack balances, etc.
    f->state.dsp = f->dsp_orig;
  #endif

  #if !defined(NDEBUG)
    f->initial_flags = f->flags.bits;
  #endif

    // Eval_Core() expects a varlist to be in the frame, therefore it must
    // be filled in by Reuse_Varlist(), or if this is something like a DO
    // of a FRAME! it needs to be filled in from that frame before eval'ing.
    //
    TRASH_POINTER_IF_DEBUG(f->varlist);
}

inline static void Push_Frame(REBVAL *out, REBFRM *f)
{
    Push_Frame_No_Varlist(out, f);
    Reuse_Varlist_If_Available(f);
}

inline static void UPDATE_EXPRESSION_START(REBFRM *f) {
    f->expr_index = f->feed->index; // this is garbage if EVAL_FLAG_VA_LIST
}


#define Literal_Next_In_Frame(out,f) \
    Literal_Next_In_Feed((out), (f)->feed)


inline static void Abort_Frame(REBFRM *f) {
    if (f->varlist and NOT_SERIES_FLAG(f->varlist, MANAGED))
        GC_Kill_Series(SER(f->varlist));  // not alloc'd with manuals tracking
    TRASH_POINTER_IF_DEBUG(f->varlist);

    // Abort_Frame() handles any work that wouldn't be done done naturally by
    // feeding a frame to its natural end.
    // 
    if (IS_END(f->feed->value))
        goto pop;

    if (FRM_IS_VALIST(f)) {
        assert(NOT_FEED_FLAG(f->feed, TOOK_HOLD));

        // Aborting valist frames is done by just feeding all the values
        // through until the end.  This is assumed to do any work, such
        // as SINGULAR_FLAG_API_RELEASE, which might be needed on an item.  It
        // also ensures that va_end() is called, which happens when the frame
        // manages to feed to the end.
        //
        // Note: While on many platforms va_end() is a no-op, the C standard
        // is clear it must be called...it's undefined behavior to skip it:
        //
        // http://stackoverflow.com/a/32259710/211160

        // !!! Since we're not actually fetching things to run them, this is
        // overkill.  A lighter sweep of the va_list pointers that did just
        // enough work to handle rebR() releases, and va_end()ing the list
        // would be enough.  But for the moment, it's more important to keep
        // all the logic in one place than to make variadic interrupts
        // any faster...they're usually reified into an array anyway, so
        // the frame processing the array will take the other branch.

        while (NOT_END(f->feed->value))
            Fetch_Next_Forget_Lookback(f);
    }
    else {
        if (GET_FEED_FLAG(f->feed, TOOK_HOLD)) {
            //
            // The frame was either never variadic, or it was but got spooled
            // into an array by Reify_Va_To_Array_In_Frame()
            //
            assert(GET_SERIES_INFO(f->feed->array, HOLD));
            CLEAR_SERIES_INFO(f->feed->array, HOLD);
            CLEAR_FEED_FLAG(f->feed, TOOK_HOLD); // !!! needed?
        }
    }

  pop:
    assert(TG_Top_Frame == f);
    TG_Top_Frame = f->prior;
    Free_Frame_Internal(f);
}


inline static void Drop_Frame_Core(REBFRM *f) {
  #ifdef DEBUG_ENSURE_FRAME_EVALUATES
    assert(f->was_eval_called);  // must call evaluator--even on empty array
  #endif

  #if defined(DEBUG_EXPIRED_LOOKBACK)
    free(f->stress);
  #endif

    if (f->varlist) {
        assert(NOT_SERIES_FLAG(f->varlist, MANAGED));
        LINK(f->varlist).reuse = TG_Reuse;
        TG_Reuse = f->varlist;
    }
    TRASH_POINTER_IF_DEBUG(f->varlist);

    assert(TG_Top_Frame == f);
    TG_Top_Frame = f->prior;
    Free_Frame_Internal(f);
}

inline static void Drop_Frame_Unbalanced(REBFRM *f) {
    Drop_Frame_Core(f);
}

inline static void Drop_Frame(REBFRM *f)
{
  #if defined(DEBUG_BALANCE_STATE)
    //
    // To avoid slowing down the debug build a lot, Eval_Core() doesn't
    // check this every cycle, just on drop.  But if it's hard to find which
    // exact cycle caused the problem, see BALANCE_CHECK_EVERY_EVALUATION_STEP
    //
    f->state.dsp = DSP; // e.g. Reduce_To_Stack_Throws() doesn't want check
    ASSERT_STATE_BALANCED(&f->state);
  #endif

    assert(DSP == f->dsp_orig); // Drop_Frame_Core() does not check
    Drop_Frame_Unbalanced(f);
}

inline static void Prep_Frame_Core(REBFRM *f, REBFED *feed, REBFLGS flags) {
    assert(NOT_FEED_FLAG(feed, BARRIER_HIT));  // couldn't do anything

    // We could OR the required true flags in automatically, but this helps
    // to ensure that EVAL_MASK_DEFAULT was OR'd in...which may have some
    // flags that want to be true by default but are optionally flipped off.
    //
    assert(
        (flags & (
            EVAL_FLAG_0_IS_TRUE
            | EVAL_FLAG_1_IS_FALSE
            | EVAL_FLAG_4_IS_TRUE
            | EVAL_FLAG_7_IS_TRUE
        )) == (EVAL_FLAG_0_IS_TRUE | EVAL_FLAG_4_IS_TRUE | EVAL_FLAG_7_IS_TRUE)
    );
    f->flags.bits = flags;

    f->feed = feed;
    Prep_Stack_Cell(&f->spare);
    Init_Unreadable_Blank(&f->spare);
    f->dsp_orig = DS_Index;
    TRASH_POINTER_IF_DEBUG(f->out);

    f->executor = &Eval_New_Expression;  // !!! should we not default it?

  #ifdef DEBUG_ENSURE_FRAME_EVALUATES
    f->was_eval_called = false;
  #endif
}

#define DECLARE_FRAME(name,feed,flags) \
    REBFRM * name = cast(REBFRM*, Make_Node(FRM_POOL)); \
    Prep_Frame_Core(name, (feed), (flags));

#define DECLARE_FRAME_AT(name,any_array,flags) \
    DECLARE_FEED_AT (name##feed, (any_array)); \
    DECLARE_FRAME (name, name##feed, (flags) | EVAL_FLAG_ALLOCATED_FEED)

#define DECLARE_FRAME_AT_CORE(name,any_array,specifier,flags) \
    DECLARE_FEED_AT_CORE (name##feed, (any_array), (specifier)); \
    DECLARE_FRAME (name, name##feed, (flags) | EVAL_FLAG_ALLOCATED_FEED)

#define DECLARE_END_FRAME(name,flags) \
    DECLARE_FRAME (name, &TG_Frame_Feed_End, (flags))


inline static void Begin_Action_Core(REBFRM *f, REBSTR *opt_label, bool enfix)
{
    assert(NOT_EVAL_FLAG(f, RUNNING_ENFIX));
    assert(NOT_FEED_FLAG(f->feed, DEFERRING_ENFIX));

    assert(not f->original);
    f->original = FRM_PHASE(f);

    assert(IS_POINTER_TRASH_DEBUG(f->opt_label)); // only valid w/REB_ACTION
    assert(not opt_label or GET_SERIES_FLAG(opt_label, IS_STRING));
    f->opt_label = opt_label;
  #if defined(DEBUG_FRAME_LABELS) // helpful for looking in the debugger
    f->label_utf8 = cast(const char*, Frame_Label_Or_Anonymous_UTF8(f));
  #endif

    assert(NOT_EVAL_FLAG(f, REQUOTE_NULL));
    f->requotes = 0;

    // There's a current state for the FEED_FLAG_NO_LOOKAHEAD which invisible
    // actions want to put back as it was when the invisible operation ends.
    // (It gets overwritten during the invisible's own argument gathering).
    // Cache it on the varlist and put it back when an R_INVISIBLE result
    // comes back.
    //
    if (GET_ACTION_FLAG(f->original, IS_INVISIBLE)) {
        if (GET_FEED_FLAG(f->feed, NO_LOOKAHEAD)) {
            assert(GET_EVAL_FLAG(f, FULFILLING_ARG));
            CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
            SET_SERIES_INFO(f->varlist, TELEGRAPH_NO_LOOKAHEAD);
        }
    }

    if (enfix) {
        SET_EVAL_FLAG(f, RUNNING_ENFIX);  // set for duration of function call
        SET_FEED_FLAG(f->feed, NEXT_ARG_FROM_OUT);  // only during first arg

        // All the enfix call sites cleared this flag on the feed, so it was
        // moved into the Begin_Enfix_Action() case.  Note this has to be done
        // *after* the existing flag state has been captured for invisibles.
        //
        CLEAR_FEED_FLAG(f->feed, NO_LOOKAHEAD);
    }
}

#define Begin_Enfix_Action(f,opt_label) \
    Begin_Action_Core((f), (opt_label), true)

#define Begin_Prefix_Action(f,opt_label) \
    Begin_Action_Core((f), (opt_label), false)


// Allocate the series of REBVALs inspected by a function when executed (the
// values behind ARG(name), REF(name), D_ARG(3),  etc.)
//
// This only allocates space for the arguments, it does not initialize.
// Eval_Core initializes as it goes, and updates f->param so the GC knows how
// far it has gotten so as not to see garbage.  APPLY has different handling
// when it has to build the frame for the user to write to before running;
// so Eval_Core only checks the arguments, and does not fulfill them.
//
// If the function is a specialization, then the parameter list of that
// specialization will have *fewer* parameters than the full function would.
// For this reason we push the arguments for the "underlying" function.
// Yet if there are specialized values, they must be filled in from the
// exemplar frame.
//
// Rather than "dig" through layers of functions to find the underlying
// function or the specialization's exemplar frame, those properties are
// cached during the creation process.
//
inline static void Push_Action(
    REBFRM *f,
    REBACT *act,
    REBNOD *binding
){
    assert(NOT_EVAL_FLAG(f, FULFILL_ONLY));
    assert(NOT_EVAL_FLAG(f, RUNNING_ENFIX));

    f->param = ACT_PARAMS_HEAD(act); // Specializations hide some params...
    REBLEN num_args = ACT_NUM_PARAMS(act); // ...so see REB_TS_HIDDEN

    // !!! Note: Should pick "smart" size when allocating varlist storage due
    // to potential reuse--but use exact size for *this* action, for now.
    //
    REBSER *s;
    if (not f->varlist) { // usually means first action call in the REBFRM
        s = Alloc_Series_Node(
            SERIES_MASK_VARLIST
                | SERIES_FLAG_STACK_LIFETIME
                | SERIES_FLAG_FIXED_SIZE // FRAME!s don't expand ATM
        );
        s->info = Endlike_Header(
            FLAG_WIDE_BYTE_OR_0(0) // signals array, also implicit terminator
                | FLAG_LEN_BYTE_OR_255(255) // signals dynamic
        );
        INIT_LINK_KEYSOURCE(s, NOD(f)); // maps varlist back to f
        MISC_META_NODE(s) = nullptr; // GC will sees this
        f->varlist = ARR(s);
    }
    else {
        s = SER(f->varlist);
        if (s->content.dynamic.rest >= num_args + 1 + 1) // +roovar, +end
            goto sufficient_allocation;

        //assert(SER_BIAS(s) == 0);
        Free_Unbiased_Series_Data(
            s->content.dynamic.data,
            SER_TOTAL(s)
        );
    }

    if (not Did_Series_Data_Alloc(s, num_args + 1 + 1)) // +rootvar, +end
        fail ("Out of memory in Push_Action()");

    f->rootvar = cast(REBVAL*, s->content.dynamic.data);
    f->rootvar->header.bits =
        NODE_FLAG_NODE
            | NODE_FLAG_CELL
            | NODE_FLAG_STACK
            | CELL_FLAG_PROTECTED  // payload/binding tweaked, but not by user
            | CELL_MASK_CONTEXT
            | FLAG_KIND_BYTE(REB_FRAME)
            | FLAG_MIRROR_BYTE(REB_FRAME);
    TRACK_CELL_IF_DEBUG(f->rootvar, __FILE__, __LINE__);
    INIT_VAL_CONTEXT_VARLIST(f->rootvar, f->varlist);

  sufficient_allocation:

    INIT_VAL_CONTEXT_PHASE(f->rootvar, act);  // FRM_PHASE() (can be dummy)
    EXTRA(Binding, f->rootvar).node = binding; // FRM_BINDING()

    s->content.dynamic.used = num_args + 1;
    RELVAL *tail = ARR_TAIL(f->varlist);
    tail->header.bits = NODE_FLAG_STACK
        | FLAG_KIND_BYTE(REB_0)
        | FLAG_MIRROR_BYTE(REB_0);
    TRACK_CELL_IF_DEBUG(tail, __FILE__, __LINE__);

    // Current invariant for all arrays (including fixed size), last cell in
    // the allocation is an end.
    RELVAL *ultimate = ARR_AT(f->varlist, s->content.dynamic.rest - 1);
    ultimate->header = Endlike_Header(0); // unreadable
    TRACK_CELL_IF_DEBUG(ultimate, __FILE__, __LINE__);

  #if !defined(NDEBUG)
    RELVAL *prep = ultimate - 1;
    for (; prep > tail; --prep) {
        prep->header.bits =
            FLAG_KIND_BYTE(REB_T_TRASH)
            | FLAG_MIRROR_BYTE(REB_T_TRASH); // unreadable
        TRACK_CELL_IF_DEBUG(prep, __FILE__, __LINE__);
    }
  #endif

    f->arg = f->rootvar + 1;

    // Each layer of specialization of a function can only add specializations
    // of arguments which have not been specialized already.  For efficiency,
    // the act of specialization merges all the underlying layers of
    // specialization together.  This means only the outermost specialization
    // is needed to fill the specialized slots contributed by later phases.
    //
    // f->special here will either equal f->param (to indicate normal argument
    // fulfillment) or the head of the "exemplar".  To speed this up, the
    // absence of a cached exemplar just means that the "specialty" holds the
    // paramlist... this means no conditional code is needed here.
    //
    f->special = ACT_SPECIALTY_HEAD(act);

    assert(NOT_SERIES_FLAG(f->varlist, MANAGED));
    assert(NOT_SERIES_INFO(f->varlist, INACCESSIBLE));
}


inline static void Drop_Action(REBFRM *f) {
    assert(NOT_SERIES_FLAG(f->varlist, VARLIST_FRAME_FAILED));

    assert(
        not f->opt_label
        or GET_SERIES_FLAG(f->opt_label, IS_STRING)
    );

    if (NOT_EVAL_FLAG(f, FULFILLING_ARG))
        CLEAR_FEED_FLAG(f->feed, BARRIER_HIT);

    CLEAR_EVAL_FLAG(f, RUNNING_ENFIX);
    CLEAR_EVAL_FLAG(f, FULFILL_ONLY);
    CLEAR_EVAL_FLAG(f, REQUOTE_NULL);

    CLEAR_EVAL_FLAG(f, DISPATCHER_CATCHES);
    CLEAR_EVAL_FLAG(f, DELEGATE_CONTROL);

    assert(
        GET_SERIES_INFO(f->varlist, INACCESSIBLE)
        or LINK_KEYSOURCE(f->varlist) == NOD(f)
    );

    if (GET_SERIES_INFO(f->varlist, INACCESSIBLE)) {
        //
        // If something like Encloser_Dispatcher() runs, it might steal the
        // variables from a context to give them to the user, leaving behind
        // a non-dynamic node.  Pretty much all the bits in the node are
        // therefore useless.  It served a purpose by being non-null during
        // the call, however, up to this moment.
        //
        if (GET_SERIES_FLAG(f->varlist, MANAGED))
            f->varlist = nullptr; // references exist, let a new one alloc
        else {
            // This node could be reused vs. calling Make_Node() on the next
            // action invocation...but easier for the moment to let it go.
            //
            Free_Node(SER_POOL, NOD(f->varlist));
            f->varlist = nullptr;
        }
    }
    else if (GET_SERIES_FLAG(f->varlist, MANAGED)) {
        //
        // Varlist wound up getting referenced in a cell that will outlive
        // this Drop_Action().
        //
        // !!! The new concept is to let frames survive indefinitely in this
        // case.  This is in order to not let JavaScript have the upper hand
        // in "closure"-like scenarios.  See:
        //
        // "What Happens To Function Args/Locals When The Call Ends"
        // https://forum.rebol.info/t/234
        //
        // Previously this said:
        //
        // "The pointer needed to stay working up until now, but the args
        // memory won't be available.  But since we know there are outstanding
        // references to the varlist, we need to convert it into a "stub"
        // that's enough to avoid crashes.
        //
        // ...but we don't free the memory for the args, we just hide it from
        // the stub and get it ready for potential reuse by the next action
        // call.  That's done by making an adjusted copy of the stub, which
        // steals its dynamic memory (by setting the stub not HAS_DYNAMIC)."
        //
      #if 0
        f->varlist = CTX_VARLIST(
            Steal_Context_Vars(
                CTX(f->varlist),
                NOD(f->original) // degrade keysource from f
            )
        );
        assert(NOT_SERIES_FLAG(f->varlist, MANAGED));
        INIT_LINK_KEYSOURCE(f->varlist, NOD(f));
      #endif

        INIT_LINK_KEYSOURCE(f->varlist, NOD(f->original));
        f->varlist = nullptr;
    }
    else {
        // We can reuse the varlist and its data allocation, which may be
        // big enough for ensuing calls.  
        //
        // But no series bits we didn't set should be set...and right now,
        // only Enter_Native() sets HOLD.  Clear that.  Also, it's possible
        // for a "telegraphed" no lookahead bit used by an invisible to be
        // left on, so clear it too.
        //
        CLEAR_SERIES_INFO(f->varlist, HOLD);
        CLEAR_SERIES_INFO(f->varlist, TELEGRAPH_NO_LOOKAHEAD);

        assert(0 == (SER(f->varlist)->info.bits & ~( // <- note bitwise not
            SERIES_INFO_0_IS_TRUE // parallels NODE_FLAG_NODE
            | FLAG_WIDE_BYTE_OR_0(0) // don't mask out wide (0 for arrays))
            | FLAG_LEN_BYTE_OR_255(255) // mask out non-dynamic-len (dynamic)
        )));
    }

  #if !defined(NDEBUG)
    if (f->varlist) {
        assert(NOT_SERIES_INFO(f->varlist, INACCESSIBLE));
        assert(NOT_SERIES_FLAG(f->varlist, MANAGED));

        RELVAL *rootvar = ARR_HEAD(f->varlist);
        assert(CTX_VARLIST(VAL_CONTEXT(rootvar)) == f->varlist);
        TRASH_POINTER_IF_DEBUG(PAYLOAD(Any, rootvar).second.node);  // phase
        TRASH_POINTER_IF_DEBUG(EXTRA(Binding, rootvar).node);
    }
  #endif

    f->original = nullptr; // signal an action is no longer running

    TRASH_POINTER_IF_DEBUG(f->opt_label);
  #if defined(DEBUG_FRAME_LABELS)
    TRASH_POINTER_IF_DEBUG(f->label_utf8);
  #endif
}


// Partially-filled function frames that only have some of their arguments
// evaluated cannot be "reified" into the form that can be persistently linked
// as a parent to API handles.  "Dummy frames" exist to look like a fulfilled
// call to a function with no arguments.  This is helpful if you ever try
// to do something like call the libRebol API from the guts of the evaluator.
//
inline static void Push_Dummy_Frame(REBFRM *f) {
    Push_Frame(nullptr, f);

    REBSTR *opt_label = NULL;

    Push_Action(f, PG_Dummy_Action, UNBOUND);
    Begin_Prefix_Action(f, opt_label);
    assert(IS_END(f->arg));
    f->param = END_NODE;  // signal all arguments gathered
    f->arg = m_cast(REBVAL*, END_NODE);
    f->special = END_NODE;
}

inline static void Drop_Dummy_Frame_Unbalanced(REBFRM *f) {
    Drop_Action(f);

    // !!! To abstract how the system deals with exception handling, the
    // rebRescue() routine started being used in lieu of PUSH_TRAP/DROP_TRAP
    // internally to the system.  Some of these system routines accumulate
    // stack state, so Drop_Frame_Unbalanced() must be used.
    //
    Drop_Frame_Unbalanced(f);
}


//=//// ARGUMENT AND PARAMETER ACCESS HELPERS ////=///////////////////////////
//
// These accessors are what is behind the INCLUDE_PARAMS_OF_XXX macros that
// are used in natives.  They capture the implicit Reb_Frame* passed to every
// REBNATIVE ('frame_') and read the information out cleanly, like this:
//
//     PARAM(1, foo);
//     PARAM(2, bar);
//
//     if (IS_INTEGER(ARG(foo)) and REF(bar)) { ... }
//
// The PARAM macro uses token pasting to name the indexes they are declaring
// `p_name` instead of just `name`.  This prevents collisions with C/C++
// identifiers, so PARAM(case) and PARAM(new) would make `p_case` and `p_new`
// instead of just `case` and `new` as the variable names.
//
// ARG() gives a mutable pointer to the argument's cell.  REF() is typically
// used with refinements, and gives a const reference where NULLED cells are
// turned into C nullptr.  This can be helpful for any argument that is
// optional, as the libRebol API does not accept NULLED cells directly.
//
// By contract, Rebol functions are allowed to mutate their arguments and
// refinements just as if they were locals...guaranteeing only their return
// result as externally visible.  Hence the ARG() cells provide a GC-safe
// slot for natives to hold values once they are no longer needed.
//
// It is also possible to get the typeset-with-symbol for a particular
// parameter or refinement, e.g. with `PAR(foo)` or `PAR(bar)`.

#define PARAM(n,name) \
    static const int p_##name##_ = n

#define ARG(name) \
    FRM_ARG(frame_, (p_##name##_))

#define PAR(name) \
    ACT_PARAM(FRM_PHASE(frame_), (p_##name##_))  // a REB_P_XXX pseudovalue

#define REF(name) \
    NULLIFY_NULLED(ARG(name))


// Quick access functions from natives (or compatible functions that name a
// Reb_Frame pointer `frame_`) to get some of the common public fields.
//
#define D_FRAME     frame_
#define D_OUT       FRM_OUT(frame_)         // GC-safe slot for output value
#define D_SPARE     FRM_SPARE(frame_)       // scratch GC-safe cell
#define D_THROWING  Is_Throwing(frame_)     // check continuation throw state

// !!! Numbered arguments got more complicated with the idea of moving the
// definitional returns into the first slot (if applicable).  This makes it
// more important to use the named ARG() and REF() macros.  As a stopgap
// measure, we just sense whether the phase has a return or not.
//
inline static REBVAL *D_ARG_Core(REBFRM *f, REBLEN n) {  // 1 for first arg
    return GET_ACTION_FLAG(FRM_PHASE(f), HAS_RETURN)
        ? FRM_ARG(f, n + 1)
        : FRM_ARG(f, n);
}
#define D_ARG(n) \
    D_ARG_Core(frame_, (n))

// Convenience routine for returning a value which is *not* located in D_OUT.
// (If at all possible, it's better to build values directly into D_OUT and
// then return the D_OUT pointer...this is the fastest form of returning.)
//
#define RETURN(v) \
    return Move_Value(D_OUT, (v))


// Conveniences for returning a continuation.  The concept is that when a
// R_CONTINUATION comes back via the C `return` for a native, that native's
// C stack variables are all gone.  But the heap-allocated Rebol frame stays
// intact and in the Rebol stack trace.  It will be resumed when the
// continuation finishes, and its f->out pointer will contain whatever was
// produced.
//
inline static REB_R Init_Continuation_With_Core(
    REBFRM *f,
    REBFLGS flags,  // EVAL_FLAG_(DELEGATE_CONTROL/DISPATCHER_CATCHES)
    const RELVAL *branch,
    REBSPC *branch_specifier,
    const REBVAL *with
){
    f->flags.bits |= flags;
    assert(branch != f->out and with != f->out);

    // !!! This code came from Do_Branch_XXX_Throws() which was not
    // continuation-based, and hence holds some reusable logic for
    // branch types that do not require evaluation...like REB_QUOTED.
    // It's the easiest way to reuse the logic for the time being,
    // though some performance gain could be achieved for instance
    // if it were reused in a way that allowed something like IF to
    // not bother running again (like if `CONTINUE()` would do a plain
    // return in those cases).  But more complex scenarios may have
    // broken control flow if such shortcuts were taken.  Review.

  recontinue:

    switch (VAL_TYPE(branch)) {
      case REB_QUOTED:
        if (not (flags & EVAL_FLAG_DELEGATE_CONTROL))
            assert(!"Temporarily no non-delegated quoted branches");
        Unquotify(Derelativize(f->out, branch, branch_specifier), 1);
        return f->out;

      case REB_HANDLE: {  // temporarily means REBFRM*, chain to stack
        REBFRM *subframe = VAL_HANDLE_POINTER(REBFRM, branch);
        assert(GET_EVAL_FLAG(subframe, CONTINUATION));
        assert(GET_EVAL_FLAG(subframe, DETACH_DONT_DROP));
        assert(subframe->prior == nullptr);
        subframe->dsp_orig = DSP;  // may be accruing state
        subframe->prior = f;
        TG_Top_Frame = subframe;
        return R_CONTINUATION; }

      case REB_BLOCK: {
        //
        // What we want to do is Do_Any_Array_At_Throws.  This means
        // we must initialize to void (in case all invisibles) and
        // we must clear off the stale flag at the end.
        //
        DECLARE_FRAME_AT_CORE (
            blockframe,
            branch,
            branch_specifier,
            EVAL_MASK_DEFAULT
                | EVAL_FLAG_CONTINUATION
                | EVAL_FLAG_TO_END
        );

        Init_Void(f->out);  // in case all invisibles, as usual
        Push_Frame(f->out, blockframe);
        break; }

      case REB_ACTION: {
        REBACT *action = VAL_ACTION(branch);
        REBNOD *binding = VAL_BINDING(branch);

        // CONTINUE_WITH when used with a 0-arity function will omit
        // the WITH parameter.  If an error is desired, that must be
        // done at a higher level (e.g. see DO of ACTION!)
        //
        REBFED *subfeed = Alloc_Feed();
        Prep_Array_Feed(subfeed,
            First_Unspecialized_Param(action) == nullptr
                ? nullptr  // 0-arity throws away `with`
                : (IS_END(with) ? nullptr : with),
            EMPTY_ARRAY,  // unused (just leveraging `with` preload)
            0,
            SPECIFIED,
            FEED_MASK_DEFAULT
        );

        DECLARE_FRAME (
            subframe,
            subfeed,
            EVAL_MASK_DEFAULT
                | EVAL_FLAG_CONTINUATION
                | EVAL_FLAG_ALLOCATED_FEED
        );

        Init_Void(f->out);
        SET_CELL_FLAG(f->out, OUT_MARKED_STALE);
        Push_Frame(f->out, subframe);
        Push_Action(subframe, action, binding);
        REBSTR *opt_label = nullptr;
        Begin_Prefix_Action(subframe, opt_label);
        break; }

      case REB_BLANK:
        assert(!"Temporarily no blank branches");
        Init_Nulled(f->out);
        break;

      case REB_SYM_WORD:
      case REB_SYM_PATH: {
        assert(!"Temporarily no SYM_WORD or SYM_PATH branches");
        //
        // !!! SYM-WORD! and SYM-PATH! were considered as speculative
        // abbreviations for things like:
        //
        //     x: 10
        //     >> if true @x
        //     == 10
        //
        // One benefit of this over `if true [:x]` would be efficiency
        // in both representation (lower cost for not needing an
        // array at source level) and execution (lower cost for not
        // needing a frame to be nested and indexed across).  Another
        // would be that perhaps it could error on VOID! instead of
        // tolerating it, and treating functions by value.
        //
        REBSTR *name;
        const bool push_refinements = false;
        bool threw = Get_If_Word_Or_Path_Throws(
            f->out,
            &name,
            branch,
            branch_specifier,
            push_refinements
        );
        if (threw)
            return R_THROWN;

        if (IS_VOID(f->out))  // need `[:x]` if it's void (unset)
            fail (Error_Need_Non_Void_Core(branch, branch_specifier));
        break; }

      case REB_SYM_GROUP: {
        //
        // !!! Because of soft-quoting of branches, it's required to
        // put anything that evaluates to a branch in a GROUP!.  The
        // downside of this is that in order to seem consistent with
        // expectations, code in that group runs regardless of whether
        // the branch runs, e.g.
        //
        //    >> either 1 (print "both" [2 + 3]) (print "run" [4 + 5])
        //    both
        //    run
        //    == 5
        //
        // An experimental idea was that a SYM-GROUP! could be used
        // to generate a branch, but only if needed.  That falls in
        // line with the expectation of what non-soft-quoting actions
        // could do with their arguments, since SYM-GROUP! is not
        // evaluative:
        //
        //    >> either 1 @(print "one" [2 + 3]) @(print "run" [4 + 5])
        //    one
        //    == 5
        //
        // It's not clear if this idea is important or not, but it
        // was a pre-continuation experiment that was preserved.
        //
/*        bool threw = Do_Any_Array_At_Throws(f->out, branch, branch_specifier);
        if (threw)
            return R_THROWN; */

        // !!! This feature will currently corrupt the caller's
        // RETURN cell, because there's no other place to put the
        // evaluative product.  Corrupting the spare could mess up
        // the state of the continuations.  It's a hack that is fine
        // for natives (that don't usually need their return anyway)
        //
        Move_Value(FRM_ARG(f, 1), f->out);
        branch = FRM_ARG(f, 1);
        goto recontinue; }  // Note: Could infinite loop if SYM-GROUP!

      case REB_FRAME: {
        REBCTX *c = VAL_CONTEXT(branch);  // check accessible
        REBACT *phase = VAL_PHASE(branch);

        assert(not CTX_FRAME_IF_ON_STACK(c));

        // To DO a FRAME! will "steal" its data.  If a user wishes to
        // use a frame multiple times, they must say DO COPY FRAME, so
        // that the data is stolen from the copy.  This allows for
        // efficient reuse of the context's memory in the cases where
        // a copy isn't needed.

        DECLARE_END_FRAME (
            subframe,
            EVAL_MASK_DEFAULT
                | EVAL_FLAG_FULLY_SPECIALIZED
                | EVAL_FLAG_CONTINUATION
        );

        assert(CTX_KEYS_HEAD(c) == ACT_PARAMS_HEAD(phase));
        subframe->param = CTX_KEYS_HEAD(c);
        REBCTX *stolen = Steal_Context_Vars(c, NOD(phase));

        // v-- This changes CTX_KEYS_HEAD()
        //
        INIT_LINK_KEYSOURCE(stolen, NOD(subframe));

        // Its data stolen, the context's node should now be GC'd when
        // references in other FRAME! value cells have all gone away.
        //
        assert(GET_SERIES_FLAG(c, MANAGED));
        assert(GET_SERIES_INFO(c, INACCESSIBLE));

        Push_Frame_No_Varlist(f->out, subframe);
        subframe->varlist = CTX_VARLIST(stolen);
        subframe->rootvar = CTX_ARCHETYPE(stolen);
        subframe->arg = subframe->rootvar + 1;
        // subframe->param set above
        subframe->special = subframe->arg;

        // !!! Original code said "Should archetype match?"
        //
        assert(FRM_PHASE(subframe) == phase);
        FRM_BINDING(subframe) = VAL_BINDING(branch);

        REBSTR *opt_label = nullptr;
        Begin_Prefix_Action(subframe, opt_label);
        break; }

      default:
        assert(!"Bad branch type");
        fail ("Bad branch type");  // !!! should be an assert or panic
    }

    return R_CONTINUATION;
}

#define Init_Continuation_With(f,flags,branch,with) \
    Init_Continuation_With_Core((f), (flags), (branch), SPECIFIED, (with))

#define DELEGATE(branch) \
    return Init_Continuation_With( \
        frame_, EVAL_FLAG_DELEGATE_CONTROL, (branch), END_NODE) 

#define DELEGATE_WITH(branch,with) \
    return Init_Continuation_With( \
        frame_, EVAL_FLAG_DELEGATE_CONTROL, (branch), (with))

#define CONTINUE(branch) \
    return Init_Continuation_With(frame_, 0, (branch), END_NODE)

#define CONTINUE_WITH(branch,with) \
    return Init_Continuation_With(frame_, 0, (branch), (with))

#define CONTINUE_CATCHABLE(branch) \
    return Init_Continuation_With( \
        frame_, EVAL_FLAG_DISPATCHER_CATCHES, (branch), END_NODE)


// Common behavior shared by dispatchers which execute on BLOCK!s of code.
// Assumes the code to be run is in the first details slot, and is relative
// to the varlist of the frame.
//
inline static REB_R Interpreted_Continuation_Core(
    REBFRM *f,
    REBFLGS flags
){
    REBACT *phase = FRM_PHASE(f);
    REBARR *details = ACT_DETAILS(phase);
    RELVAL *body = ARR_HEAD(details);  // usually CONST (doesn't have to be)
    assert(IS_BLOCK(body) and IS_RELATIVE(body) and VAL_INDEX(body) == 0);

    if (GET_ACTION_FLAG(phase, HAS_RETURN)) {
        assert(VAL_PARAM_SYM(ACT_PARAMS_HEAD(phase)) == SYM_RETURN);
        REBVAL *cell = FRM_ARG(f, 1);
        Move_Value(cell, NAT_VALUE(return));
        INIT_BINDING(cell, f->varlist);
        SET_CELL_FLAG(cell, ARG_MARKED_CHECKED);  // necessary?
    }

    // The function body contains relativized words, that point to the
    // paramlist but do not have an instance of an action to line them up
    // with.  We use the frame (identified by varlist) as the "specifier".
    //
    return Init_Continuation_With_Core(
        f,
        flags,
        body,
        SPC(f->varlist),
        END_NODE
    );
}

#define Interpreted_Continuation(f) \
    Interpreted_Continuation_Core((f), 0)

#define Interpreted_Delegation(f) \
    Interpreted_Continuation_Core((f), EVAL_FLAG_DELEGATE_CONTROL)


// The native entry prelude makes sure that once native code starts running,
// then the frame's stub is flagged to indicate access via a FRAME! should
// not have write access to variables.  That could cause crashes, as raw C
// code is not insulated against having bit patterns for types in cells that
// aren't expected.
//
// !!! Debug injection of bad types into usermode code may cause havoc as
// well, and should be considered a security/permissions issue.  It just won't
// (or shouldn't) crash the evaluator itself.
//
// This is automatically injected by the INCLUDE_PARAMS_OF_XXX macros.  The
// reason this is done with code inlined into the native itself instead of
// based on an IS_NATIVE() test is to avoid the cost of the testing--which
// is itself a bit dodgy to tell a priori if a dispatcher is native or not.
// This way there is no test and only natives pay the cost of flag setting.
//
inline static void Enter_Native(REBFRM *f) {
    SET_SERIES_INFO(f->varlist, HOLD); // may or may not be managed
}


// Shared code for type checking the return result.  It's used by the
// Returner_Dispatcher(), but custom dispatchers use it to (e.g. JS-NATIVE)
//
inline static void FAIL_IF_BAD_RETURN_TYPE(REBFRM *f) {
    REBACT *phase = FRM_PHASE(f);
    REBVAL *typeset = ACT_PARAMS_HEAD(phase);
    assert(VAL_PARAM_SYM(typeset) == SYM_RETURN);

    // Typeset bits for locals in frames are usually ignored, but the RETURN:
    // local uses them for the return types of a function.
    //
    if (not Typecheck_Including_Quoteds(typeset, f->out))
        fail (Error_Bad_Return_Type(f, VAL_TYPE(f->out)));
}


inline static void Expire_Out_Cell_Unless_Invisible(REBFRM *f) {
    REBACT *phase = FRM_PHASE(f);
    if (GET_ACTION_FLAG(phase, IS_INVISIBLE)) {
        if (NOT_ACTION_FLAG(f->original, IS_INVISIBLE))
            fail ("All invisible action phases must be invisible");
        return;
    }

    if (GET_ACTION_FLAG(f->original, IS_INVISIBLE))
        return;

  #ifdef DEBUG_UNREADABLE_BLANKS
    //
    // The f->out slot should be initialized well enough for GC safety.
    // But in the debug build, if we're not running an invisible function
    // set it to END here, to make sure the non-invisible function writes
    // *something* to the output.
    //
    // END has an advantage because recycle/torture will catch cases of
    // evaluating into movable memory.  But if END is always set, natives
    // might *assume* it.  Fuzz it with unreadable blanks.
    //
    // !!! Should natives be able to count on f->out being END?  This was
    // at one time the case, but this code was in one instance.
    //
    if (NOT_ACTION_FLAG(FRM_PHASE(f), IS_INVISIBLE)) {
        if (SPORADICALLY(2))
            Init_Unreadable_Blank(f->out);
        else
            SET_END(f->out);
        SET_CELL_FLAG(f->out, OUT_MARKED_STALE);
    }
  #endif
}
