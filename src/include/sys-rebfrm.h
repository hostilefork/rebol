//
//  File: %sys-frame.h
//  Summary: {Evaluator "Do State"}
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
// The primary routine that handles DO and EVALUATE is Eval_Core().  It
// takes a single parameter which holds the running state of the evaluator.
// This state may be allocated on the C variable stack.
//
// Eval_Core() is written so that a longjmp to a failure handler above
// it can do cleanup safely even though intermediate stacks have vanished.
// This is because Push_Frame and Drop_Frame maintain an independent global
// list of the frames in effect, so that the Fail_Core() routine can unwind
// all the associated storage and structures for each frame.
//
// Revolt can not only run the evaluator across a REBARR-style series of
// input based on index, it can also enumerate through C's `va_list`,
// providing the ability to pass pointers as REBVAL* in a variadic function
// call from the C (comma-separated arguments, as with printf()).  Future data
// sources might also include a REBVAL[] raw C array.
//
// To provide even greater flexibility, it allows the very first element's
// pointer in an evaluation to come from an arbitrary source.  It doesn't
// have to be resident in the same sequence from which ensuing values are
// pulled, allowing a free head value (such as an ACTION! REBVAL in a local
// C variable) to be evaluated in combination from another source (like a
// va_list or series representing the arguments.)  This avoids the cost and
// complexity of allocating a series to combine the values together.
//


#define EVAL_FLAG_0_IS_TRUE FLAG_LEFT_BIT(0)  // IS a node
STATIC_ASSERT(EVAL_FLAG_0_IS_TRUE == NODE_FLAG_NODE);

#define EVAL_FLAG_1_IS_FALSE FLAG_LEFT_BIT(1)  // is NOT free
STATIC_ASSERT(EVAL_FLAG_1_IS_FALSE == NODE_FLAG_FREE);


//=//// EVAL_FLAG_PUSH_TO_STACK ///////////////////////////////////////////=//
//
#define EVAL_FLAG_PUSH_TO_STACK \
    FLAG_LEFT_BIT(2)


//=//// EVAL_FLAG_MARKED //////////////////////////////////////////////////=//
//
// REBFRM are a REBNOD subclass, and may be referred to by FRAME! values that
// keep them alive such that they need to be GC'd.
//
#define EVAL_FLAG_MARKED \
    FLAG_LEFT_BIT(3)
STATIC_ASSERT(EVAL_FLAG_MARKED == NODE_FLAG_MARKED);


//=//// EVAL_FLAG_4_IS_TRUE ///////////////////////////////////////////////////////=//
//
#define EVAL_FLAG_4_IS_TRUE \
    FLAG_LEFT_BIT(4)
STATIC_ASSERT(EVAL_FLAG_4_IS_TRUE == NODE_FLAG_FRAME);


//=//// EVAL_FLAG_5 ///////////////////////////////////////////////////////=//
//
#define EVAL_FLAG_5 \
    FLAG_LEFT_BIT(5)


//=//// EVAL_FLAG_6 ///////////////////////////////////////////////////////=//
//
#define EVAL_FLAG_6 \
    FLAG_LEFT_BIT(6)


#define EVAL_FLAG_7_IS_TRUE FLAG_LEFT_BIT(7)  // !!! Temporary...claims CELL
STATIC_ASSERT(EVAL_FLAG_7_IS_TRUE == NODE_FLAG_CELL);


//=//// EVAL_FLAG_DISPATCHER_CATCHES //////////////////////////////////////=//
//
// While a continuation is running, the native or dispatcher that made the
// request is no longer on the stack.  But if something like a WHILE loop
// wishes to catch a BREAK, it has to be told about it.  Setting this flag
// when asking for a continuation will give a callback in the thrown state,
// which can be tested with Is_Throwing().
//
#define EVAL_FLAG_DISPATCHER_CATCHES \
    FLAG_LEFT_BIT(8)


//=//// EVAL_FLAG_REEVALUATE_CELL /////////////////////////////////////////=//
//
// Function dispatchers have a special return value used by EVAL, which tells
// it to use the frame's cell as the head of the next evaluation (before
// what f->value would have ordinarily run.)
//
// This allows EVAL/ONLY to be implemented by entering a new subframe with
// new flags, and may have other purposes as well.
//
#define EVAL_FLAG_REEVALUATE_CELL \
    FLAG_LEFT_BIT(9)


