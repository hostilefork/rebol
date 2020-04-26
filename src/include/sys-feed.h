//
//  File: %sys-feed.h
//  Summary: {Accessors and Argument Pushers/Poppers for Function Call Frames}
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2018-2019 Revolt Open Source Contributors
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
// A "Feed" represents an abstract source of Rebol values, which only offers
// a guarantee of being able to have two sequential values in the feed as
// having valid pointers at one time.  The main pointer is the feed's value
// (feed->value), and to be able to have another pointer to the previous
// value one must request a "lookback" at the time of advancing the feed.
//
// One reason for the feed's strict nature is that it offers an interface not
// just to Rebol BLOCK!s and other arrays, but also to variadic lists such
// as C's va_list...in a system which also allows the mixure of portions of
// UTF-8 string source text.  C's va_list does not retain a memory of the
// past, so once va_arg() is called it forgets the previous value...and
// since values may also be fabricated from text it can get complicated.
//
// Another reason for the strictness is to help rein in the evaluator design
// to keep it within a certain boundary of complexity.


#define FEED_FLAG_0_IS_TRUE \
    FLAG_LEFT_BIT(0)
STATIC_ASSERT(FEED_FLAG_0_IS_TRUE == NODE_FLAG_NODE);

#define FEED_FLAG_1_IS_FALSE \
    FLAG_LEFT_BIT(1)
STATIC_ASSERT(FEED_FLAG_1_IS_FALSE == NODE_FLAG_FREE);


// SERIES_INFO_HOLD is used to make a temporary read-only lock of an array
// while it is running.  Since the same array can wind up on multiple levels
// of the stack (e.g. recursive functions), the source must be connected with
// a bit saying whether it was the level that protected it, so it can know to
// release the hold when it's done.
//
#define FEED_FLAG_TOOK_HOLD \
    FLAG_LEFT_BIT(2)


// Infix functions may (depending on the #tight or non-tight parameter
// acquisition modes) want to suppress further infix lookahead while getting
// a function argument.  This precedent was started in R3-Alpha, where with
// `1 + 2 * 3` it didn't want infix `+` to "look ahead" past the 2 to see the
// infix `*` when gathering its argument, that was saved until the `1 + 2`
// finished its processing.
//
#define FEED_FLAG_NO_LOOKAHEAD \
    FLAG_LEFT_BIT(3)


// Defer notes when there is a pending enfix operation that was seen while an
// argument was being gathered, that decided not to run yet.  It will run only
// if it turns out that was the last argument that was being gathered...
// otherwise it will error.
//
//    if 1 [2] then [3]     ; legal
//    if 1 then [2] [3]     ; **error**
//    if (1 then [2]) [3]   ; legal, arguments weren't being gathered
//
// This flag is marked on a parent frame by the argument fulfillment the
// first time it sees a left-deferring operation like a THEN or ELSE, and is
// used to decide whether to report an error or not.
//
// (At one point, mechanics were added to make the second case not an
// error.  However, this gave the evaluator complex properties of re-entry
// that made its behavior harder to characterize.  This means that only a
// flag is needed, vs complex marking of a parameter to re-enter eval with.)
//
#define FEED_FLAG_DEFERRING_ENFIX \
    FLAG_LEFT_BIT(4)


// Evaluation of arguments can wind up seeing a barrier and "consuming" it.
// This is true of a BAR!, but also GROUP!s which have no effective content:
//
//    >> 1 + (comment "vaporizes, but disrupts like a BAR! would") 2
//    ** Script Error: + is missing its value2 argument
//
// But the evaluation will advance the frame.  So if a function has more than
// one argument it has to remember that one of its arguments saw a "barrier",
// otherwise it would receive an end signal on an earlier argument yet then
// get a later argument fulfilled.
//
#define FEED_FLAG_BARRIER_HIT \
    FLAG_LEFT_BIT(5)


