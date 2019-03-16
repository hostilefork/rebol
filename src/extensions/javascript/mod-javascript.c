//
//  File: %mod-javascript.c
//  Summary: "Support for calling Javascript from Rebol in Emscripten build"
//  Section: Extension
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2018-2019 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
//=////////////////////////////////////////////////////////////////////////=//
//
// See %extensions/javascript/README.md
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * This extension expands the RL_rebXXX() API with new entry points.  It
//   was tried to avoid this--doing everything with helper natives.  This
//   would use things like `reb.UnboxInteger("rebpromise-helper", ...)` and
//   build a pure-JS reb.Promise() on top of that.  But in addition to the
//   inefficiency intrinsic to such approaches, reb.UnboxInteger() has to
//   allocate stack for the va_list calling convention.  This disrupts the
//   "sneaky exit and reentry" done by the emterpreter.  All told, adding
//   raw WASM entry points like RL_rebPromise_internal() is more practical,
//   and happens to be faster too.
//
// * Return codes from pthread primitives that can only come from usage errors
//   are not checked (e.g. `pthread_mutex_lock()`).  We only check ones from
//   circumstances like system resource exhaustion (e.g. `pthread_create()`).
//   This tradeoff balances readability.  Example precedent:
//
//   https://www.cs.cmu.edu/afs/cs/academic/class/15492-f07/www/pthreads.html
//

// Older emscripten.h do an #include of <stdio.h>, so %sys-core.h must allow
// it until this: https://github.com/emscripten-core/emscripten/pull/8089
//
#if !defined(DEBUG_STDIO_OK)
    #define DEBUG_STDIO_OK
#endif

#include "sys-core.h"

#include "tmp-mod-javascript.h"

#include <limits.h>  // for UINT_MAX

#include <emscripten.h>

#if defined(USE_EMTERPRETER) == defined(USE_PTHREADS)
    #error "Define one (and only one) of USE_EMTERPRETER or USE_PTHREADS"
#endif

#if defined(USE_PTHREADS)
    #include <pthread.h>  // C11 threads not much better (and less portable)
#endif


//=//// DEBUG_JAVASCRIPT_EXTENSION TOOLS //////////////////////////////////=//
//
// Ren-C has a very aggressive debug build.  Turning on all the debugging
// means a prohibitive experience in emscripten--not just in size and speed of
// the build products, but the compilation can wind up taking a long time--or
// not succeeding at all).
//
// So most of the system is built with NDEBUG, and no debugging is built
// in for the emscripten build.  The hope is that the core is tested elsewhere
// (or if a bug is encountered in the interpreter under emscripten, it will
// be reproduced and can be debugged in a non-JavaScript build).
//
// However, getting some amount of feedback in the console is essential to
// debugging the JavaScript extension itself.  These are some interim hacks
// for doing that until better ideas come along.