//=//// EVAL_FLAG_POST_SWITCH /////////////////////////////////////////////=//
//
// This jump allows a deferred lookback to compensate for the lack of the
// evaluator's ability to (easily) be psychic about when it is gathering the
// last argument of a function.  It allows re-entery to argument gathering at
// the point after the switch() statement, with a preloaded f->out.
//
#define EVAL_FLAG_POST_SWITCH \
    FLAG_LEFT_BIT(10)


//=//// EVAL_FLAG_FULFILLING_ARG //////////////////////////////////////////=//
//
// Deferred lookback operations need to know when they are dealing with an
// argument fulfillment for a function, e.g. `summation 1 2 3 |> 100` should
// be `(summation 1 2 3) |> 100` and not `summation 1 2 (3 |> 100)`.  This
// also means that `add 1 <| 2` will act as an error.
//
#define EVAL_FLAG_FULFILLING_ARG \
    FLAG_LEFT_BIT(11)


//=//// EVAL_FLAG_12 //////////////////////////////////////////////////////=//
//
#define EVAL_FLAG_12 \
    FLAG_LEFT_BIT(12)


//=//// EVAL_FLAG_13 //////////////////////////////////////////////////////=//
//
#define EVAL_FLAG_13 \
    FLAG_LEFT_BIT(13)


//=//// EVAL_FLAG_14 //////////////////////////////////////////////////////=//
//
#define EVAL_FLAG_14 \
    FLAG_LEFT_BIT(14)


//=//// EVAL_FLAG_15 //////////////////////////////////////////////////////=//
//
#define EVAL_FLAG_15 \
    FLAG_LEFT_BIT(15)


//=//// EVAL_FLAG_RUNNING_ENFIX ///////////////////////////////////////////=//
//
// IF NOT(EVAL_FLAG_PATH_MODE)...
//
// Due to the unusual influences of partial refinement specialization, a frame
// may wind up with its enfix parameter as being something like the last cell
// in the argument list...when it has to then go back and fill earlier args
// as normal.  There's no good place to hold the memory that one is doing an
// enfix fulfillment besides a bit on the frame itself.
//
// It is also used to indicate to a EVAL_FLAG_REEVALUATE_CELL frame whether
// to run an ACTION! cell as enfix or not.  The reason this may be overridden
// on what's in the action can be seen in the REBNATIVE(shove) code.
//
// IF EVAL_FLAG_PATH_MODE...
//
// (unused)

#define EVAL_FLAG_16 \
    FLAG_LEFT_BIT(16)

#define EVAL_FLAG_RUNNING_ENFIX         EVAL_FLAG_16


//=//// EVAL_FLAG_DIDNT_LEFT_QUOTE_PATH ///////////////////////////////////=//
//
// There is a contention between operators that want to quote their left hand
// side and ones that want to quote their right hand side.  The left hand side
// wins in order for things like `help default` to work.  But deciding on
// whether the left hand side should win or not if it's a PATH! is a tricky
// case, as one must evaluate the path to know if it winds up producing a
// right quoting action or not.
//
// So paths win automatically unless a special (rare) override is used.  But
// if that path doesn't end up being a right quoting operator, it's less
// confusing to give an error message informing the user to use -> vs. just
// make it appear there was no left hand side.
//
#define EVAL_FLAG_DIDNT_LEFT_QUOTE_PATH \
    FLAG_LEFT_BIT(17)


//=//// EVAL_FLAG_PROCESS_ACTION //////////////////////////////////////////=//
//
// Used to indicate that the Eval_Core code is being jumped into directly to
// process an ACTION!, in a varlist that has already been set up.
//
#define EVAL_FLAG_PROCESS_ACTION \
    FLAG_LEFT_BIT(18)


//=//// EVAL_FLAG_NO_PATH_GROUPS //////////////////////////////////////////=//
//
// When EVAL_FLAG_PATH_MODE:
//
// This feature is used in PATH! evaluations to request no side effects.
// It prevents GET of a PATH! from running GROUP!s.
//
// When not EVAL_FLAG_PATH_MODE:
//
// A dispatcher may want to run a "continuation" but not be called back.
// This is referred to as delegation.
//
#define EVAL_FLAG_19 \
    FLAG_LEFT_BIT(19)