// When processing something like enfix, the output cell of a frame is the
// place to look for the "next" value.  This setting has to be managed
// carefully in recursion, because the recursion must preserve the same
// notion of what the "out" cell is for the frame.
//
// !!! It may be better to think of this as an EVAL_FLAG, but those per-frame
// flags are running short.  Once this setting is on a feed, it has to be
// consumed or there will be an error.
//
#define FEED_FLAG_NEXT_ARG_FROM_OUT \
    FLAG_LEFT_BIT(6)


STATIC_ASSERT(FLAG_LEFT_BIT(7) == NODE_FLAG_CELL);  // !!! use it or not?


#define FEED_MASK_DEFAULT \
    FEED_FLAG_0_IS_TRUE


//=//// BITS 8...15 ARE THE QUOTING LEVEL /////////////////////////////////=//

// There was significant deliberation over what the following code should do:
//
//     REBVAL *word = rebValue("'print");
//     REBVAL *type = rebValue("type of", word);
//
// If the WORD! is simply spliced into the code and run, then that will be
// an error.  It would be as if you had written:
//
//     do compose [type of (word)]
//
// It may seem to be more desirable to pretend you had fetched word from a
// variable, as if the code had been Rebol.  The illusion could be given by
// automatically splicing quotes, but doing this without being asked creates
// other negative side effects:
//
//     REBVAL *x = rebInteger(10);
//     REBVAL *y = rebInteger(20);
//     REBVAL *coordinate = rebValue("[", x, y, "]");
//
// You don't want to wind up with `['10 '20]` in that block.  So automatic
// splicing with quotes is fraught with problems.  Still it might be useful
// sometimes, so it is exposed via `rebValueQ()` and other `rebXxxQ()`.
//
// These facilities are generalized so that one may add and drop quoting from
// splices on a feed via ranges, countering any additions via rebQ() with a
// corresponding rebU().  This is kept within reason at up to 255 levels
// in a byte, and that byte is in the feed flags in the second byte (where
// it is least likely to be needed to line up with cell bits etc.)  Being in
// the flags means it can be initialized with them in one assignment if
// it does not change.
//

#define FLAG_QUOTING_BYTE(quoting) \
    FLAG_SECOND_BYTE(quoting)

#define QUOTING_BYTE(feed) \
    SECOND_BYTE((feed)->flags.bits)

#define mutable_QUOTING_BYTE(feed) \
    mutable_SECOND_BYTE((feed)->flags.bits)


// The user is able to flip the constness flag explicitly with the CONST and
// MUTABLE functions explicitly.  However, if a feed has FEED_FLAG_CONST,
// the system imposes it's own constness as part of the "wave of evaluation"
// it does.  While this wave starts out initially with frames demanding const
// marking, if it ever gets flipped, it will have to encounter an explicit
// CONST marking on a value before getting flipped back.
//
// (This behavior is designed to permit switching into a "mode" that is
//
#define FEED_FLAG_CONST \
    FLAG_LEFT_BIT(22)
STATIC_ASSERT(FEED_FLAG_CONST == CELL_FLAG_CONST);


#if !defined __cplusplus
    #define FEED(f) f
#else
    #define FEED(f) static_cast<REBFED*>(f)
#endif