#ifdef DEBUG_JAVASCRIPT_EXTENSION
    #undef assert  // if it was defined (most emscripten builds are NDEBUG)
    #define assert(expr) \
        do { if (!(expr)) { \
            printf("%s:%d - assert(%s)\n", __FILE__, __LINE__, #expr); \
            exit(0); \
        } } while (0)

    bool PG_JS_Trace = false;  // Can be turned on/off with JS-TRACE native

    #define TRACE(...)  /* variadic, but emscripten is at least C99! :-) */ \
        do { if (PG_JS_Trace) { \
            printf("@%ld: ", cast(long, TG_Tick));  /* tick count prefix */ \
            printf(__VA_ARGS__); \
            printf("\n");  /* console.log() won't show up until newline */ \
            fflush(stdout);  /* just to be safe */ \
        } } while (0)

    // TRASH_POINTER_IF_DEBUG() is defined in release builds as a no-op, but
    // it's kind of complicated.  For the purposes in this file these END
    // macros work just as well and don't collide.

    #define ENDIFY_POINTER_IF_DEBUG(p) \
        p = m_cast(REBVAL*, END_NODE)

    #define IS_POINTER_END_DEBUG(p) \
        cast(void*, p) == cast(void*, m_cast(REBVAL*, END_NODE))

    // One of the best pieces of information to follow for a TRACE() is what
    // the EM_ASM() calls.  So printing the JavaScript sent to execute is
    // very helpful.  But it's not possible to "hook" EM_ASM() in terms of
    // its previous definition:
    //
    // https://stackoverflow.com/q/3085071/
    //
    // Fortunately the definitions for EM_ASM() are pretty simple, so writing
    // them again is fine...just needs to change if emscripten.h does.
    // (Note that EM_ASM_INT would require changes to TRACE() as implemented)
    //
    #undef EM_ASM
    #define EM_ASM(code, ...) \
        TRACE("EM_ASM(%s)", #code); \
        ((void)emscripten_asm_const_int(#code _EM_ASM_PREP_ARGS(__VA_ARGS__)))
#else
    // assert() is defined as a noop in release builds already

    #define TRACE(...)                      NOOP
    #define ENDIFY_POINTER_IF_DEBUG(p)      NOOP
    #define IS_POINTER_END_DEBUG(p)         NOOP
#endif


//=//// HEAP ADDRESS ABSTRACTION //////////////////////////////////////////=//
//
// Generally speaking, C exchanges integers with JavaScript.  These integers
// (e.g. the ones that come back from EM_ASM_INT) are typed as `unsigned int`.
// That's unfortunately not a `uintptr_t`...which would be a type that by
// definition can hold any pointer.  But there are cases in the emscripten
// code where this is presumed to be good enough to hold any heap address.
//
// Track the places that make this assumption with `heapaddr_t`, and sanity
// check that we aren't truncating any C pointers in the conversions.
//
// Note heap addresses can be used as ID numbers in JavaScript for mapping
// C entities to JavaScript objects that cannot be referred to directly.
// Tables referring to them must be updated when the related pointer is
// freed, as the pointer may get reused.

typedef unsigned int heapaddr_t;

inline static heapaddr_t Heapaddr_From_Pointer(void *p) {
    intptr_t i = cast(intptr_t, cast(void*, p));
    assert(i < UINT_MAX);
    return i;
}

inline static void* Pointer_From_Heapaddr(heapaddr_t addr)
  { return cast(void*, cast(intptr_t, addr)); }


//=//// FRAME ID AND THROWING /////////////////////////////////////////////=//
//
// We go ahead and use the REBCTX* instead of the raw REBFRM* to act as the
// unique pointer to identify a frame.  That's because if the JavaScript code
// throws and that throw needs to make it to a promise higher up the stack, it
// uses that pointer as an ID in a mapping table (on the main thread) to
// associate the call with the JavaScript object it threw.
//
// !!! This aspect is overkill for something that can only happen once on
// the stack at a time.  Review.
//
// !!! Future designs may translate that object into Rebol so it could
// be caught by Rebol, but for now we assume a throw originating from
// JavaScript code may only be caught by JavaScript code.
//

inline static heapaddr_t Frame_Id_For_Frame_May_Outlive_Call(REBFRM* f) {
    REBCTX *frame_ctx = Context_For_Frame_May_Manage(f);
    return Heapaddr_From_Pointer(frame_ctx);
}

static REBCTX *js_throw_nocatch_converter(REBVAL *thrown)
{
    // If this is a pthread build and we're not on the main thread, we have
    // to do maneuvers to get the information off the GUI thread and to
    // remove the thrown object from the table.  First test the concept.

    return Error_User(
        "JavaScript error not in rebPromise()! (TBD: extract string here)"
    );
}

inline static REB_R Init_Throw_For_Frame_Id(REBVAL *out, heapaddr_t frame_id)
{
    // Note that once the throw is performed, the context will be expired.
    // VAL_CONTEXT() will fail, so VAL_NODE() must be used to extract the
    // heap address.
    //
    REBCTX *frame_ctx = CTX(Pointer_From_Heapaddr(frame_id));
    REBVAL *frame = CTX_ARCHETYPE(frame_ctx);

    // Use NOCATCH_FUNC trick in the throw machinery, so that we can bubble
    // the throw up and see if we find a rebPromise() which is equipped to
    // unpack a JavaScript object.  If not, the function converts to a
    // Rebol ERROR! failure.
    //
    DECLARE_LOCAL (handle);
    Init_Handle_Cfunc(handle, cast(CFUNC*, &js_throw_nocatch_converter));

    return Init_Thrown_With_Label(out, frame, handle);
}


//=//// JS-NATIVE PER-ACTION! DETAILS /////////////////////////////////////=//
//
// All Rebol ACTION!s that claim to be natives have to provide a BODY field
// for source, and an ANY-CONTEXT! that indicates where any API calls will
// be bound while that native is on the stack.  For now, if you're writing
// any JavaScript native it will presume binding in the user context.
//
// (A refinement could be added to control this, e.g. JS-NATIVE/CONTEXT.
// But generally the caller of the API can override with their own binding.)
//
// For the JS-native-specific information, it uses a HANDLE!...but only to
// get the GC hook a handle provides.  When a JavaScript native is GC'd, it
// calls into JavaScript to remove the mapping from integer to function that
// was put in that table at the time of creation (the native_id).
//

inline static heapaddr_t Native_Id_For_Action(REBACT *act)
  { return Heapaddr_From_Pointer(ACT_PARAMLIST(act)); }

#define IDX_JS_NATIVE_HANDLE \
    IDX_NATIVE_MAX  // handle gives hookpoint for GC of table entry

#define IDX_JS_NATIVE_IS_AWAITER \
    (IDX_NATIVE_MAX + 1)  // LOGIC! of if this is an awaiter or not

#define IDX_JS_NATIVE_MAX \
    (IDX_JS_NATIVE_IS_AWAITER + 1)

REB_R JavaScript_Dispatcher(REBFRM *f);

static void cleanup_js_native(const REBVAL *v) {
    REBARR *paramlist = ARR(VAL_HANDLE_POINTER(REBARR*, v));
    heapaddr_t native_id = Native_Id_For_Action(ACT(paramlist));
    assert(native_id < UINT_MAX);
    EM_ASM(
        { reb.UnregisterId_internal($0); },  // don't leak map[int->JS funcs]
        native_id  // => $0
    );
}


//=//// GLOBAL PROMISE STATE //////////////////////////////////////////////=//
//
// Several promises can be requested sequentially, and so they queue up in
// a linked list.  If Rebol were multithreaded, we would be able to start
// those threads and run them while the MAIN were still going...but since it
// is not, we have to wait until the MAIN is idle and isn't making any calls
// into libRebol.
//
// See %extensions/javascript/README.md for a discussion of the emterpreter
// option vs. the PTHREAD option.
//

enum Reb_Promise_State {
    PROMISE_STATE_QUEUEING,
    PROMISE_STATE_RUNNING,
    PROMISE_STATE_AWAITING,
    PROMISE_STATE_RESOLVED,
    PROMISE_STATE_REJECTED
};

struct Reb_Promise_Info {
    enum Reb_Promise_State state;
    heapaddr_t promise_id;

    struct Reb_Promise_Info *next;
};

struct Reb_Promise_Info *PG_Promises;  // Singly-linked list

#if defined(USE_PTHREADS)

    // For why pthread conditions need a mutex:
    // https://stackoverflow.com/q/2763714/

    static pthread_t PG_Main_Thread;
    static pthread_mutex_t PG_Main_Mutex;
    static pthread_cond_t PG_Main_Cond;

    static pthread_t PG_Promise_Thread;
    static pthread_mutex_t PG_Promise_Mutex;
    static pthread_cond_t PG_Promise_Cond;

    // Information cannot be exchanged between the worker thread and the main
    // thread via JavaScript values, so they are proxied between threads as
    // heap pointers via these globals.
    //
    static REBVAL *PG_Promise_Result;
    static REBVAL *PG_Native_Result;

    inline static void ASSERT_ON_MAIN_THREAD() {  // in a browser, this is GUI
        if (not pthread_equal(pthread_self(), PG_Main_Thread))
            assert(!"Expected to be on MAIN thread but wasn't");
    }

    inline static void ASSERT_ON_PROMISE_THREAD() {
        if (not pthread_equal(pthread_self(), PG_Promise_Thread))
            assert(!"Didn't expect to be on MAIN thread but was\n");
    }
#else
    #define ASSERT_ON_PROMISE_THREAD()      NOOP
    #define ASSERT_ON_MAIN_THREAD()         NOOP
#endif


// This returns an integer of a unique memory address it allocated to use in
// a mapping for the [resolve, reject] functions.  We will trigger those
// mappings when the promise is fulfilled.  In order to come back and do that
// fulfillment, it either puts the code processing into a timer callback
// (emterpreter) or queues it to a thread (pthreads).
//
// The resolve will be called if it reaches the end of the input and the
// reject if there is a failure.
//
// Note: See %make-reb-lib.r for code that produces the `rebPromise(...)` API,
// which ties the returned integer into the resolve and reject branches of an
// actual JavaScript ES6 Promise.
//
static intptr_t rebPromise_internal(
    REBFLGS flags,
    void *p,
    va_list *vaptr
){
    TRACE("rebPromise_internal() called");
    ASSERT_ON_MAIN_THREAD();

    // If we're asked to run `rebPromise("input")` from the MAIN thread, there
    // is no way of that being fulfilled synchronously.  But could if you were
    // doing something like `rebPromise("1 + 2")`.  Speculatively running
    // and then yielding only on asynchronous requests would be *technically*
    // possible in the pthread model, but would require each API entry point
    // to take an interpreter lock.  The emterpreter is incapable of doing it
    // (it would be stuck in a JS stack it can't sleep_with_yield() from).
    //
    // But there's also an issue that if we allow a thread to run now, then we
    // would have to block the MAIN thread from running.  And while the MAIN
    // was blocked we might actually fulfill the promise in question.  But
    // then this would need a protocol for returning already fulfilled
    // promises--which becomes a complex management exercise of when the
    // table entry is freed for the promise.
    //
    // To keep the contract simple (and not having a wildly different version
    // for the emterpreter vs. not), we don't execute anything now.  Instead
    // we spool the request into an array.  Then we use `setTimeout()` to ask
    // to execute that array in a callback at the top level.  This permits
    // an emterpreter sleep_with_yield(), or running a thread that can take
    // for granted the resolve() function created on return from this helper
    // already exists.

    DECLARE_VA_FEED (feed, p, vaptr, flags);

    REBDSP dsp_orig = DSP;
    while (NOT_END(feed->value)) {
        Derelativize(DS_PUSH(), feed->value, feed->specifier);
        SET_CELL_FLAG(DS_TOP, UNEVALUATED);
        Fetch_Next_In_Feed(feed, false);
    }
    // Note: exhausting feed should take care of the va_end()

    REBARR *code = Pop_Stack_Values(dsp_orig);
    assert(NOT_SERIES_FLAG(code, MANAGED));  // using array as ID, don't GC it

    // We singly link the promises such that they will be executed backwards.
    // What's good about that is that it will help people realize that over
    // the long run, there's no ordering guarantee of promises (e.g. if they
    // were running on individual threads).

    if (PG_Promises == nullptr) { // e.g. not already executing a promise
        EM_ASM(
            { setTimeout(function() { _RL_rebIdle_internal(); }, 0); }
        );  // note `_RL` (leading underscore means no cwrap)
    }

    struct Reb_Promise_Info *info = ALLOC(struct Reb_Promise_Info);
    info->state = PROMISE_STATE_QUEUEING;
    info->promise_id = cast(intptr_t, code);
    info->next = PG_Promises;
    PG_Promises = info;

    return info->promise_id;
}

EMSCRIPTEN_KEEPALIVE EXTERN_C
intptr_t RL_rebPromise(void *p, va_list *vaptr)
  { return rebPromise_internal(FEED_MASK_DEFAULT, p, vaptr); }

EMSCRIPTEN_KEEPALIVE EXTERN_C
intptr_t RL_rebPromiseQ(void *p, va_list *vaptr)
  { return rebPromise_internal(FLAG_QUOTING_BYTE(1), p, vaptr); }

EMSCRIPTEN_KEEPALIVE EXTERN_C
bool RL_rebIsPromiseDuringPromise_internal(void)
{
    return false;
}


// Function passed to rebRescue() so code can be run but trap errors safely.
//
REBVAL *Run_Array_Dangerous(void *opaque) {
    REBARR *code = cast(REBARR*, opaque);

    REBVAL *result = Alloc_Value();
    if (Do_At_Mutable_Throws(result, code, 0, SPECIFIED)) {
        if (IS_HANDLE(VAL_THROWN_LABEL(result))) {
            CATCH_THROWN(result, result);
            assert(IS_FRAME(result));
            return result;
        }

        fail (Error_No_Catch_For_Throw(result));
    }

    // We want to distinguish an error that was raised from an error that
    // was returned by value.  Here we experiment with using the MARKED flag.
    // An error that comes back from the trap won't have the flag.
    //
    assert(NOT_CELL_FLAG(result, MARKED_RESULT_SUCCESS));
    SET_CELL_FLAG(result, MARKED_RESULT_SUCCESS);
    return result;
}


#if defined(USE_PTHREADS)

// Worker pthread that loops, picks up promise work items, and runs the
// associated array of code.
//
void *promise_worker(void *vargp)
{
    assert(IS_POINTER_END_DEBUG(vargp));  // unused arg (reads PG_Promises)

    ASSERT_ON_PROMISE_THREAD();

    // This loop should have a signal to exit cleanly and shut down the
    // worker thread: https://forum.rebol.info/t/960
    //
    while (true) {
        //
        // Wait until we're signaled that MAIN won't make any libRebol calls.
        // (the signal comes from rebIdle(), which blocks the MAIN)
        //
        TRACE("promise_worker() => waiting on promise condition");
        pthread_cond_wait(&PG_Promise_Cond, &PG_Promise_Mutex);
        pthread_mutex_unlock(&PG_Promise_Mutex);
        TRACE("promise_worker() => got signal to start running promise");

        // There should be a promise ready to run if we're awoken here.

        struct Reb_Promise_Info *info = PG_Promises;
        REBARR *code = ARR(Pointer_From_Heapaddr(info->promise_id));
        assert(IS_POINTER_END_DEBUG(PG_Promise_Result));
        assert(info->state == PROMISE_STATE_QUEUEING);
        info->state = PROMISE_STATE_RUNNING;

        // We run the code using rebRescue() so that if there are errors, we
        // will be able to trap them.  the difference between `throw()`
        // and `reject()` in JS is subtle.
        //
        // https://stackoverflow.com/q/33445415/

        REBVAL *result = rebRescue(&Run_Array_Dangerous, code);
        if (info->state == PROMISE_STATE_REJECTED)
            assert(IS_FRAME(result));
        else {
            assert(info->state == PROMISE_STATE_RUNNING);

            if (NOT_CELL_FLAG(result, MARKED_RESULT_SUCCESS)) {
                //
                // Note this could be an uncaught throw error, raised by the
                // Run_Array_Dangerous() itself.
                //
                assert(IS_ERROR(result));
                info->state = PROMISE_STATE_REJECTED;
                TRACE("promise_worker() => error unblocked MAIN => reject");
            }
            else {
                CLEAR_CELL_FLAG(result, MARKED_RESULT_SUCCESS);
                info->state = PROMISE_STATE_RESOLVED;
                TRACE("promise_worker() => unblock MAIN => resolve");
            }
        }

        PG_Promise_Result = result;

        // Signal MAIN to unblock.  (Any time rebIdle() is unblocked, it needs
        // to check promise_result to see if it's ready, or if there's a
        // request to run some other code)
        //
        pthread_mutex_lock(&PG_Main_Mutex);
        pthread_cond_signal(&PG_Main_Cond);
        pthread_mutex_unlock(&PG_Main_Mutex);
        TRACE("promise_worker() => MAIN unblocked, finishing up");
    }
}

#endif  // defined(USE_PTHREADS)


REBVAL *Get_Native_Result(heapaddr_t frame_id)
{
    ASSERT_ON_MAIN_THREAD();
    heapaddr_t result_addr = EM_ASM_INT(
        { return reb.GetNativeResult_internal($0) },
        frame_id  // => $0
    );
    return VAL(Pointer_From_Heapaddr(result_addr));
}

#ifdef USE_PTHREADS
    void Proxy_Native_Result_To_Worker(heapaddr_t frame_id) {
        //
        // We can get the result now...and need to.  (We won't be able to
        // access MAIN variables on the MAIN thread.)
        //
        assert(IS_POINTER_END_DEBUG(PG_Native_Result));
        PG_Native_Result = Get_Native_Result(frame_id);
        EM_ASM(
            { setTimeout(function() { _RL_rebIdle_internal(); }, 0); }
        );  // note `_RL` (leading underscore means no cwrap)
    }
#endif

void Invoke_Js_Body_On_Main(REBFRM *f)
{
    ASSERT_ON_MAIN_THREAD();
    struct Reb_Promise_Info *info = PG_Promises;

    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    bool is_awaiter = VAL_LOGIC(ARR_AT(details, IDX_JS_NATIVE_IS_AWAITER));

    heapaddr_t native_id = Native_Id_For_Action(FRM_PHASE(f));
    heapaddr_t frame_id = Frame_Id_For_Frame_May_Outlive_Call(f);

    TRACE("Invoke_Js_Body_On_Main(%s)", Frame_Label_Or_Anonymous_UTF8(f));

    if (is_awaiter) {
        //
        // We pre-emptively set the state to awaiting, in case it gets
        // resolved during the JavaScript of the awaiter's body--so we can
        // detect a transition back to RUNNING (e.g. resolved).
        //
        assert(info and info->state == PROMISE_STATE_RUNNING);
        info->state = PROMISE_STATE_AWAITING;

        EM_ASM(
            { reb.RunNativeAwaiter_internal($0, $1) },
            native_id,  // => $0
            frame_id  // => $1
        );
    }
    else {
        assert(not info or info->state == PROMISE_STATE_RUNNING);

        EM_ASM(
            { reb.RunNative_internal($0, $1) },
            native_id,  // => $0
            frame_id  // = $1
        );
    }
}


// This is the code that rebPromise() defers to run until there is no
// JavaScript above it or after it on the MAIN thread stack.  This makes it
// safe to use emscripten_sleep_with_yield() inside of it in the emterpreter
// build, and it purposefully holds up the MAIN from running in the pthread
// version so that both threads don't call libRebol APIs at once.
//
EMSCRIPTEN_KEEPALIVE EXTERN_C
void RL_rebIdle_internal(void)  // there should be NO user JS code on stack!
{
    ASSERT_ON_MAIN_THREAD();

    struct Reb_Promise_Info *info = PG_Promises;

    // Each call to rebPromise() queues an idle cycle, but we never process
    // more than one promise per idle.  (We may not finish a promise depending
    // on how many JS-NATIVE and JS-AWAITER calls there are.)  There should
    // always be at least one promise to be dealt with--maybe an unfinished
    // one from before.
    //
    assert(info != nullptr);

    REBARR *code = ARR(Pointer_From_Heapaddr(info->promise_id));
    if (info->state == PROMISE_STATE_QUEUEING) {
        assert(NOT_SERIES_FLAG(code, MANAGED));  // took off so it didn't GC
        SET_SERIES_FLAG(code, MANAGED);  // but need it back on to execute it
    }

    REBVAL *result;

  #if defined(USE_EMTERPRETER)

    assert(code);
    assert(info->state == PROMISE_STATE_QUEUEING);
    info->state = PROMISE_STATE_RUNNING;

    // The "simple" case: we actually stay on the MAIN thread, and JS-AWAITER
    // can call emscripten_sleep_with_yield()...which will just post any
    // requests to continue as a setTimeout().  Emscripten does all the work.
    //
    TRACE("rebIdle() => begin emterpreting promise code");

    result = rebRescue(&Run_Array_Dangerous, code);
    assert(info->state == PROMISE_STATE_RUNNING);

    if (NOT_CELL_FLAG(result, MARKED_RESULT_SUCCESS)) {
        assert(IS_ERROR(result));
        info->state = PROMISE_STATE_REJECTED;
        TRACE("rebIdle() => error in emterpreted promise => reject");
    }
    else {
        CLEAR_CELL_FLAG(result, MARKED_RESULT_SUCCESS);
        info->state = PROMISE_STATE_RESOLVED;
        TRACE("rebIdle() => successful emterpreted promise => resolve");
    }

  #else  // PTHREAD model

    // Harder (but worth it to be able to run WASM direct and not through a
    // bloated and slow bytecode!)  We spawn a thread and have to forcibly
    // block the MAIN so it doesn't keep running (because it could potentially
    // execute more libRebol code and crash the running promise--Rebol isn't
    // multithreaded.)

    // We're about to signal the promise, which may turn around and signal
    // us right back...be sure we guard the signaling so we don't miss it.
    //
    pthread_mutex_lock(&PG_Main_Mutex);

    // We guarantee that the MAIN is at the top level and not running API code,
    // so it's time to signal the promise to make some progress.
    //
    if (info->state == PROMISE_STATE_QUEUEING)
        TRACE("rebIdle() => waking up worker to start promise");
    else if (info->state == PROMISE_STATE_AWAITING)
        TRACE("rebIdle() => waking up worker to resume after awaiting");
    else if (info->state == PROMISE_STATE_RUNNING)
        TRACE("rebIdle() => waking up worker after non-awaiter native");
    else if (info->state == PROMISE_STATE_REJECTED)
        TRACE("rebIdle() => waking up JavaScript_Dispatcher after throw");
    else
        assert(!"rebIdle() bad promise state");
    pthread_mutex_lock(&PG_Promise_Mutex);
    pthread_cond_signal(&PG_Promise_Cond);
    pthread_mutex_unlock(&PG_Promise_Mutex);

    // Have to put the MAIN on hold so it doesn't make any libRebol API calls
    // while the promise is running its own code that might make some.
    //
    TRACE("rebIdle() => blocking MAIN until wakeup signal");
    pthread_cond_wait(&PG_Main_Cond, &PG_Main_Mutex);
    pthread_mutex_unlock(&PG_Main_Mutex);
    TRACE("rebIdle() => MAIN unblocked");

    // The MAIN can be unblocked for several reasons--but in all of these
    // the promise thread should not be running right now.  Either it is
    // finished or it is suspended waiting on a condition to continue.

    if (info->state == PROMISE_STATE_RESOLVED) {
        result = PG_Promise_Result;
        ENDIFY_POINTER_IF_DEBUG(PG_Promise_Result);
    }
    else if (info->state == PROMISE_STATE_REJECTED) {
        result = PG_Promise_Result;
        ENDIFY_POINTER_IF_DEBUG(PG_Promise_Result);
    }
    else {
        // We didn't finish, and the reason we're unblocking is because a
        // JS-NATIVE wants to run.  It wants us to run the body text of the
        // JavaScript here on the MAIN, because that's where it is useful.
        //
        REBACT *phase = FRM_PHASE(FS_TOP);
        assert(ACT_DISPATCHER(phase) == &JavaScript_Dispatcher);

        REBARR *details = ACT_DETAILS(phase);
        bool is_awaiter = VAL_LOGIC(ARR_AT(details, IDX_JS_NATIVE_IS_AWAITER));

        TRACE("rebIdle() => Invoke_Js_Body_On_Main()");
        Invoke_Js_Body_On_Main(FS_TOP);

        if (not is_awaiter) {
            //
            // During an await in the pthread model, libRebol routines can be
            // called from the MAIN thread (just not promises and awaiters).
            // So that includes plain JavaScript natives.  But that native
            // is blocked here.  Solve it by requeuing idle for now.
            //
            heapaddr_t frame_id = Frame_Id_For_Frame_May_Outlive_Call(FS_TOP);
            Proxy_Native_Result_To_Worker(frame_id);
        }

        // The awaiter *might* have called resolve() or reject() in its body.
        // we could handle that and loop here, but the average case is that
        // it called JavaScript to do some MAIN work.  Since any resolve() or
        // reject() requeues rebIdle() anyway, have that case handled by the
        // next run of rebIdle() vs. a goto above here.

        result = m_cast(REBVAL*, END_NODE);  // avoid compiler warning
    }
  #endif

    if (
        info->state == PROMISE_STATE_AWAITING
        or info->state == PROMISE_STATE_RUNNING
    ){
        // Not finished
    }
    else {
        if (IS_NULLED(result)) {
            rebRelease(result);
            result = nullptr;
        }

        if (info->state == PROMISE_STATE_RESOLVED) {
            EM_ASM(
                { reb.ResolvePromise_internal($0, $1); },
                info->promise_id,  // => $0 (table entry will be freed)
                result  // => $1 (recipient takes over handle)
            );
        }
        else {
            assert(info->state == PROMISE_STATE_REJECTED);

            assert(IS_FRAME(result));  // Note: Expired, can't use VAL_CONTEXT
            REBNOD *frame_ctx = VAL_NODE(result);
            heapaddr_t throw_id = Heapaddr_From_Pointer(frame_ctx);

            EM_ASM(
                { reb.RejectPromise_internal($0, $1); },
                info->promise_id,  // => $0 (table entry will be freed)
                throw_id  // => $1 (table entry will be freed)
            );
        }

        assert(PG_Promises == info);
        PG_Promises = info->next;
        FREE(struct Reb_Promise_Info, info);
    }

    TRACE("rebIdle() => exiting to generic MAIN processing");
}


// This function receives a frame_id and not the resolved REBVAL* because
// the emterpreter build can't really pass a REBVAL*.  All the APIs it would
// need to make REBVAL* are unavailable.  So it instead pokes an item for
// finding the appropriate JavaScript object where it can be found when no longer in
// emscripten_sleep_with_yield(), then reb.GetAwaiterResult_internal() is used
// to ask for that value later.
//
// The pthreads build *could* take a value and poke it into the promise info.
// But it's not worth it to wire up two different protocols on the JavaScript
// side.  It should be rethought if someday the emterpreter version is axed.
//
EMSCRIPTEN_KEEPALIVE EXTERN_C
void RL_rebAwaiterResolve_internal(intptr_t frame_id, int rejected) {
    ASSERT_ON_MAIN_THREAD();

    struct Reb_Promise_Info *info = PG_Promises;
    assert(info->state == PROMISE_STATE_AWAITING);

    TRACE("reb.SignalAwaiter_internal() => signaling resolve");

    // If we resolved the awaiter, we didn't resolve the overall promise,
    // but just pushed it back into the running state.
    //
    info->state = PROMISE_STATE_RUNNING;

  #if defined(USE_PTHREADS)
    Proxy_Native_Result_To_Worker(frame_id);
  #endif

    // !!! "The above only signaled.  The awaiter will get the actual
    // result with reb.GetAwaiterResult_internal()."  Still true?
}


EMSCRIPTEN_KEEPALIVE EXTERN_C
void RL_rebAwaiterReject_internal(intptr_t frame_id) {
    ASSERT_ON_MAIN_THREAD();

    struct Reb_Promise_Info *info = PG_Promises;
    assert(info->state == PROMISE_STATE_AWAITING);

    TRACE("reb.SignalAwaiter_internal() => signaling reject");

    // !!! Rejecting an awaiter basically means the promise as a whole
    // failed, I'd assume?
    //
    info->state = PROMISE_STATE_REJECTED;

  #if defined(USE_PTHREADS)
    //
    // This signal is happening during the .catch() clause of the internal
    // routine that runs natives.  But it happens after it is no longer
    // on the stack, e.g.
    //
    //     async function js_awaiter_impl() { throw 1020; }
    //     function js_awaiter_invoker() {
    //         js_awaiter_impl().catch(function() {
    //              console.log("prints second")  // we're here now
    //         })
    //         console.log("prints first")  // fell through to GUI pump
    //     }
    //
    // So the js_awaiter_invoker() is not on the stack, this is an async
    // resolution even if the throw was called directly like that.
    //
    // In the long term it may be possible for Rebol constructs like
    // TRAP or CATCH to intercept a JavaScript-thrown error.  If they
    // did they may ask for more work to be done on the GUI so it would
    // need to be in idle for that (otherwise the next thing it ran
    // could always be assumed as the result to the await).
    //
    // But if the Rebol construct could catch a JS throw, it would need
    // to convert it somehow to a Rebol value.  That conversion would
    // have to be done right now--or there'd have to be some specific
    // protocol for coming back and requesting it.
    //
    // But what we have to do is unblock the JS-AWAITER that's running
    // with a throw so it can finish.  We do not want to do the promise
    // rejection until it is.  We make that thrown value the frame so
    // we can get the ID back out of it (and so it doesn't GC, so the
    // lifetime lasts long enough to not conflate IDs in the table).
    //
    // Note: We don't want to fall through to the main thread's message
    // pump so long as any code is running on the worker that's using Rebol
    // features.  A stray setTimeout() message might get processed while
    // the R_THROW is being unwound, and use a Rebol API which would
    // be contentious with running code on another thread.  Block, and
    // it should be unblocked to let the catch() clause run.
    //
    // We *could* do mutex management here and finish up the signal
    // sequence.  But we can't on the emterpreted build, because it has
    // to unwind that asm.js stack safely, so we could only call the
    // reject here for pthread.  Pipe everything through idle so both
    // emterpreter and not run the reject on GUI from the same stack.
    //
    EM_ASM(
        { setTimeout(function() { _RL_rebIdle_internal(); }, 0); }
    );  // note `_RL` (leading underscore means no cwrap)
  #endif
}


//
//  JavaScript_Dispatcher: C
//
// Called when the ACTION! produced by JS-NATIVE is run.  The tricky bit is
// that it doesn't actually return to the caller when the body of the JS code
// is done running...it has to wait for either the `resolve` or `reject`
// parameter functions to get called.
//
// An AWAITER can only be called inside a rebPromise().  And it needs its
// body to run on the MAIN thread.
//
REB_R JavaScript_Dispatcher(REBFRM *f)
{
    heapaddr_t frame_id = Frame_Id_For_Frame_May_Outlive_Call(f);

    REBARR *details = ACT_DETAILS(FRM_PHASE(f));
    bool is_awaiter = VAL_LOGIC(ARR_AT(details, IDX_JS_NATIVE_IS_AWAITER));

    TRACE("JavaScript_Dispatcher(%s)", Frame_Label_Or_Anonymous_UTF8(f));

    struct Reb_Promise_Info *info = PG_Promises;
    if (is_awaiter) {
        if (info == nullptr)
            fail ("JavaScript /AWAITER can only be called from rebPromise()");
        if (info->state != PROMISE_STATE_RUNNING)
            fail ("Cannot call JavaScript /AWAITER during another await");
    }

    REBVAL *result;

  #if defined(USE_EMTERPRETER)  // on MAIN thread (by definition)

    Invoke_Js_Body_On_Main(f);

    // We don't know exactly what MAIN event is going to trigger and cause a
    // resolve() to happen.  It could be a timer, it could be a fetch(),
    // it could be anything.  The emterpreted build doesn't really have a
    // choice other than to poll...there's nothing like pthread wait
    // conditions available.  We wait at least 50msec (probably more, as
    // we don't control how long the MAIN will be running whatever it does).
    //
    TRACE("JavaScript_Dispatcher() => begin sleep_with_yield() loop");
    while (info and info->state == PROMISE_STATE_AWAITING) {  // !!! volatile?
        if (Eval_Signals & SIG_HALT) {
            //
            // !!! TBD: How to handle halts?  We're spinning here, so the
            // MAIN should theoretically have a chance to write some cancel.
            // Is it a reject()?
            //
            Eval_Signals &= ~SIG_HALT; // don't preserve state once observed
        }
        emscripten_sleep_with_yield(50); // no resolve(), no reject() calls...
    }
    TRACE("JavaScript_Dispatcher() => end sleep_with_yield() loop");

    if (not info or info->state == PROMISE_STATE_RUNNING)
        result = Get_Native_Result(frame_id);
    else {
        assert(info->state == PROMISE_STATE_REJECTED);
        result = m_cast(REBVAL*, END_NODE);
    }
  #else
    // If we're already on the MAIN thread, then we're just calling a JS
    // service routine with no need to yield.
    //
    if (pthread_equal(pthread_self(), PG_Main_Thread)) {
        assert(not is_awaiter);
        Invoke_Js_Body_On_Main(f);
        result = Get_Native_Result(frame_id);
    }
    else {
        // We are not using the emterpreter, so we have to block our return
        // on a condition, while signaling the MAIN that it can go ahead and
        // run.  The MAIN has to actually run the JS code.

        assert(IS_POINTER_END_DEBUG(PG_Native_Result));

        // We are going to be signaled when a resolve() or reject() happens,
        // but might miss the signal if there weren't coordination that the
        // signal would only be given when we were waiting.
        //
        pthread_mutex_lock(&PG_Promise_Mutex);

        TRACE("JavaScript_Dispatcher() => signaling MAIN wakeup");
        pthread_mutex_lock(&PG_Main_Mutex);
        pthread_cond_signal(&PG_Main_Cond);
        pthread_mutex_unlock(&PG_Main_Mutex);

        // Block until the MAIN says that what we're awaiting on is resolved
        // (or rejected...TBD)
        //
        TRACE("JavaScript_Dispatcher() => suspending for native result");
        pthread_cond_wait(&PG_Promise_Cond, &PG_Promise_Mutex);
        pthread_mutex_unlock(&PG_Promise_Mutex);
        TRACE("JavaScript_Dispatcher() => native result was signaled");

        result = PG_Native_Result;
        ENDIFY_POINTER_IF_DEBUG(PG_Native_Result);

        // MAIN should be locked again at this point
    }
  #endif

    // See notes on frame_id above for how the context's heap address is used
    // to identify the thrown JavaScript object in a mapping table, so that
    // when this throw reaches the top it can be supplied to the caller.
    // (We don't want to unsafely EM_ASM() throw from within a Rebol call in
    // a way that breaks asm.js execution and bypasses the cleanup needed)
    //
    if (info and info->state == PROMISE_STATE_REJECTED) {
        assert(IS_END(result));

        // !!! Testing at the moment to see if a throw can at least not cause
        // a crash (?)  The throw currently goes up and gets caught by
        // someone, can we see what happens?

        info->state = PROMISE_STATE_RUNNING;

        return Init_Throw_For_Frame_Id(f->out, frame_id);
    }

    assert(not info or info->state == PROMISE_STATE_RUNNING);  // reject?
    return result;
}


//
//  export js-native-mainthread: native [
//
//  {Create ACTION! from textual JavaScript code, works only on main thread}
//
//      return: [action!]
//      spec "Function specification (similar to the one used by FUNCTION)"
//          [block!]
//      source "JavaScript code as a text string" [text!]
//      /awaiter "implicit resolve()/reject() parameters signal return result"
//  ]
//
REBNATIVE(js_native_mainthread)
//
// !!! As a temporary workaround to make sure all JS-NATIVEs get registered
// on the main thread, the JS-NATIVE itself runs JavaScript which then calls
// this function.  It thus very inefficiently reuses the code for proxying
// natives to the main thread.  But it runs through the right code path
// and shows what mechanism we should reuse (just in a better way).
//
// Note: specialized as JS-AWAITER in %ext-javascript-init.reb
{
    JAVASCRIPT_INCLUDE_PARAMS_OF_JS_NATIVE_MAINTHREAD;

    // The use of the worker is for running Rebol code, when that code may
    // need to hold its state in suspended animation while JavaScript runs.
    // But the JavaScript *always* runs on the main/GUI thread.  So the
    // table matching integers to code lives only on the GUI thread (and
    // due to the rules of JavaScript web workers, is not visible to the
    // worker thread).
    //
    ASSERT_ON_MAIN_THREAD();

    REBVAL *spec = ARG(spec);
    REBVAL *source = ARG(source);

    REBARR *paramlist = Make_Paramlist_Managed_May_Fail(spec, MKF_MASK_NONE);
    REBACT *native = Make_Action(
        paramlist,
        &JavaScript_Dispatcher,
        nullptr,  // no underlying action (use paramlist)
        nullptr,  // no specialization exemplar (or inherited exemplar)
        IDX_JS_NATIVE_MAX  // details len [source module handle]
    );

    heapaddr_t native_id = Native_Id_For_Action(native);

    REBARR *details = ACT_DETAILS(native);

    if (Is_Series_Frozen(VAL_SERIES(source)))
        Move_Value(ARR_AT(details, IDX_NATIVE_BODY), source);  // no copy
    else {
        Init_Text(
            ARR_AT(details, IDX_NATIVE_BODY),
            Copy_String_At_Len(source, -1)  // might change
        );
    }

    // !!! A bit wasteful to use a whole cell for this--could just be whether
    // the ID is positive or negative.  Keep things clear, optimize later.
    //
    Init_Logic(ARR_AT(details, IDX_JS_NATIVE_IS_AWAITER), REF(awaiter));

    // The generation of the function called by JavaScript.  It takes no
    // arguments, as giving it arguments would make calling it more complex
    // as well as introduce several issues regarding mapping legal Rebol
    // names to names for JavaScript parameters.  libRebol APIs must be used
    // to access the arguments out of the frame.

    DECLARE_MOLD (mo);
    Push_Mold(mo);

    Append_Unencoded(mo->series, "reb.RegisterId_internal(");

    REBYTE buf[60];  // !!! Why 60?  Copied from MF_Integer()
    REBINT len = Emit_Integer(buf, native_id);
    Append_Unencoded_Len(mo->series, s_cast(buf), len);

    // By not using `new function` we avoid escaping of the string literal.
    // We also have the option of making it an async function in the future
    // if we wanted to...which would allow `await` in the body.
    //
    Append_Unencoded(mo->series, ", ");

    // A JS-AWAITER can only be triggered from Rebol on the worker thread as
    // part of a rebPromise().  The function itself has no return value, as
    // it does its work by either resolve() or reject().  Making it an async
    // function means it will return an ES6 Promise, and allows use of the
    // AWAIT JavaScript feature inside the body:
    //
    // https://javascript.info/async-await
    //
    // Using plain return inside an async function returns a fulfilled promise
    // while using AWAIT causes the execution to pause and return a pending
    // promise.  When that promise is fulfilled it will jump back in and
    // pick up code on the line after that AWAIT.
    //
    if (REF(awaiter))
        Append_Unencoded(mo->series, "async ");

    // We do not try to auto-translate the Rebol arguments into JS args.  It
    // would make calling it more complex, and introduce several issues of
    // mapping Rebol names to legal JavaScript identifiers.  reb.Arg() or
    // reb.ArgR() must be used to access the arguments out of the frame.
    //
    Append_Unencoded(mo->series, "function () {");

    REBSIZ offset;
    REBSIZ size;
    REBSER *temp = Temp_UTF8_At_Managed(
        &offset,
        &size,
        source,
        VAL_LEN_AT(source)
    );
    Append_Utf8_Utf8(mo->series, cs_cast(BIN_AT(temp, offset)), size);

    Append_Unencoded(mo->series, "}\n");  // end `function() {`
    Append_Unencoded(mo->series, ");");  // end `reb.RegisterId_internal(`

    TERM_SERIES(mo->series);

    TRACE("Registering native_id %d", native_id);
    emscripten_run_script(cs_cast(BIN_AT(mo->series, mo->start)));

    Drop_Mold(mo);

    // !!! Natives on the stack can specify where APIs like reb.Run() should
    // look for bindings.  For the moment, set user natives to use the user
    // context...it could be a parameter of some kind (?)
    //
    Move_Value(
        ARR_AT(details, IDX_NATIVE_CONTEXT),
        Get_System(SYS_CONTEXTS, CTX_USER)
    );

    Init_Handle_Managed(
        ARR_AT(details, IDX_JS_NATIVE_HANDLE),
        ACT_PARAMLIST(native),
        0,
        &cleanup_js_native
    );

    TERM_ARRAY_LEN(details, IDX_JS_NATIVE_MAX);
    SET_ACTION_FLAG(native, IS_NATIVE);

    return Init_Action_Unbound(D_OUT, native);
}


//
//  export init-javascript-extension: native [
//
//  {Initialize the JavaScript Extension}
//
//      return: <void>
//  ]
//
REBNATIVE(init_javascript_extension)
{
    JAVASCRIPT_INCLUDE_PARAMS_OF_INIT_JAVASCRIPT_EXTENSION;

    TRACE("INIT-JAVASCRIPT-EXTENSION called");

    PG_Probe_Failures = PG_JS_Trace = true;

  #if defined(USE_PTHREADS)
    int ret = 0;

    PG_Main_Thread = pthread_self();  // remember for debug checks
    ret |= pthread_mutex_init(&PG_Main_Mutex, nullptr);
    ret |= pthread_cond_init(&PG_Main_Cond, nullptr);

    ret |= pthread_create(
        &PG_Promise_Thread,
        nullptr,  // pthread attributes (optional)
        &promise_worker,
        m_cast(REBVAL*, END_NODE)  // unused arg (reads global state directly)
    );
    ret |= pthread_mutex_init(&PG_Promise_Mutex, nullptr);
    ret |= pthread_cond_init(&PG_Promise_Cond, nullptr);

    if (ret != 0)
        fail ("non-zero pthread API result in INIT-JAVASCRIPT-EXTENSION");

    ENDIFY_POINTER_IF_DEBUG(PG_Promise_Result);
    ENDIFY_POINTER_IF_DEBUG(PG_Native_Result);
  #endif

    return Init_Void(D_OUT);
}


//
//  export js-trace: native [
//
//  {Internal debug tool for seeing what's going on in JavaScript dispatch}
//
//      return: <void>
//      enable [logic!]
//  ]
//
REBNATIVE(js_trace)
{
    JAVASCRIPT_INCLUDE_PARAMS_OF_JS_TRACE;

  #ifdef DEBUG_JAVASCRIPT_EXTENSION
    PG_Probe_Failures = PG_JS_Trace = VAL_LOGIC(ARG(enable));
  #else
    fail ("JS-TRACE only if DEBUG_JAVASCRIPT_EXTENSION set in %emscripten.r");
  #endif

    return Init_Void(D_OUT);
}


// !!! Need shutdown, but there's currently no module shutdown
//
// https://forum.rebol.info/t/960