#define EVAL_FLAG_NO_PATH_GROUPS EVAL_FLAG_19
#define EVAL_FLAG_DELEGATE_CONTROL EVAL_FLAG_19



//=//// EVAL_FLAG_PATH_MODE ///////////////////////////////////////////////=//
//
// The frame is for a PATH! dispatch.  Many of the Eval_Core() flags are not
// applicable in this case.
//
#define EVAL_FLAG_PATH_MODE \
    FLAG_LEFT_BIT(20)


//=//// EVAL_FLAG_PATH_HARD_QUOTE /////////////////////////////////////////=//
//
// IF EVAL_FLAG_PATH_MODE...
// ...Path processing uses this flag, to say that if a path has GROUP!s in
// it, operations like DEFAULT do not want to run them twice...once on a get
// path and then on a set path.  This means the path needs to be COMPOSEd and
// then use GET/HARD and SET/HARD.
//
// IF NOT(EVAL_FLAG_PATH_MODE)...
// If EVAL_FLAG_POST_SWITCH is being used due to an inert optimization, this
// flag is set, so that the quoting machinery can realize the lookback quote
// is not actually too late.

#define EVAL_FLAG_21 \
    FLAG_LEFT_BIT(21)

#define EVAL_FLAG_INERT_OPTIMIZATION    EVAL_FLAG_21

#define EVAL_FLAG_PATH_HARD_QUOTE       EVAL_FLAG_21


//=//// EVAL_FLAG_CONTINUATION ////////////////////////////////////////////=//
//
// This frame is a continuation, which is to say that when it is finished it
// should not return to the callsite of the evaluator, but it should first
// complete the frame underneath it (which may also be a continuation).
//
// NOTE: This cannot currently be used uniquely in path dispatch because
// the Free_Frame_Internal() mechanics use the flag as an indication that
// it needs the feed to be freed.
//
#define EVAL_FLAG_CONTINUATION \
    FLAG_LEFT_BIT(22)


//=//// EVAL_FLAG_ERROR_ON_DEFERRED_ENFIX /////////////////////////////////=//
//
// There are advanced features that "abuse" the evaluator, e.g. by making it
// create a specialization exemplar by example from a stream of code.  These
// cases are designed to operate in isolation, and are incompatible with the
// idea of enfix operations that stay pending in the evaluation queue, e.g.
//
//     match parse "aab" [some "a" end] else [print "what should this do?"]
//
// MATCH is variadic, and in one step asks to make a frame from the right
// hand side.  But it's 99% likely intent of this was to attach the ELSE to
// the MATCH and not the PARSE.  That looks inconsistent, since the user
// imagines it's the evaluator running PARSE as a parameter to MATCH (vs.
// MATCH becoming the evaluator and running it).
//
// It would be technically possible to allow ELSE to bind to the MATCH in
// this case.  It might even be technically possible to give MATCH back a
// frame for a CHAIN of actions that starts with PARSE but includes the ELSE
// (which sounds interesting but crazy, considering that's not what people
// would want here, but maybe sometimes they would).
//
// The best answer for right now is just to raise an error.
//
#define EVAL_FLAG_ERROR_ON_DEFERRED_ENFIX \
    FLAG_LEFT_BIT(23)


//=//// EVAL_FLAG_REQUOTE_NULL ////////////////////////////////////////////=//
//
// Most routines that try to pass through the quoted level of their input
// can't process a dequoted null (e.g. don't have <opt> input).  Hence if
// a quoted input comes in like '''FOO, but the routine decides to return
// null as a signal, it wants to give back plain null and not ''' as a
// triple-quoted null.
//
// But we use the heuristic that if a routine intentionally takes nulls, then
// a quoted null on input signals requoting a null on output.
//
#define EVAL_FLAG_REQUOTE_NULL \
    FLAG_LEFT_BIT(24)


//=//// EVAL_FLAG_FULLY_SPECIALIZED ///////////////////////////////////////=//
//
// When a null is seen in f->special, the question is whether that is an
// intentional "null specialization" or if it means the argument should be
// gathered normally (if applicable), as it would in a typical invocation.
// If the frame is considered fully specialized (as with DO F) then there
// will be no further argument gathered at the callsite, nulls are as-is.
//
#define EVAL_FLAG_FULLY_SPECIALIZED \
    FLAG_LEFT_BIT(25)