#define SET_FEED_FLAG(f,name) \
    (FEED(f)->flags.bits |= FEED_FLAG_##name)

#define GET_FEED_FLAG(f,name) \
    ((FEED(f)->flags.bits & FEED_FLAG_##name) != 0)

#define CLEAR_FEED_FLAG(f,name) \
    (FEED(f)->flags.bits &= ~FEED_FLAG_##name)

#define NOT_FEED_FLAG(f,name) \
    ((FEED(f)->flags.bits & FEED_FLAG_##name) == 0)



// Ordinary Rebol internals deal with REBVAL* that are resident in arrays.
// But a va_list can contain UTF-8 string components or special instructions
// that are other Detect_Revolt_Pointer() types.  Anyone who wants to set or
// preload a frame's state for a va_list has to do this detection, so this
// code has to be factored out to just take a void* (because a C va_list
// cannot have its first parameter in the variadic, va_list* is insufficient)
//
inline static const RELVAL *Detect_Feed_Pointer_Maybe_Fetch(
    REBFED *feed,
    const void *p,
    bool preserve
){
    const RELVAL *lookback;

    if (not preserve)
        lookback = nullptr;
    else {
        assert(READABLE(feed->value, __FILE__, __LINE__));  // ensure cell

        if (GET_CELL_FLAG(&feed->fetched, FETCHED_MARKED_TEMPORARY)) {
            //
            // f->value was transient and hence constructed into f->fetched.
            // We may overwrite it below for this fetch.  So save the old one
            // into f->lookback, where it will be safe until the next fetch.
            //
            assert(feed->value == &feed->fetched);
            lookback = Move_Value(&feed->lookback, KNOWN(&feed->fetched));
        }
        else {
            // pointer they had should be stable, GC-safe

            lookback = feed->value;
        }
    }

  detect_again:;

    TRASH_POINTER_IF_DEBUG(feed->value);  // should be assigned below

    if (not p) {  // libRevolt's null/<opt> (IS_NULLED illegal in CELL case)

        if (QUOTING_BYTE(feed) == 0)
            panic ("Cannot directly splice nulls...use rebQ(), rebXxxQ()");

        // !!! We could make a global QUOTED_NULLED_VALUE with a stable
        // pointer and not have to use fetched or FETCHED_MARKED_TEMPORARY.
        //
        feed->array = nullptr;
        Quotify(Init_Nulled(&feed->fetched), 1);
        SET_CELL_FLAG(&feed->fetched, FETCHED_MARKED_TEMPORARY);
        feed->value = &feed->fetched;

    } else switch (Detect_Revolt_Pointer(p)) {

      case DETECTED_AS_UTF8: {
        REBDSP dsp_orig = DSP;

        // !!! Current hack is to just allow one binder to be passed in for
        // use binding any newly loaded portions (spliced ones are left with
        // their bindings, though there may be special "binding instructions"
        // or otherwise, that get added).
        //
        feed->context = Get_Context_From_Stack();
        feed->lib = (feed->context != Lib_Context) ? Lib_Context : nullptr;

        struct Reb_Binder binder;
        Init_Interning_Binder(&binder, feed->context);
        feed->binder = &binder;

        feed->specifier = SPECIFIED;

        SCAN_LEVEL level;
        SCAN_STATE ss;
        const REBLIN start_line = 1;
        Init_Va_Scan_Level_Core(
            &level,
            &ss,
            Intern("sys-do.h"),
            start_line,
            cast(const REBYTE*, p),
            feed
        );

        REBVAL *error = rebRescue(cast(REBDNG*, &Scan_To_Stack), &level);
        Shutdown_Interning_Binder(&binder, feed->context);

        if (error) {
            REBCTX *error_ctx = VAL_CONTEXT(error);
            rebRelease(error);
            fail (error_ctx);
        }

        if (DSP == dsp_orig) {
            //
            // This happens when somone says rebValue(..., "", ...) or similar,
            // and gets an empty array from a string scan.  It's not legal
            // to put an END in f->value, and it's unknown if the variadic
            // feed is actually over so as to put null... so get another
            // value out of the va_list and keep going.
            //
            p = va_arg(*feed->vaptr, const void*);
            goto detect_again;
        }

        // !!! for now, assume scan went to the end; ultimately it would need
        // to pass the feed in as a parameter for partial scans
        //
        feed->vaptr = nullptr;

        REBARR *reified = Pop_Stack_Values(dsp_orig);

        // !!! We really should be able to free this array without managing it
        // when we're done with it, though that can get a bit complicated if
        // there's an error or need to reify into a value.  For now, do the
        // inefficient thing and manage it.
        //
        // !!! Scans that produce only one value (which are likely very
        // common) can go into feed->fetched and not make an array at all.
        //
        Manage_Array(reified);

        feed->value = ARR_HEAD(reified);
        feed->pending = feed->value + 1;  // may be END
        feed->array = reified;
        feed->index = 1;

        CLEAR_CELL_FLAG(&feed->fetched, FETCHED_MARKED_TEMPORARY);
        break; }

      case DETECTED_AS_SERIES: {  // e.g. rebQ, rebU, or a rebR() handle
        REBARR *inst1 = ARR(m_cast(void*, p));

        // As we feed forward, we're supposed to be freeing this--it is not
        // managed -and- it's not manuals tracked, it is only held alive by
        // the va_list()'s plan to visit it.  A fail() here won't auto free
        // it *because it is this traversal code which is supposed to free*.
        //
        // !!! Actually, THIS CODE CAN'T FAIL.  :-/  It is part of the
        // implementation of fail's cleanup itself.
        //
        if (GET_ARRAY_FLAG(inst1, INSTRUCTION_ADJUST_QUOTING)) {
            assert(NOT_SERIES_FLAG(inst1, MANAGED));

            if (QUOTING_BYTE(feed) + MISC(inst1).quoting_delta < 0)
                panic ("rebU() can't unquote a feed splicing plain values");

            assert(ARR_LEN(inst1) > 0);
            if (ARR_LEN(inst1) > 1)
                panic ("rebU() of more than one value splice not written");

            REBVAL *single = KNOWN(ARR_SINGLE(inst1));
            Move_Value(&feed->fetched, single);
            Quotify(
                &feed->fetched,
                QUOTING_BYTE(feed) + MISC(inst1).quoting_delta
            );
            SET_CELL_FLAG(&feed->fetched, FETCHED_MARKED_TEMPORARY);
            feed->value = &feed->fetched;

            GC_Kill_Series(SER(inst1));  // not manuals-tracked
        }
        else if (GET_ARRAY_FLAG(inst1, SINGULAR_API_RELEASE)) {
            assert(GET_SERIES_FLAG(inst1, MANAGED));

            // See notes above (duplicate code, fix!) about how we might like
            // to use the as-is value and wait to free until the next cycle
            // vs. putting it in fetched/MARKED_TEMPORARY...but that makes
            // this more convoluted.  Review.

            REBVAL *single = KNOWN(ARR_SINGLE(inst1));
            Move_Value(&feed->fetched, single);
            Quotify(&feed->fetched, QUOTING_BYTE(feed));
            SET_CELL_FLAG(&feed->fetched, FETCHED_MARKED_TEMPORARY);
            feed->value = &feed->fetched;
            rebRelease(cast(const REBVAL*, single));  // *is* the instruction
        }
        else
            panic (inst1);

        break; }

      case DETECTED_AS_CELL: {
        const REBVAL *cell = cast(const REBVAL*, p);
        assert(not IS_RELATIVE(cast(const RELVAL*, cell)));

        feed->array = nullptr;

        if (IS_NULLED(cell))  // API enforces use of C's nullptr (0) for NULL
            assert(!"NULLED cell API leak, see NULLIFY_NULLED() in C source");

        if (QUOTING_BYTE(feed) == 0) {
            feed->value = cell;  // cell can be used as-is
        }
        else {
            // We don't want to corrupt the value itself.  We have to move
            // it into the fetched cell and quote it.
            //
            Quotify(Move_Value(&feed->fetched, cell), QUOTING_BYTE(feed));
            SET_CELL_FLAG(&feed->fetched, FETCHED_MARKED_TEMPORARY);
            feed->value = &feed->fetched;  // note END is detected separately
        }
        break; }

      case DETECTED_AS_END: {  // end of variadic input, so that's it for this
        feed->value = END_NODE;
        TRASH_POINTER_IF_DEBUG(feed->pending);

        // The va_end() is taken care of here, or if there is a throw/fail it
        // is taken care of by Abort_Frame_Core()
        //
        va_end(*feed->vaptr);
        feed->vaptr = nullptr;

        // !!! Error reporting expects there to be an array.  The whole story
        // of errors when there's a va_list is not told very well, and what
        // will have to likely happen is that in debug modes, all va_list
        // are reified from the beginning, else there's not going to be
        // a way to present errors in context.  Fake an empty array for now.
        //
        feed->array = EMPTY_ARRAY;
        feed->index = 0;

        CLEAR_CELL_FLAG(&feed->fetched, FETCHED_MARKED_TEMPORARY);  // needed?
        break; }

      case DETECTED_AS_FREED_SERIES:
      case DETECTED_AS_FREED_CELL:
      default:
        panic (p);
    }

    return lookback;
}


//
// Fetch_Next_In_Feed_Core() (see notes above)
//
// Once a va_list is "fetched", it cannot be "un-fetched".  Hence only one
// unit of fetch is done at a time, into f->value.  f->feed->pending thus
// must hold a signal that data remains in the va_list and it should be
// consulted further.  That signal is an END marker.
//
// More generally, an END marker in f->feed->pending for this routine is a
// signal that the vaptr (if any) should be consulted next.
//
inline static const RELVAL *Fetch_Next_In_Feed_Core(
    REBFED *feed,
    bool preserve
){
  #ifdef DEBUG_EXPIRED_LOOKBACK
    if (feed->stress) {
        TRASH_CELL_IF_DEBUG(feed->stress);
        free(feed->stress);
        feed->stress = nullptr;
    }
  #endif

    // We are changing ->value, and thus by definition any ->gotten value
    // will be invalid.  It might be "wasteful" to always set this to null,
    // especially if it's going to be overwritten with the real fetch...but
    // at a source level, having every call to Fetch_Next_In_Frame have to
    // explicitly set ->gotten to null is overkill.  Could be split into
    // a version that just trashes ->gotten in the debug build vs. null.
    //
    feed->gotten = nullptr;

    const RELVAL *lookback;

    if (NOT_END(feed->pending)) {
        //
        // We assume the ->pending value lives in a source array, and can
        // just be incremented since the array has SERIES_INFO_HOLD while it
        // is being executed hence won't be relocated or modified.  This
        // means the release build doesn't need to call ARR_AT().
        //
        assert(
            feed->array // incrementing plain array of REBVAL[]
            or feed->pending == ARR_AT(feed->array, feed->index)
        );

        lookback = feed->value;  // should have been stable
        feed->value = feed->pending;

        ++feed->pending; // might be becoming an END marker, here
        ++feed->index;
    }
    else if (not feed->vaptr) {
        //
        // The frame was either never variadic, or it was but got spooled into
        // an array by Reify_Va_To_Array_In_Frame().  The first END we hit
        // is the full stop end.

        lookback = feed->value;
        feed->value = END_NODE;
        TRASH_POINTER_IF_DEBUG(feed->pending);

        ++feed->index; // for consistency in index termination state

        if (GET_FEED_FLAG(feed, TOOK_HOLD)) {
            assert(GET_SERIES_INFO(feed->array, HOLD));
            CLEAR_SERIES_INFO(feed->array, HOLD);

            // !!! Future features may allow you to move on to another array.
            // If so, the "hold" bit would need to be reset like this.
            //
            CLEAR_FEED_FLAG(feed, TOOK_HOLD);
        }
    }
    else {
        // A variadic can source arbitrary pointers, which can be detected
        // and handled in different ways.  Notably, a UTF-8 string can be
        // differentiated and loaded.
        //
        const void *p = va_arg(*feed->vaptr, const void*);
        feed->index = TRASHED_INDEX; // avoids warning in release build
        lookback = Detect_Feed_Pointer_Maybe_Fetch(feed, p, preserve);
    }

    assert(
        IS_END(feed->value)
        or feed->value == &feed->fetched
        or NOT_CELL_FLAG(&feed->fetched, FETCHED_MARKED_TEMPORARY)
    );

  #ifdef DEBUG_EXPIRED_LOOKBACK
    if (preserve) {
        f->stress = cast(RELVAL*, malloc(sizeof(RELVAL)));
        memcpy(f->stress, *opt_lookback, sizeof(RELVAL));
        lookback = f->stress;
    }
  #endif

    return lookback;
}

#define Fetch_First_In_Feed(feed) \
    Fetch_Next_In_Feed_Core((feed), false)  // !!! not used at time of writing

inline static const RELVAL *Fetch_Next_In_Feed(  // adds not-end checking
    REBFED *feed,
    bool preserve
){
    ASSERT_NOT_END(feed->value);
    return Fetch_Next_In_Feed_Core(feed, preserve);
}


// Most calls to Fetch_Next_In_Frame() are no longer interested in the
// cell backing the pointer that used to be in f->value (this is enforced
// by a rigorous test in DEBUG_EXPIRED_LOOKBACK).  Special care must be
// taken when one is interested in that data, because it may have to be
// moved.  So current can be returned from Fetch_Next_In_Frame_Core().

#define Lookback_While_Fetching_Next(f) \
    Fetch_Next_In_Feed(FRM(f)->feed, true)

#define Fetch_Next_Forget_Lookback(f) \
    ((void)Fetch_Next_In_Feed(FRM(f)->feed, false))


// This code is shared by Literal_Next_In_Feed(), and used without a feed
// advancement in the inert branch of the evaluator.  So for something like
// `loop 2 [append [] 10]`, the steps are:
//
//    1. loop defines its body parameter as <const>
//    2. When LOOP runs Do_Any_Array_At_Throws() on the const ARG(body), the
//       frame gets FEED_FLAG_CONST due to the CELL_FLAG_CONST.
//    3. The argument to append is handled by the inert processing branch
//       which moves the value here.  If the block wasn't made explicitly
//       mutable (e.g. with MUTABLE) it takes the flag from the feed.
//
inline static void Inertly_Derelativize_Inheriting_Const(
    REBVAL *out,
    const RELVAL *v,
    REBFED *feed
){
    Derelativize(out, v, feed->specifier);
    SET_CELL_FLAG(out, UNEVALUATED);
    if (not GET_CELL_FLAG(v, EXPLICITLY_MUTABLE))
        out->header.bits |= (feed->flags.bits & FEED_FLAG_CONST);
}

inline static void Literal_Next_In_Feed(REBVAL *out, REBFED *feed) {
    Inertly_Derelativize_Inheriting_Const(out, feed->value, feed);
    (void)(Fetch_Next_In_Feed(feed, false));
}


inline static REBFED* Alloc_Feed(void) {
    REBFED* feed = cast(REBFED*, Make_Node(FED_POOL));
  #ifdef DEBUG_COUNT_TICKS
    feed->tick = TG_Tick;
  #endif
    return feed;
}

inline static void Free_Feed(REBFED *feed)
  { Free_Node(FED_POOL, cast(REBNOD*, feed)); }


// It is more pleasant to have a uniform way of speaking of frames by pointer,
// so this macro sets that up for you, the same way DECLARE_LOCAL does.  The
// optimizer should eliminate the extra pointer.
//
// Just to simplify matters, the frame cell is set to a bit pattern the GC
// will accept.  It would need stack preparation anyway, and this simplifies
// the invariant so if a recycle happens before Eval_Core() gets to its
// body, it's always set to something.  Using an unreadable blank means we
// signal to users of the frame that they can't be assured of any particular
// value between evaluations; it's not cleared.
//

inline static void Prep_Array_Feed(
    REBFED *feed,
    const RELVAL *opt_first,
    REBARR *array,
    REBLEN index,
    REBSPC *specifier,
    REBFLGS flags
){
    Prep_Stack_Cell(&feed->fetched);
    Init_Unreadable_Blank(&feed->fetched);
    Prep_Stack_Cell(&feed->lookback);
    Init_Unreadable_Blank(&feed->lookback);

    feed->vaptr = nullptr;
    feed->array = array;
    feed->specifier = specifier;
    feed->flags.bits = flags;
    if (opt_first) {
        feed->value = opt_first;
        feed->index = index;
        feed->pending = ARR_AT(array, index);
        ASSERT_NOT_END(feed->value);
    }
    else {
        feed->value = ARR_AT(array, index);
        feed->index = index + 1;
        feed->pending = feed->value + 1;
    }

    feed->gotten = nullptr;
    if (IS_END(feed->value))
        TRASH_POINTER_IF_DEBUG(feed->pending);
    else
        assert(READABLE(feed->value, __FILE__, __LINE__));
}

#define DECLARE_ARRAY_FEED(name,array,index,specifier) \
    REBFED *name = Alloc_Feed(); \
    Prep_Array_Feed(name, \
        nullptr, (array), (index), (specifier), FEED_MASK_DEFAULT \
    );


inline static void Prep_Va_Feed(
    REBFED *feed,
    const void *p,
    va_list *vaptr,
    REBFLGS flags
){
    Prep_Stack_Cell(&feed->fetched);
    Init_Unreadable_Blank(&feed->fetched);
    Prep_Stack_Cell(&feed->lookback);
    Init_Unreadable_Blank(&feed->lookback);

    feed->index = TRASHED_INDEX;  // avoid warning in release build
    feed->array = nullptr;
    feed->flags.bits = flags;
    feed->vaptr = vaptr;
    feed->pending = END_NODE;  // signal next fetch comes from va_list
    feed->specifier = SPECIFIED;  // relative values not allowed
    Detect_Feed_Pointer_Maybe_Fetch(feed, p, false);

    feed->gotten = nullptr;
    assert(IS_END(feed->value) or READABLE(feed->value, __FILE__, __LINE__));
}

// The flags is passed in by the macro here by default, because it does a
// fetch as part of the initialization from the opt_first...and if you want
// FLAG_QUOTING_BYTE() to take effect, it must be passed in up front.
//
#define DECLARE_VA_FEED(name,p,vaptr,flags) \
    REBFED name##struct; \
    Prep_Va_Feed(&name##struct, (p), (vaptr), (flags)); \
    REBFED *name = &name##struct

inline static void Prep_Any_Array_Feed(
    REBFED *feed,
    const RELVAL *any_array,
    REBSPC *specifier,
    REBFLGS parent_flags  // only reads FEED_FLAG_CONST out of this
){
    // Note that `CELL_FLAG_CONST == FEED_FLAG_CONST`
    //
    REBFLGS flags;
    if (GET_CELL_FLAG(any_array, EXPLICITLY_MUTABLE))
        flags = FEED_MASK_DEFAULT;  // override const from parent frame
    else
        flags = FEED_MASK_DEFAULT
            | (parent_flags & FEED_FLAG_CONST)  // inherit
            | (any_array->header.bits & CELL_FLAG_CONST);  // heed

    Prep_Array_Feed(
        feed,
        nullptr,  // opt_first = nullptr, don't inject arbitrary 1st element
        VAL_ARRAY(any_array),
        VAL_INDEX(any_array),
        Derive_Specifier(specifier, any_array),
        flags
    );
}

#define DECLARE_FEED_AT_CORE(name,any_array,specifier) \
    REBFED *name = Alloc_Feed(); \
    Prep_Any_Array_Feed(name, \
        (any_array), (specifier), FS_TOP->feed->flags.bits \
    );

#define DECLARE_FEED_AT(name,any_array) \
    DECLARE_FEED_AT_CORE (name, (any_array), SPECIFIED)