//=//// EVAL_FLAG_NO_RESIDUE //////////////////////////////////////////////=//
//
// Sometimes a single step evaluation is done in which it would be considered
// an error if all of the arguments are not used.  This requests an error if
// the frame does not reach the end.
//
// !!! Interactions with ELIDE won't currently work with this, so evaluation
// would have to take this into account to greedily run ELIDEs if the flag
// is set.  However, it's only used in variadic apply at the moment with
// calls from the system that do not use ELIDE.  These calls may someday
// turn into rebValue(), in which case the mechanism would need rethinking.
//
// !!! A userspace tool for doing this was once conceived as `||`, which
// was variadic and would only allow one evaluation step after it, after
// which it would need to reach either an END or another `||`.
//
#define EVAL_FLAG_NO_RESIDUE \
    FLAG_LEFT_BIT(26)


//=//// EVAL_FLAG_DOING_PICKUPS ///////////////////////////////////////////=//
//
// If an ACTION! is invoked through a path and uses refinements in a different
// order from how they appear in the frame's parameter definition, then the
// arguments at the callsite can't be gathered in sequence.  Revisiting them
// will be necessary.  This flag is set while they are revisited, which is
// important not only for Eval_Core() to know, but also the GC...since it
// means it must protect *all* of the arguments--not just up thru f->param.
//
#define EVAL_FLAG_DOING_PICKUPS \
    FLAG_LEFT_BIT(27)


//=//// EVAL_FLAG_ALLOCATED_FEED //////////////////////////////////////////=//
//
// Some frame recursions re-use a feed that already existed, while others will
// allocate them.  This re-use allows recursions to keep index positions and
// fetched "gotten" values in sync.  The dynamic allocation means that feeds
// can be kept alive across contiuations--which wouldn't be possible if they
// were on the C stack.
//
// If a frame allocated a feed, then it has to be freed...which is done when
// the frame is dropped or aborted.
//
#define EVAL_FLAG_ALLOCATED_FEED \
    FLAG_LEFT_BIT(28)


//=//// EVAL_FLAG_PUSH_PATH_REFINES + EVAL_FLAG_BLAME_PARENT //////////////=//
//
// IF EVAL_FLAG_PATH_MODE...
//
// It is technically possible to produce a new specialized ACTION! each
// time you used a PATH!.  This is needed for `apdo: :append/dup/only` as a
// method of partial specialization, but would be costly if just invoking
// a specialization once.  So path dispatch can be asked to push the path
// refinements in the reverse order of their invocation.
//
// This mechanic is also used by SPECIALIZE, so that specializing refinements
// in order via a path and values via a block of code can be done in one
// step, vs needing to make an intermediate ACTION!.
//
// IF NOT(EVAL_FLAG_PATH_MODE)...
//
// Marks an error to hint that a frame is internal, and that reporting an
// error on it probably won't give a good report.
//
#define EVAL_FLAG_29 \
    FLAG_LEFT_BIT(29)

#define EVAL_FLAG_PUSH_PATH_REFINES         EVAL_FLAG_29
#define EVAL_FLAG_BLAME_PARENT              EVAL_FLAG_29


//=//// EVAL_FLAG_FULFILL_ONLY ////////////////////////////////////////////=//
//
// In some scenarios, the desire is to fill up the frame but not actually run
// an action.  At one point this was done with a special "dummy" action to
// dodge having to check the flag on every dispatch.  But in the scheme of
// things, checking the flag is negligible...and it's better to do it with
// a flag so that one does not lose the paramlist information one was working
// with (overwriting with a dummy action on FRM_PHASE() led to an inconsistent
// case that had to be accounted for, since the dummy's arguments did not
// line up with the frame being filled).
//
#define EVAL_FLAG_FULFILL_ONLY \
    FLAG_LEFT_BIT(30)


//=//// EVAL_FLAG_31 ///////////////////////////////////////=//
//
#define EVAL_FLAG_31 \
    FLAG_LEFT_BIT(31)


STATIC_ASSERT(31 < 32);  // otherwise EVAL_FLAG_XXX too high


// Default for Eval_Core_May_Throw() is just a single EVALUATE step.
//
#define EVAL_MASK_DEFAULT \
    (EVAL_FLAG_0_IS_TRUE | EVAL_FLAG_4_IS_TRUE | EVAL_FLAG_7_IS_TRUE)


#define SET_EVAL_FLAG(f,name) \
    (FRM(f)->flags.bits |= EVAL_FLAG_##name)

#define GET_EVAL_FLAG(f,name) \
    ((FRM(f)->flags.bits & EVAL_FLAG_##name) != 0)

#define CLEAR_EVAL_FLAG(f,name) \
    (FRM(f)->flags.bits &= ~EVAL_FLAG_##name)

#define NOT_EVAL_FLAG(f,name) \
    ((FRM(f)->flags.bits & EVAL_FLAG_##name) == 0)


#define TRASHED_INDEX ((REBLEN)(-3))

#define IS_KIND_INERT(k) \
    ((k) >= REB_BLOCK)


struct Reb_Feed {
    union Reb_Header flags;  // quoting level included

    // A frame may be sourced from a va_list of pointers, or not.  If this is
    // NULL it is assumed that the values are sourced from a simple array.
    //
    // Note: This second platform-pointer-sized thing is in the second slot
    // to offset the ensuing cells so that they align on 64-bit boundaries
    // on a 32-bit platform.  See ALIGN_SIZE for more details.
    //
    va_list *vaptr;

    // Sometimes the frame can be advanced without keeping track of the
    // last cell.  And sometimes the last cell lives in an array that is
    // being held onto and read only, so its pointer is guaranteed to still
    // be valid after a fetch.  But there are cases where values are being
    // read from transient sources that disappear as they go...if that is
    // the case, and lookback is needed, it is written into this cell.
    //
    RELVAL lookback;

    // When feeding cells from a variadic, those cells may wish to mutate the
    // value in some way... e.g. to add a quoting level.  Rather than
    // complicate the evaluator itself with flags and switches, each frame
    // has a holding cell which can optionally be used as the pointer that
    // is returned by Fetch_Next_in_Frame(), where arbitrary mutations can
    // be applied without corrupting the value they operate on.
    //
    RELVAL fetched;

    // If the binder isn't NULL, then any words or arrays are bound into it
    // during the loading process.  
    //
    // !!! Note: At the moment a UTF-8 string is seen in the feed, it sets
    // these fields on-demand, and then runs a scan of the entire rest of the
    // feed, caching it.  It doesn't have a choice as only one binder can
    // be in effect at a time, and so it can't run code as it goes.
    //
    // Hence these fields aren't in use at the same time as the lookback
    // at this time; since no evaluations are being done.  They could be put
    // into a pseudotype cell there, if this situation of scanning-to-end
    // is a going to stick around.  But it is slow and smarter methods are
    // going to be necessary.
    //
    struct Reb_Binder *binder;
    REBCTX *lib;  // does not expand, has negative indices in binder
    REBCTX *context;  // expands, has positive indices in binder

    // This contains an IS_END() marker if the next fetch should be an attempt
    // to consult the va_list (if any).  That end marker may be resident in
    // an array, or if it's a plain va_list source it may be the global END.
    //
    const RELVAL *pending;

    // If values are being sourced from an array, this holds the pointer to
    // that array.  By knowing the array it is possible for error and debug
    // messages to reach backwards and present more context of where the
    // error is located.
    //
    REBARR *array;

    // This holds the index of the *next* item in the array to fetch as
    // f->value for processing.  It's invalid if the frame is for a C va_list.
    //
    REBLEN index;

    // This is used for relatively bound words to be looked up to become
    // specific.  Typically the specifier is extracted from the payload of the
    // ANY-ARRAY! value that provided the source.array for the call to DO.
    // It may also be NULL if it is known that there are no relatively bound
    // words that will be encountered from the source--as in va_list calls.
    //
    REBSPC *specifier;

    // This is the "prefetched" value being processed.  Entry points to the
    // evaluator must load a first value pointer into it...which for any
    // successive evaluations will be updated via Fetch_Next_In_Frame()--which
    // retrieves values from arrays or va_lists.  But having the caller pass
    // in the initial value gives the option of that value being out of band.
    //
    // (Hence if one has the series `[[a b c] [d e]]` it would be possible to
    // have an independent path value `append/only` and NOT insert it in the
    // series, yet get the effect of `append/only [a b c] [d e]`.  This only
    // works for one value, but is a convenient no-cost trick for apply-like
    // situations...as insertions usually have to "slide down" the values in
    // the series and may also need to perform alloc/free/copy to expand.
    // It also is helpful since in C, variadic functions must have at least
    // one non-variadic parameter...and one might want that non-variadic
    // parameter to be blended in with the variadics.)
    //
    // !!! Review impacts on debugging; e.g. a debug mode should hold onto
    // the initial value in order to display full error messages.
    //
    NEVERNULL(const RELVAL *) value;

    // There is a lookahead step to see if the next item in an array is a
    // WORD!.  If so it is checked to see if that word is a "lookback word"
    // (e.g. one that refers to an ACTION! value set with SET/ENFIX).
    // Performing that lookup has the same cost as getting the variable value.
    // Considering that the value will need to be used anyway--infix or not--
    // the pointer is held in this field for WORD!s.
    //
    // However, reusing the work is not possible in the general case.  For
    // instance, this would cause a problem:
    //
    //     obj: make object! [x: 10]
    //     foo: does [append obj [y: 20]]
    //     do in obj [foo x]
    //                   ^-- consider the moment of lookahead, here
    //
    // Before foo is run, it will fetch x to ->gotten, and see that it is not
    // a lookback function.  But then when it runs foo, the memory location
    // where x had been found before may have moved due to expansion.
    //
    // Basically any function call invalidates ->gotten, as does obviously any
    // Fetch_Next_In_Frame (because the position changes).  So it has to be
    // nulled out fairly often, and checked for null before reuse.
    //
    // !!! Review how often gotten has hits vs. misses, and what the benefit
    // of the feature actually is.
    //
    const REBVAL *gotten;

  #if defined(DEBUG_EXPIRED_LOOKBACK)
    //
    // On each call to Fetch_Next_In_Feed, it's possible to ask it to give
    // a pointer to a cell with equivalent data to what was previously in
    // f->value, but that might not be f->value.  So for all practical
    // purposes, one is to assume that the f->value pointer died after the
    // fetch.  If clients are interested in doing "lookback" and examining
    // two values at the same time (or doing a GC and expecting to still
    // have the old f->current work), then they must not use the old f->value
    // but request the lookback pointer from Fetch_Next_In_Frame().
    //
    // To help stress this invariant, frames will forcibly expire REBVAL
    // cells, handing out disposable lookback pointers on each eval.
    //
    // !!! Test currently leaks on shutdown, review how to not leak.
    //
    RELVAL *stress;
  #endif

   #if defined(DEBUG_COUNT_TICKS)
    uintptr_t tick;
  #endif
};


// NOTE: The ordering of the fields in `Reb_Frame` are specifically done so
// as to accomplish correct 64-bit alignment of pointers on 64-bit systems.
//
// Because performance in the core evaluator loop is system-critical, this
// uses full platform `int`s instead of REBLENs.  If modifying the structure,
// be sensitive to this issue.
//
struct Reb_Frame {
    //
    // These are EVAL_FLAG_XXX or'd together--see their documentation above.
    // See REBNOD for the idea of fitting into the Rebol pointer ecology.
    // A REBFRM* needs to be distinguishable from a REBSER*.
    //
    union Reb_Header flags;  // See Endlike_Header()

    // The prior call frame.  This never needs to be checked against nullptr,
    // because the bottom of the stack is FS_BOTTOM which is allocated at
    // startup and never used to run code.
    //
    // Note: This platform-pointer-sized item is in position 2 so that the
    // `spare` cell is 64-bit aligned on 32-bit platforms.
    //
    struct Reb_Frame *prior;

    // The frame's "spare" is used for different purposes.  PARSE uses it as a
    // scratch storage space.  Path evaluation uses it as where the calculated
    // "picker" goes (so if `foo/(1 + 2)`, the 3 would be stored there to be
    // used to pick the next value in the chain).
    //
    // The evaluator uses it as a general temporary place for evaluations, but
    // it is available for use by natives while they are running.  This is
    // particularly useful because it is GC guarded and also a valid target
    // location for evaluations.  (The argument cells of a native are *not*
    // legal evaluation targets, although they can be used as GC safe scratch
    // space for things other than evaluation.)
    //
    RELVAL spare;


    // !!! There was quite a bit of tweaking done to avoid caching a Reb_Kind
    // in the frame state.  However, with a non-recursive evaluator that just
    // loops and jumps, it has to know which kind of continuation it just
    // finished.  This will ultimately need to be more complex, but for the
    // moment use an enumeration just to get things going.
    //
    uintptr_t continuation_type;

    // The data stack pointer captured on entry to the evaluation.  It is used
    // by debug checks to make sure the data stack stays balanced after each
    // sub-operation.  It's also used to measure how many refinements have
    // been pushed to the data stack by a path evaluation.
    //
    uintptr_t dsp_orig; // type is REBDSP, but enforce alignment here

    // This is where to write the result of the evaluation.  It should not be
    // in "movable" memory, hence not in a series data array.  Often it is
    // used as an intermediate free location to do calculations en route to
    // a final result, due to being GC-safe during function evaluation.
    //
    REBVAL *out;

    // This is the source from which new values will be fetched.  In addition
    // to working with an array, it is also possible to feed the evaluator
    // arbitrary REBVAL*s through a variable argument list on the C stack.
    // This means no array needs to be dynamically allocated (though some
    // conditions require the va_list to be converted to an array, see notes
    // on Reify_Va_To_Array_In_Frame().)
    //
    // Since frames may share source information, this needs to be done with
    // a dereference.
    //
    REBFED *feed;

    // The error reporting machinery doesn't want where `index` is right now,
    // but where it was at the beginning of a single EVALUATE step.
    //
    uintptr_t expr_index;

    // If a function call is currently in effect, FRM_PHASE() is how you get
    // at the current function being run.  This is the action that started
    // the process.
    //
    // Compositions of functions (adaptations, specializations, hijacks, etc)
    // update the FRAME!'s payload in the f->varlist archetype to say what
    // the current "phase" is.  The reason it is updated there instead of
    // as a REBFRM field is because specifiers use it.  Similarly, that is
    // where the binding is stored.
    //
    REBACT *original;

    // Functions don't have "names", though they can be assigned to words.
    // However, not all function invocations are through words or paths, so
    // the label may not be known.  It is NULL to indicate anonymity.
    //
    // The evaluator only enforces that the symbol be set during function
    // calls--in the release build, it is allowed to be garbage otherwise.
    //
    REBSTR *opt_label;

    // The varlist is where arguments for the frame are kept.  Though it is
    // ultimately usable as an ordinary CTX_VARLIST() for a FRAME! value, it
    // is different because it is built progressively, with random bits in
    // its pending capacity that are specifically accounted for by the GC...
    // which limits its marking up to the progress point of `f->param`.
    //
    // It starts out unmanaged, so that if no usages by the user specifically
    // ask for a FRAME! value, and the REBCTX* isn't needed to store in a
    // Derelativize()'d or Move_Velue()'d value as a binding, it can be
    // reused or freed.  See Push_Action() and Drop_Action() for the logic.
    //
    REBARR *varlist;
    REBVAL *rootvar; // cache of CTX_ARCHETYPE(varlist) if varlist is not null

    // We use the convention that "param" refers to the TYPESET! (plus symbol)
    // from the spec of the function--a.k.a. the "formal argument".  This
    // pointer is moved in step with `arg` during argument fulfillment.
    //
    // (Note: It is const because we don't want to be changing the params,
    // but also because it is used as a temporary to store value if it is
    // advanced but we'd like to hold the old one...this makes it important
    // to protect it from GC if we have advanced beyond as well!)
    //
    // Made relative just to have another RELVAL on hand.
    //
    const RELVAL *param;

    // `arg is the "actual argument"...which holds the pointer to the
    // REBVAL slot in the `arglist` for that corresponding `param`.  These
    // are moved in sync.  This movement can be done for typechecking or
    // fulfillment, see In_Typecheck_Mode()
    //
    // If arguments are actually being fulfilled into the slots, those
    // slots start out as trash.  Yet the GC has access to the frame list,
    // so it can examine f->arg and avoid trying to protect the random
    // bits that haven't been fulfilled yet.
    //
    REBVAL *arg;

    // `special` may be the same as `param` (if fulfilling an unspecialized
    // function) or it may be the same as `arg` (if doing a typecheck pass).
    // Otherwise it points into values of a specialization or APPLY, where
    // non-null values are being written vs. acquiring callsite parameters.
    //
    // It is assumed that special, param, and arg may all be incremented
    // together at the same time...reducing conditionality (this is why it
    // is `param` and not nullptr when processing unspecialized).
    //
    // However, in PATH! frames, `special` is non-NULL if this is a SET-PATH!,
    // and it is the value to ultimately set the path to.  The set should only
    // occur at the end of the path, so most setters should check
    // `IS_END(pvs->value + 1)` before setting.
    //
    // !!! See notes at top of %c-path.c about why the path dispatch is more
    // complicated than simply being able to only pass the setval to the last
    // item being dispatched (which would be cleaner, but some cases must
    // look ahead with alternate handling).
    //
    const REBVAL *special;

    REBLEN requotes; // negative means null result should be requoted

  union {
    //
    // References are used by path dispatch.
    //
    struct {
        RELVAL *cell;
        REBSPC *specifier;
    } ref;

    // Used to slip cell to re-evaluate into Eval_Core()
    //
    struct {
        const REBVAL *value;
    } reval;

    // Used to parameterize REB_R_CONTINUATION.
    //
    struct {
        const RELVAL *branch;
        REBSPC *branch_specifier;
        const REBVAL *with;
    } cont;
  } u;

   #if defined(DEBUG_COUNT_TICKS)
    //
    // The expression evaluation "tick" where the Reb_Frame is starting its
    // processing.  This is helpful for setting breakpoints on certain ticks
    // in reproducible situations.
    //
    uintptr_t tick; // !!! Should this be in release builds, exposed to users?
  #endif

  #if defined(DEBUG_FRAME_LABELS)
    //
    // Knowing the label symbol is not as handy as knowing the actual string
    // of the function this call represents (if any).  It is in UTF8 format,
    // and cast to `char*` to help debuggers that have trouble with REBYTE.
    //
    const char *label_utf8;
  #endif

  #if !defined(NDEBUG)
    //
    // An emerging feature in the system is the ability to connect user-seen
    // series to a file and line number associated with their creation,
    // either their source code or some trace back to the code that generated
    // them.  As the feature gets better, it will certainly be useful to be
    // able to quickly see the information in the debugger for f->feed.
    //
    const char *file; // is REBYTE (UTF-8), but char* for debug watch
    int line;
  #endif

  #if !defined(NDEBUG)
    //
    // Frames can be re-entered and re-used...this is fundamental to the
    // evaluation model.  So it's important that any flags that are changed
    // get put back how they were.  It's also a good check that whatever was
    // manipulating that state knew what it was doing enough to clear it.
    //
    REBFLGS initial_flags;
  #endif

  #if defined(DEBUG_BALANCE_STATE)
    //
    // Debug reuses PUSH_TRAP's snapshotting to check for leaks at each stack
    // level.  It can also be made to use a more aggresive leak check at every
    // evaluator step--see BALANCE_CHECK_EVERY_EVALUATION_STEP.
    //
    struct Reb_State state;
  #endif

  #if defined(DEBUG_ENSURE_FRAME_EVALUATES)
    //
    // Originally in R3-Alpha the evaluator would be skipped for empty arrays.
    // This meant that `forever []` would never get a chance to run.  Revolt
    // always runs the evaluator--see DEBUG_ENSURE_FRAME_EVALUATES definition.
    //
    bool was_eval_called;
  #endif
};


#define FS_TOP (TG_Top_Frame + 0) // avoid assign to FS_TOP via + 0
#define FS_BOTTOM (TG_Bottom_Frame + 0) // avoid assign to FS_BOTTOM via + 0


// Hookable evaluator core function (see PG_Eval_Maybe_Stale_Throws)
// Unlike a dispatcher, its result is always in the frame's ->out cell, and
// the boolean result only tells you whether or not it threw.
//
typedef bool (REBEVL)(void);


#if !defined(DEBUG_CHECK_CASTS)

    #define FRM(p) \
        cast(REBFRM*, (p)) // FRM() just does a cast (maybe with added checks)

#else

    template <class T>
    inline REBFRM *FRM(T *p) {
        constexpr bool base = std::is_same<T, void>::value
            or std::is_same<T, REBNOD>::value
            or std::is_same<T, REBFRM>::value;

        static_assert(base, "FRM() works on void/REBNOD/REBFRM");

        if (base and (reinterpret_cast<REBNOD*>(p)->header.bits & (
            NODE_FLAG_NODE | NODE_FLAG_FREE | NODE_FLAG_CELL
        )) != (
            NODE_FLAG_NODE | NODE_FLAG_CELL
        )){
            panic (p);
        }

        return reinterpret_cast<REBFRM*>(p);
    }

#endif
