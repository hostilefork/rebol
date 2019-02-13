//
//  File: %a-lib.c
//  Summary: "Lightweight Export API (REBVAL as opaque type)"
//  Section: environment
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Rebol Open Source Contributors
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
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// This is the "external" API, and %rebol.h contains its exported
// definitions.  That file (and %make-reb-lib.r which generates it) contains
// comments and notes which will help understand it.
//
// What characterizes the external API is that it is not necessary to #include
// the extensive definitions of `struct REBSER` or the APIs for dealing with
// all the internal details (e.g. PUSH_GC_GUARD(), which are easy to get
// wrong).  Not only does this simplify the interface, but it also means that
// the C code using the library isn't competing as much for definitions in
// the global namespace.
//
// Also, due to the nature of REBNOD (see %sys-node.h), it's possible to feed
// the scanner with a list of pointers that may be to UTF-8 strings or to
// Rebol values.  The behavior is to "splice" in the values at the point in
// the scan that they occur, e.g.
//
//     REBVAL *item1 = ...;
//     REBVAL *item2 = ...;
//     REBVAL *item3 = ...;
//
//     REBARR *result = rebRun(
//         "if not", item1, "[\n",
//             item2, "| print {Close brace separate from content}\n",
//         "] else [\n",
//             item3, "| print {Close brace with content}]\n",
//         rebEND
//     );
//
// (The rebEND is needed by the variadic processing, but C99-based macros or
// other language bindings can inject it automatically...only C89 has no way
// to work around it.)
//
// While the approach is flexible, any token must appear fully inside its
// UTF-8 string component.  So you can't--for instance--divide a scan up like
// ("{abc", "def", "ghi}") and get the TEXT! {abcdefghi}.  On that note,
// ("a", "/", "b") produces `a / b` and not the PATH! `a/b`.
//
//==//// NOTES ////////////////////////////////////////////////////////////=//
//
// Each exported routine here has a name RL_rebXxxYyy.  This is a name by
// which it can be called internally from the codebase like any other function
// that is part of the core.  However, macros for calling it from the core
// are given as `#define rebXxxYyy RL_rebXxxYyy`.  This is a little bit nicer
// and consistent with the way it looks when an external client calls the
// functions.
//
// Then extension clients use macros which have you call the functions through
// a struct-based "interface" (similar to the way that interfaces work in
// something like COM).  Here the macros merely pick the API functions through
// a table, e.g. `#define rebXxxYyy interface_struct->rebXxxYyy`.  This means
// paying a slight performance penalty to dereference that API per call, but
// it keeps API clients from depending on the conventional C linker...so that
// DLLs can be "linked" against a Rebol EXE.
//
// (It is not generically possible to export symbols from an executable, and
// just in general there's no cross-platform assurances about how linking
// works, so this provides the most flexibility.)
//

#include "sys-core.h"


// "Linkage back to HOST functions. Needed when we compile as a DLL
// in order to use the OS_* macro functions."
//
#ifdef REB_API  // Included by C command line
    const REBOL_HOST_LIB *Host_Lib = nullptr;
    EXTERN_C REBOL_HOST_LIB Host_Lib_Init;
#endif


//
// rebEnterApi_Internal: RL_API
//
// This stub is added automatically to the calling wrappers.
//
// !!! Review how much checking one wants to do when calling API routines,
// and what the balance should be of debug vs. release.  Right now, this helps
// in particular notice if the core tries to use an API function before the
// proper moment in the boot.
//
void RL_rebEnterApi_internal(void) {
    if (not Host_Lib)
        panic ("rebStartup() not called before API call");
}


//=//// SERIES-BACKED ALLOCATORS //////////////////////////////////////////=//
//
// These are replacements for malloc(), realloc(), and free() which use a
// byte-sized REBSER as the backing store for the data.
//
// One benefit of using a series is that it offers more options for automatic
// memory management (such as being freed in case of a fail(), vs. leaked as
// a malloc() would, or perhaps being GC'd when a particular FRAME! ends).
//
// It also has the benefit of helping interface with client code that has
// been stylized to use malloc()-ish hooks to produce data, when the eventual
// target of that data is a Rebol series.  It does this without exposing
// REBSER* internals to the external API, by allowing one to "rebRepossess()"
// the underlying series as a BINARY! REBVAL*.
//


//
//  rebMalloc: RL_API
//
// * Unlike plain malloc(), this will fail() instead of return null if an
//   allocation cannot be fulfilled.
//
// * Like plain malloc(), if size is zero, the implementation just has to
//   return something that free() will take.  A backing series is added in
//   this case vs. returning null, in order to avoid null handling in other
//   routines (e.g. rebRepossess() or handle lifetime control functions).
//
// * Because of the above points, null is *never* returned.
//
// * It tries to be like malloc() by giving back a pointer "suitably aligned
//   for the size of any fundamental type".  See notes on ALIGN_SIZE.
//
// !!! rebAlignedMalloc() could exist to take an alignment, which could save
// on wasted bytes when ALIGN_SIZE > sizeof(REBSER*)...or work with "weird"
// large fundamental types that need more alignment than ALIGN_SIZE.
//
void *RL_rebMalloc(size_t size)
{
    REBSER *s = Make_Series_Core(
        ALIGN_SIZE // stores REBSER* (must be at least big enough for void*)
            + size // for the actual data capacity (may be 0...see notes)
            + 1, // for termination (even BINARY! has this, review necessity)
        sizeof(REBYTE), // rebRepossess() only creates binary series ATM
        SERIES_FLAG_DONT_RELOCATE // direct data pointer is being handed back!
            | SERIES_FLAG_ALWAYS_DYNAMIC // rebRepossess() needs bias field
    );

    REBYTE *ptr = BIN_HEAD(s) + ALIGN_SIZE;

    REBSER **ps = (cast(REBSER**, ptr) - 1);
    *ps = s; // save self in bytes *right before* data
    POISON_MEMORY(ps, sizeof(REBSER*)); // let ASAN catch underruns

    // !!! The data is uninitialized, and if it is turned into a BINARY! via
    // rebRepossess() before all bytes are assigned initialized, it could be
    // worse than just random data...MOLDing such a binary and reading those
    // bytes could be bad (due to, for instance, "trap representations"):
    //
    // https://stackoverflow.com/a/37184840
    //
    // It may be that rebMalloc() and rebRealloc() should initialize with 0
    // to defend against that, but that isn't free.  For now we make no such
    // promise--and leave it uninitialized so that address sanitizer notices
    // when bytes are used that haven't been assigned.
    //
    TERM_BIN_LEN(s, ALIGN_SIZE + size);

    return ptr;
}


//
//  rebRealloc: RL_API
//
// * Like plain realloc(), null is legal for ptr (despite the fact that
//   rebMalloc() never returns null, this can still be useful)
//
// * Like plain realloc(), it preserves the lesser of the old data range or
//   the new data range, and memory usage drops if new_size is smaller:
//
// https://stackoverflow.com/a/9575348
//
// * Unlike plain realloc() (but like rebMalloc()), this fails instead of
//   returning null, hence it is safe to say `ptr = rebRealloc(ptr, new_size)`
//
// * A 0 size is considered illegal.  This is consistent with the C11 standard
//   for realloc(), but not with malloc() or rebMalloc()...which allow it.
//
void *RL_rebRealloc(void *ptr, size_t new_size)
{
    assert(new_size > 0); // realloc() deprecated this as of C11 DR 400

    if (not ptr) // C realloc() accepts null
        return rebMalloc(new_size);

    REBSER **ps = cast(REBSER**, ptr) - 1;
    UNPOISON_MEMORY(ps, sizeof(REBSER*)); // need to underrun to fetch `s`

    REBSER *s = *ps;

    REBCNT old_size = BIN_LEN(s) - ALIGN_SIZE;

    // !!! It's less efficient to create a new series with another call to
    // rebMalloc(), but simpler for the time being.  Switch to do this with
    // the same series node.
    //
    void *reallocated = rebMalloc(new_size);
    memcpy(reallocated, ptr, old_size < new_size ? old_size : new_size);
    Free_Unmanaged_Series(s);

    return reallocated;
}


//
//  rebFree: RL_API
//
// * As with free(), null is accepted as a no-op.
//
void RL_rebFree(void *ptr)
{
    if (not ptr)
        return;

    REBSER **ps = cast(REBSER**, ptr) - 1;
    UNPOISON_MEMORY(ps, sizeof(REBSER*)); // need to underrun to fetch `s`

    REBSER *s = *ps;
    if (s->header.bits & NODE_FLAG_CELL) {
        rebJumps(
            "PANIC [",
                "{rebFree() mismatched with allocator!}"
                "{Did you mean to use free() instead of rebFree()?}",
            "]",
            rebEND
        );
    }

    assert(BYTE_SIZE(s));

    Free_Unmanaged_Series(s);
}


//
//  rebRepossess: RL_API
//
// Alternative to rebFree() is to take over the underlying series as a
// BINARY!.  The old void* should not be used after the transition, as this
// operation makes the series underlying the memory subject to relocation.
//
// If the passed in size is less than the size with which the series was
// allocated, the overage will be treated as unused series capacity.
//
// Note that all rebRepossess()'d data will be terminated by an 0x00 byte
// after the end of its capacity.
//
// !!! All bytes in the allocation are expected to be initialized by this
// point, as failure to do so will mean reads crash the interpreter.  See
// remarks in rebMalloc() about the issue, and possibly doing zero fills.
//
// !!! It might seem tempting to use (BIN_LEN(s) - ALIGN_SIZE).  However,
// some routines make allocations bigger than they ultimately need and do not
// realloc() before converting the memory to a series...rebInflate() and
// rebDeflate() do this.  So a version passing the size will be necessary,
// and since C does not have the size exposed in malloc() and you track it
// yourself, it seems fair to *always* ask the caller to pass in a size.
//
REBVAL *RL_rebRepossess(void *ptr, size_t size)
{
    REBSER **ps = cast(REBSER**, ptr) - 1;
    UNPOISON_MEMORY(ps, sizeof(REBSER*)); // need to underrun to fetch `s`

    REBSER *s = *ps;
    assert(NOT_SERIES_FLAG(s, MANAGED));

    if (size > BIN_LEN(s) - ALIGN_SIZE)
        fail ("Attempt to rebRepossess() more than rebMalloc() capacity");

    assert(GET_SERIES_FLAG(s, DONT_RELOCATE));
    CLEAR_SERIES_FLAG(s, DONT_RELOCATE);

    if (IS_SER_DYNAMIC(s)) {
        //
        // Dynamic series have the concept of a "bias", which is unused
        // allocated capacity at the head of a series.  Bump the "bias" to
        // treat the embedded REBSER* (aligned to REBI64) as unused capacity.
        //
        SER_SET_BIAS(s, ALIGN_SIZE);
        s->content.dynamic.data += ALIGN_SIZE;
        s->content.dynamic.rest -= ALIGN_SIZE;
    }
    else {
        // Data is in REBSER node itself, no bias.  Just slide the bytes down.
        //
        memmove( // src overlaps destination, can't use memcpy()
            BIN_HEAD(s),
            BIN_HEAD(s) + ALIGN_SIZE,
            size
        );
    }

    TERM_BIN_LEN(s, size);
    return Init_Binary(Alloc_Value(), s);
}



//
//  Startup_Api: C
//
// RL_API routines may be used by extensions (which are invoked by a fully
// initialized Rebol core) or by normal linkage (such as from within the core
// itself).  A call to rebStartup() won't be needed in the former case.  So
// setup code that is needed to interact with the API needs to be done by the
// core independently.
//
void Startup_Api(void)
{
}


//
//  Shutdown_Api: C
//
// See remarks on Startup_Api() for the difference between this idea and
// rebShutdown.
//
void Shutdown_Api(void)
{
    assert(Host_Lib);
    Host_Lib = nullptr;
}


//
//  rebStartup: RL_API
//
// This function will allocate and initialize all memory structures used by
// the REBOL interpreter. This is an extensive process that takes time.
//
// `lib` is the host lib table (OS_XXX functions) which Rebol core does not
// take for granted--and assumes a host must provide to operate.  An example
// of this would be that getting the current UTC date and time varies from OS
// to OS, so for the NOW native to be implemented it has to call something
// outside of standard C...e.g. OS_GET_TIME().  So even though NOW is in the
// core, it will be incomplete without having that function supplied.
//
// !!! Increased modularization of the core, and new approaches, are making
// this concept obsolete.  For instance, the NOW native might not even live
// in the core, but be supplied by a "Timer Extension" which is considered to
// be sandboxed and non-core enough that having platform-specific code in it
// is not a problem.  Also, hooks can be supplied in the form of natives that
// are later HIJACK'd by some hosts (see PANIC and FAIL), as a way of
// injecting richer platform-or-scenario-specific code into a more limited
// default host operation.  It is expected that the OS_XXX functions will
// eventually disappear completely.
//
void RL_rebStartup(void)
{
    if (Host_Lib)
        panic ("rebStartup() called when it's already started");

    Host_Lib = &Host_Lib_Init;

    if (Host_Lib->size < HOST_LIB_SIZE)
        panic ("Host-lib wrong size");

    if (((HOST_LIB_VER << 16) + HOST_LIB_SUM) != Host_Lib->ver_sum)
        panic ("Host-lib wrong version/checksum");

    Startup_Core();
}


//
//  rebShutdown: RL_API
//
// Shut down a Rebol interpreter initialized with rebStartup().
//
// The `clean` parameter tells whether you want Rebol to release all of its
// memory accrued since initialization.  If you pass false, then it will
// only do the minimum needed for data integrity (it assumes you are planning
// to exit the process, and hence the OS will automatically reclaim all
// memory/handles/etc.)
//
// For rigor, the debug build *always* runs a "clean" shutdown.
//
void RL_rebShutdown(bool clean)
{
    // At time of writing, nothing Shutdown_Core() does pertains to
    // committing unfinished data to disk.  So really there is
    // nothing to do in the case of an "unclean" shutdown...yet.

  #if defined(NDEBUG)
    if (not clean)
        return; // Only do the work above this line in an unclean shutdown
  #else
    UNUSED(clean);
  #endif

    // Run a clean shutdown anyway in debug builds--even if the
    // caller didn't need it--to see if it triggers any alerts.
    //
    Shutdown_Core();
}


//
//  rebTick: RL_API
//
// If the executable is built with tick counting, this will return the tick
// without requiring any Rebol code to run (which would disrupt the tick).
//
long RL_rebTick(void)
{
  #ifdef DEBUG_COUNT_TICKS
    return cast(long, TG_Tick);
  #else
    return 0;
  #endif
}


//
//  rebArgR: RL_API
//
// This is the version of getting an argument that does not require a release.
// However, it is more optimal than `rebR(rebArg(...))`, because how it works
// is by returning the actual REBVAL* to the argument in the frame.  It's not
// good to have client code having those as handles--however--as they do not
// follow the normal rules for lifetime, so rebArg() should be used if the
// client really requires a REBVAL*.
//
// !!! When code is being used to look up arguments of a function, exactly
// how that will work is being considered:
//
// https://forum.rebol.info/t/817
// https://forum.rebol.info/t/820
//
// For the moment, this routine specifically accesses arguments of the most
// recent ACTION! on the stack.
//
const void *RL_rebArgR(const void *p, va_list *vaptr)
{
    REBFRM *f = FS_TOP;
    REBACT *act = FRM_PHASE(f);

    // !!! Currently the JavaScript wrappers do not do the right thing for
    // taking just a `const char*`, so this falsely is a variadic to get the
    // JavaScript string proxying.
    //
    const char *name = cast(const char*, p);
    const void *p2 = va_arg(*vaptr, const void*);
    if (Detect_Rebol_Pointer(p2) != DETECTED_AS_END)
        fail ("rebArg() isn't actually variadic, it's arity-1");

    REBSTR *spelling = Intern_UTF8_Managed(
        cb_cast(name),
        LEN_BYTES(cb_cast(name))
    );

    REBVAL *param = ACT_PARAMS_HEAD(act);
    REBVAL *arg = FRM_ARGS_HEAD(f);
    for (; NOT_END(param); ++param, ++arg) {
        if (SAME_STR(VAL_PARAM_SPELLING(param), spelling))
            return arg;
    }

    fail ("Unknown rebArg(...) name.");
}


//
//  rebArg: RL_API
//
// Wrapper over the more optimal rebArgR() call, which can be used to get
// an "safer" API handle to the argument.
//
REBVAL *RL_rebArg(const void *p, va_list *vaptr)
{
    const void* argR = RL_rebArgR(p, vaptr);
    if (not argR)
        return nullptr;

    REBVAL *arg = cast(REBVAL*, c_cast(void*, argR)); // sneaky, but we know!
    return Move_Value(Alloc_Value(), arg); // don't give REBVAL* arg directly
}


//
//  rebRun: RL_API
//
// C variadic function which calls the evaluator on multiple pointers.
// Each pointer may either be a REBVAL* or a UTF-8 string which will be
// scanned to reflect one or more values in the sequence.
//
// All REBVAL* are spliced in quoted by default.  Use rebEVAL or rebU():
//
// https://forum.rebol.info/t/1050
//
REBVAL *RL_rebRun(const void *p, va_list *vaptr)
{
    REBVAL *result = Alloc_Value();
    if (Do_Va_Throws(result, p, vaptr))  // will ensure va_end() is called
        fail (Error_No_Catch_For_Throw(result));  // implicit result release

    if (not IS_NULLED(result))
        return result;

    rebRelease(result);
    return nullptr;  // No NULLED cells in API, see notes on NULLIFY_NULLED()
}


//
//  rebQuote: RL_API
//
// Variant of rebRun() that simply quotes its result.  So `rebQuote(...)` is
// equivalent to `rebRun("quote", ...)`, with the advantage of being faster
// and not depending on what the QUOTE word looks up to.
//
// (It also has the advantage of not showing QUOTE on the call stack.  That
// is important for the console when trapping its generated result, to be
// able to quote it without the backtrace showing a QUOTE stack frame.)
//
REBVAL *RL_rebQuote(const void *p, va_list *vaptr)
{
    REBVAL *result = Alloc_Value();
    if (Do_Va_Throws(result, p, vaptr))  // will ensure va_end() is called
        fail (Error_No_Catch_For_Throw(result));  // implicit result release

    return Quotify(result, 1);
}


//
//  rebUNEVALUATIVE: RL_API
//
// https://forum.rebol.info/t/1050
//
// !!! It may be possible to create variations of this which are done in a
// way that would allow arbitrary spans, `rebU("[, value1), value2, "]")`.
// But those variants would have to be more sophisticated than this.
//
const void *RL_rebUNEVALUATIVE(const void *p, va_list *vaptr)
{
    REBFLGS feed_flags = FEED_MASK_DEFAULT | FEED_FLAG_UNEVALUATIVE;
    DECLARE_VA_FEED (feed, p, vaptr, feed_flags);

    // Feed through all the values to the stack, unevaluated

    REBDSP dsp_orig = DSP;

    while (NOT_END(feed->value)) {
        Move_Value(DS_PUSH(), KNOWN(feed->value));
        Fetch_Next_In_Feed(feed, false);
    }

    if (dsp_orig == DSP)
        fail ("No values in rebUNEVALUATIVE()");

    if (dsp_orig > DSP + 1)
        fail ("Multiple values in rebUNEVALUATIVE(), not implemented");

    assert(not IS_NULLED(DS_TOP));  // UNEVALUATIVE should fail on nulls

    REBVAL *result = Move_Value(Alloc_Value(), DS_TOP);
    DS_DROP();

    REBARR *a = Singular_From_Cell(result);
    SET_ARRAY_FLAG(a, SINGULAR_API_RELEASE);
    SET_ARRAY_FLAG(a, SINGULAR_API_UNEVALUATIVE);
    return a;
}


//
//  rebElide: RL_API
//
// Variant of rebRun() which assumes you don't need the result.  This saves on
// allocating an API handle, or the caller needing to manage its lifetime.
//
void RL_rebElide(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (elided);
    if (Do_Va_Throws(elided, p, vaptr)) // calls va_end()
        fail (Error_No_Catch_For_Throw(elided));
}


//
//  rebJumps: RL_API [
//      #noreturn
//  ]
//
// rebJumps() is like rebElide, but has the noreturn attribute.  This helps
// inform the compiler that the routine is not expected to return.  Use it
// with things like `rebJumps("FAIL", ...)` or `rebJumps("THROW", ...)`.  If
// by some chance the code passed to it does not jump and finishes normally,
// then an error will be raised.
//
// (Note: Capitalizing the "FAIL" or other non-returning operation is just a
// suggestion to help emphasize the operation.  Capitalizing rebJUMPS was
// considered, but looked odd.)
//
// !!! The name is not ideal, but other possibilites aren't great:
//
//    rebDeadEnd(...) -- doesn't sound like it should take arguments
//    rebNoReturn(...) -- whose return?
//    rebStop(...) -- STOP is rather final sounding, the code keeps going
//
void RL_rebJumps(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (elided);
    if (Do_Va_Throws(elided, p, vaptr)) { // calls va_end()
        //
        // !!! Being able to THROW across C stacks is necessary in the general
        // case (consider implementing QUIT or HALT).  Probably need to be
        // converted to a kind of error, and then re-converted into a THROW
        // to bubble up through Rebol stacks?  Development on this is ongoing.
        //
        fail (Error_No_Catch_For_Throw(elided));
    }

    fail ("rebJumps() was used to run code, but it didn't FAIL/QUIT/THROW!");
}


//
//  rebEVAL_internal: RL_API
//
// (Note: "_internal" is so that macro can be rebEVAL with no parentheses.)
//
// Optimized stand in for the EVAL function, useful for triggering actions
// or picking the quotes off of things.
//
// It is not intended for use inside blocks, e.g.:
//
//    REBVAL *block = rebRun("[", rebEVAL, word, "]");
//
// Plan is that this will either raise an error in the variadic scanner,
// or possibly put the native EVAL ACTION! in that spot.  Instead, use the
// rebUNEVALUATIVE() mechanism:
//
//    REBVAL *block = rebRun("[", rebU(word), "]");
//    REBVAL *block = rebRun(rebU("[", word, "]"));  // only 1 scan item ATM
//
const void *RL_rebEVAL_internal(void)
{
    REBARR *instruction = Alloc_Instruction(API_OPCODE_EVAL);
    return instruction;
}


//
//  rebRELEASING: RL_API
//
// Convenience tool for making "auto-release" form of values.  They will only
// exist for one API call.  They will be automatically rebRelease()'d when
// they are seen (or even if they are not seen, if there is a failure on that
// call it will still process the va_list in order to release these handles)
//
const void *RL_rebRELEASING(REBVAL *v)
{
    if (not Is_Api_Value(v))
        fail ("Cannot apply rebR() to non-API value");

    REBARR *a = Singular_From_Cell(v);
    if (GET_ARRAY_FLAG(a, SINGULAR_API_RELEASE))
        fail ("Cannot apply rebR() more than once to the same API value");

    SET_ARRAY_FLAG(a, SINGULAR_API_RELEASE);
    return a; // returned as const void* to discourage use outside variadics
}


//
//  rebVoid: RL_API
//
REBVAL *RL_rebVoid(void)
{
    return Init_Void(Alloc_Value());
}


//
//  rebBlank: RL_API
//
REBVAL *RL_rebBlank(void)
{
    return Init_Blank(Alloc_Value());
}


//
//  rebLogic: RL_API
//
// !!! Use of bool in this file assumes compatibility between C99 stdbool and
// C++ stdbool.  Are they compatible?
//
// "There doesn't seem to be any guarantee that C int is compatible with C++
//  int. 'Linkage from C++ to objects defined in other languages and to
//  objects defined in C++ from other languages is implementation-defined
//  (...) I'd expect C and C++ compilers intended to be used together (such as
//  gcc and g++) to make their bool and int types, among others, compatible."
// https://stackoverflow.com/q/3529831
//
// Take this for granted, then assume that a shim `bool` that is -not- part of
// stdbool would be defined to be the same size as C++'s bool (if such a
// pre-C99 system could even be used to make a C++ build at all!)
//
REBVAL *RL_rebLogic(bool logic)
{
    // Use DID on the bool, in case it's a "shim bool" (e.g. just some integer
    // type) and hence may have values other than strictly 0 or 1.
    //
    return Init_Logic(Alloc_Value(), did logic);
}


//
//  rebChar: RL_API
//
REBVAL *RL_rebChar(uint32_t codepoint)
{
    if (codepoint > MAX_UNI)
        fail ("Codepoint out of range, see: https://forum.rebol.info/t/374");

    return Init_Char(Alloc_Value(), codepoint);
}


//
//  rebInteger: RL_API
//
// !!! Should there be rebSigned() and rebUnsigned(), in order to catch cases
// of using out of range values?
//
REBVAL *RL_rebInteger(int64_t i)
{
    return Init_Integer(Alloc_Value(), i);
}


//
//  rebDecimal: RL_API
//
REBVAL *RL_rebDecimal(double dec)
{
    return Init_Decimal(Alloc_Value(), dec);
}


//
//  rebHalt: RL_API
//
// Signal that code evaluation needs to be interrupted.
//
// Returns:
//     nothing
// Notes:
//     This function sets a signal that is checked during evaluation
//     and will cause the interpreter to begin processing an escape
//     trap. Note that control must be passed back to REBOL for the
//     signal to be recognized and handled.
//
void RL_rebHalt(void)
{
    SET_SIGNAL(SIG_HALT);
}


//
//  rebRescue: RL_API
//
// This API abstracts the mechanics by which exception-handling is done.
// While code that knows specifically which form is used can take advantage of
// that knowledge and use the appropriate mechanism without this API, any
// code (such as core code) that wants to be agnostic to mechanism should
// use rebRescue() instead.
//
// There are three current mechanisms which can be built with.  One is to
// use setjmp()/longjmp(), which is extremely dodgy.  But it's what R3-Alpha
// used, and it's the only choice if one is sticking to ANSI C89-99:
//
// https://en.wikipedia.org/wiki/Setjmp.h#Exception_handling
//
// If one is willing to compile as C++ -and- link in the necessary support
// for exception handling, there are benefits to doing exception handling
// with throw/catch.  One advantage is performance: most compilers can avoid
// paying for catch blocks unless a throw occurs ("zero-cost exceptions"):
//
// https://stackoverflow.com/q/15464891/ (description of the phenomenon)
// https://stackoverflow.com/q/38878999/ (note that it needs linker support)
//
// It also means that C++ API clients can use try/catch blocks without needing
// the rebRescue() abstraction, as well as have destructors run safely.
// (longjmp pulls the rug out from under execution, and doesn't stack unwind).
//
// The other abstraction is for JavaScript, where an emscripten build would
// have to painstakingly emulate setjmp/longjmp.  Using inline JavaScript to
// catch and throw is more efficient, and also provides the benefit of API
// clients being able to use normal try/catch of a RebolError instead of
// having to go through rebRescue().
//
// But using rebRescue() internally allows the core to be compiled and run
// compatibly across all these scenarios.  It is named after Ruby's operation,
// which deals with the identical problem:
//
// http://silverhammermba.github.io/emberb/c/#rescue
//
// !!! As a first step, this only implements the setjmp/longjmp logic.
//
REBVAL *RL_rebRescue(
    REBDNG *dangerous, // !!! pure C function only if not using throw/catch!
    void *opaque
){
    struct Reb_State state;
    REBCTX *error_ctx;

    PUSH_TRAP(&error_ctx, &state);

    // We want allocations that occur in the body of the C function for the
    // rebRescue() to be automatically cleaned up in the case of an error.
    //
    // !!! This is currently done by knowing what frame an error occurred in
    // and marking any allocations while that frame was in effect as being
    // okay to "leak" (in the sense of leaking to be GC'd).  So we have to
    // make a dummy frame here, and unfortunately the frame must be reified
    // so it has to be an "action frame".  Improve mechanic later, but for
    // now pretend to be applying a dummy native.
    //
    DECLARE_END_FRAME (f, EVAL_MASK_DEFAULT);  // not FULLY_SPECIALIZED
    Push_Frame(nullptr, f);

    REBSTR *opt_label = NULL;

    Push_Action(f, PG_Dummy_Action, UNBOUND);
    Begin_Action(f, opt_label);
    assert(IS_END(f->arg));
    f->param = END_NODE; // signal all arguments gathered
    assert(f->refine == ORDINARY_ARG); // Begin_Action() sets
    f->arg = m_cast(REBVAL*, END_NODE);
    f->special = END_NODE;

    // The first time through the following code 'error' will be null, but...
    // `fail` can longjmp here, so 'error' won't be null *if* that happens!
    //
    if (error_ctx) {
        assert(f->varlist); // action must be running
        REBARR *stub = f->varlist; // will be stubbed, with info bits reset
        Drop_Action(f);
        SET_SERIES_FLAG(stub, VARLIST_FRAME_FAILED); // signal API leaks ok
        Abort_Frame(f);
        return Init_Error(Alloc_Value(), error_ctx);
    }

    REBVAL *result = (*dangerous)(opaque);

    Drop_Action(f);

    // !!! To abstract how the system deals with exception handling, the
    // rebRescue() routine started being used in lieu of PUSH_TRAP/DROP_TRAP
    // internally to the system.  Some of these system routines accumulate
    // stack state, so Drop_Frame_Unbalanced() must be used.
    //
    Drop_Frame_Unbalanced(f);

    DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&state);

    if (not result)
        return nullptr; // null is considered a legal result

    // Analogous to how TRAP works, if you don't have a handler for the
    // error case then you can't return an ERROR!, since all errors indicate
    // a failure.  Use KIND_BYTE() since R_THROWN or other special things can
    // be used internally, and literal errors don't count either.
    //
    if (KIND_BYTE(result) == REB_ERROR) {
        if (Is_Api_Value(result))
            rebRelease(result);
        return rebVoid();
    }

    if (not Is_Api_Value(result))
        return result; // no proxying needed

    assert(not IS_NULLED(result)); // leaked API nulled cell (not nullptr)

    // !!! We automatically proxy the ownership of any managed handles to the
    // caller.  Any other handles that leak out (e.g. via state) will not be
    // covered by this, and would have to be unmanaged.  Do another allocation
    // just for the sake of it.

    REBVAL *proxy = Move_Value(Alloc_Value(), result); // parent is not f
    rebRelease(result);
    return proxy;
}


//
//  rebRescueWith: RL_API
//
// Variant of rebRescue() with a handler hook (parallels TRAP/WITH, except
// for C code as the protected code and the handler).  More similar to
// Ruby's rescue2 operation.
//
REBVAL *RL_rebRescueWith(
    REBDNG *dangerous, // !!! pure C function only if not using throw/catch!
    REBRSC *rescuer, // errors in the rescuer function will *not* be caught
    void *opaque
){
    struct Reb_State state;
    REBCTX *error_ctx;

    PUSH_TRAP(&error_ctx, &state);

    // The first time through the following code 'error' will be null, but...
    // `fail` can longjmp here, so 'error' won't be null *if* that happens!
    //
    if (error_ctx) {
        REBVAL *error = Init_Error(Alloc_Value(), error_ctx);

        REBVAL *result = (*rescuer)(error, opaque); // *not* guarded by trap!

        rebRelease(error);
        return result; // no special handling, may be null
    }

    REBVAL *result = (*dangerous)(opaque); // guarded by trap
    assert(not IS_NULLED(result)); // nulled cells not exposed by API

    DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&state);

    return result; // no special handling, may be NULL
}


//
//  rebDid: RL_API
//
bool RL_rebDid(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (condition);
    if (Do_Va_Throws(condition, p, vaptr)) // calls va_end()
        fail (Error_No_Catch_For_Throw(condition));

    return IS_TRUTHY(condition); // will fail() on voids
}


//
//  rebNot: RL_API
//
// !!! If this were going to be a macro like (not (rebDid(...))) it would have
// to be a variadic macro.  Just make a separate entry point for now.
//
bool RL_rebNot(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (condition);
    if (Do_Va_Throws(condition, p, vaptr)) // calls va_end()
        fail (Error_No_Catch_For_Throw(condition));

    return IS_FALSEY(condition); // will fail() on voids
}


//
//  rebUnbox: RL_API
//
// C++, JavaScript, and other languages can do some amount of intelligence
// with a generic `rebUnbox()` operation...either picking the type to return
// based on the target in static typing, or returning a dynamically typed
// value.  For convenience in C, make the generic unbox operation return
// an integer for INTEGER!, LOGIC!, CHAR!...assume it's most common so the
// short name is worth it.
//
long RL_rebUnbox(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (result);
    if (Do_Va_Throws(result, p, vaptr))
        fail (Error_No_Catch_For_Throw(result));

    switch (VAL_TYPE(result)) {
      case REB_INTEGER:
        return VAL_INT64(result);

      case REB_CHAR:
        return VAL_CHAR(result);

      case REB_LOGIC:
        return VAL_LOGIC(result) ? 1 : 0;

      default:
        fail ("C-based rebUnbox() only supports INTEGER!, CHAR!, and LOGIC!");
    }
}


//
//  rebUnboxInteger: RL_API
//
long RL_rebUnboxInteger(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (result);
    if (Do_Va_Throws(result, p, vaptr))
        fail (Error_No_Catch_For_Throw(result));

    if (VAL_TYPE(result) != REB_INTEGER)
        fail ("rebUnboxInteger() called on non-INTEGER!");

    return VAL_INT64(result);
}


//
//  rebUnboxDecimal: RL_API
//
double RL_rebUnboxDecimal(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (result);
    if (Do_Va_Throws(result, p, vaptr))
        fail (Error_No_Catch_For_Throw(result));

    if (VAL_TYPE(result) == REB_DECIMAL)
        return VAL_DECIMAL(result);

    if (VAL_TYPE(result) == REB_INTEGER)
        return cast(double, VAL_INT64(result));

    fail ("rebUnboxDecimal() called on non-DECIMAL! or non-INTEGER!");
}


//
//  rebUnboxChar: RL_API
//
uint32_t RL_rebUnboxChar(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (result);
    if (Do_Va_Throws(result, p, vaptr))
        fail (Error_No_Catch_For_Throw(result));

    if (VAL_TYPE(result) != REB_CHAR)
        fail ("rebUnboxChar() called on non-CHAR!");

    return VAL_CHAR(result);
}


//
//  rebHandle: RL_API
//
// !!! The HANDLE! type has some complexity to it, because function pointers
// in C and C++ are not actually guaranteed to be the same size as data
// pointers.  Also, there is an optional size stored in the handle, and a
// cleanup function the GC may call when references to the handle are gone.
//
REBVAL *RL_rebHandle(void *data, size_t length, CLEANUP_CFUNC *cleaner)
{
    return Init_Handle_Managed(Alloc_Value(), data, length, cleaner);
}


//
//  rebSpellInto: RL_API
//
// Extract UTF-8 data from an ANY-STRING! or ANY-WORD!.
//
// API does not return the number of UTF-8 characters for a value, because
// the answer to that is always cached for any value position as LENGTH OF.
// The more immediate quantity of concern to return is the number of bytes.
//
size_t RL_rebSpellInto(
    char *buf,
    size_t buf_size, // number of bytes
    const REBVAL *v
){
    const char *utf8;
    REBSIZ utf8_size;
    if (ANY_STRING(v)) {
        REBSIZ offset;
        REBSER *temp = Temp_UTF8_At_Managed(
            &offset, &utf8_size, v, VAL_LEN_AT(v)
        );
        utf8 = cs_cast(BIN_AT(temp, offset));
    }
    else {
        assert(ANY_WORD(v));

        REBSTR *spelling = VAL_WORD_SPELLING(v);
        utf8 = STR_HEAD(spelling);
        utf8_size = STR_SIZE(spelling);
    }

    if (not buf) {
        assert(buf_size == 0);
        return utf8_size; // caller must allocate a buffer of size + 1
    }

    REBSIZ limit = MIN(buf_size, utf8_size);
    memcpy(buf, utf8, limit);
    buf[limit] = '\0';
    return utf8_size;
}


//
//  rebSpell: RL_API
//
// This gives the spelling as UTF-8 bytes.  Length in codepoints should be
// extracted with LENGTH OF.  If size in bytes of the encoded UTF-8 is needed,
// use the binary extraction API (works on ANY-STRING! to get UTF-8)
//
char *RL_rebSpell(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (string);
    if (Do_Va_Throws(string, p, vaptr)) // calls va_end()
        fail (Error_No_Catch_For_Throw(string));

    if (IS_NULLED(string))
        return nullptr; // NULL is passed through, for opting out

    size_t size = rebSpellInto(nullptr, 0, string);
    char *result = cast(char*, rebMalloc(size + 1)); // add space for term
    rebSpellInto(result, size, string);
    return result;
}


//
//  rebSpellIntoW: RL_API
//
// Extract UCS-2 data from an ANY-STRING! or ANY-WORD!.  Note this is *not*
// UTF-16, so codepoints that require more than two bytes to represent will
// cause errors.
//
// !!! Although the rebSpellInto API deals in bytes, this deals in count of
// characters.  (The use of REBCNT instead of REBSIZ indicates this.)  It may
// be more useful for the wide string APIs to do this so leaving it that way
// for now.
//
unsigned int RL_rebSpellIntoW(
    REBWCHAR *buf,
    unsigned int buf_chars, // chars buf can hold (not including terminator)
    const REBVAL *v
){
    REBSER *s;
    REBCNT index;
    REBCNT len;
    if (ANY_STRING(v)) {
        s = VAL_SERIES(v);
        index = VAL_INDEX(v);
        len = VAL_LEN_AT(v);
    }
    else {
        assert(ANY_WORD(v));

        REBSTR *spelling = VAL_WORD_SPELLING(v);
        s = Make_Sized_String_UTF8(STR_HEAD(spelling), STR_SIZE(spelling));
        index = 0;
        len = SER_LEN(s);
    }

    if (not buf) { // querying for size
        assert(buf_chars == 0);
        if (ANY_WORD(v))
            Free_Unmanaged_Series(s);
        return len; // caller must now allocate buffer of len + 1
    }

    REBCNT limit = MIN(buf_chars, len);
    REBCNT n = 0;
    for (; index < limit; ++n, ++index)
        buf[n] = GET_ANY_CHAR(s, index);

    buf[limit] = 0;

    if (ANY_WORD(v))
        Free_Unmanaged_Series(s);
    return len;
}


//
//  rebSpellW: RL_API
//
// Gives the spelling as WCHARs.  If length in codepoints is needed, use
// a separate LENGTH OF call.
//
// !!! Unlike with rebSpell(), there is not an alternative for getting
// the size in UTF-16-encoded characters, just the LENGTH OF result.  While
// that works for UCS-2 (where all codepoints are two bytes), it would not
// work if Rebol supported UTF-16.  Which it may never do in the core or
// API (possible solutions could include usermode UTF-16 conversion to binary,
// and extraction of that with rebBytes(), then dividing the size by 2).
//
REBWCHAR *RL_rebSpellW(const void *p, va_list *vaptr)
{
    DECLARE_LOCAL (string);
    if (Do_Va_Throws(string, p, vaptr)) // calls va_end()
        fail (Error_No_Catch_For_Throw(string));

    if (IS_NULLED(string))
        return nullptr; // NULL is passed through, for opting out

    REBCNT len = rebSpellIntoW(nullptr, 0, string);
    REBWCHAR *result = cast(
        REBWCHAR*, rebMalloc(sizeof(REBWCHAR) * (len + 1))
    );
    rebSpellIntoW(result, len, string);
    return result;
}


//
//  rebBytesInto: RL_API
//
// Extract binary data from a BINARY!
//
// !!! Caller must allocate a buffer of the returned size + 1.  It's not clear
// if this is a good idea; but this is based on a longstanding convention of
// zero termination of Rebol series, including binaries.  Review.
//
size_t RL_rebBytesInto(
    unsigned char *buf,
    size_t buf_size,
    const REBVAL *binary
){
    if (not IS_BINARY(binary))
        fail ("rebBytesInto() only works on BINARY!");

    REBCNT size = VAL_LEN_AT(binary);

    if (not buf) {
        assert(buf_size == 0);
        return size; // currently, caller must allocate a buffer of size + 1
    }

    REBCNT limit = MIN(buf_size, size);
    memcpy(s_cast(buf), cs_cast(VAL_BIN_AT(binary)), limit);
    buf[limit] = '\0';
    return size;
}


//
//  rebBytes: RL_API
//
// Can be used to get the bytes of a BINARY! and its size, or the UTF-8
// encoding of an ANY-STRING! or ANY-WORD! and that size in bytes.  (Hence,
// for strings it is like rebSpell() except telling you how many bytes.)
//
// !!! This may wind up being a generic TO BINARY! converter, so you might
// be able to get the byte conversion for any type.
//
unsigned char *RL_rebBytes(
    size_t *size_out, // !!! Enforce non-null, to ensure type safety?
    const void *p, va_list *vaptr
){
    DECLARE_LOCAL (series);
    if (Do_Va_Throws(series, p, vaptr)) // calls va_end()
        fail (Error_No_Catch_For_Throw(series));

    if (IS_NULLED(series)) {
        *size_out = 0;
        return nullptr; // NULL is passed through, for opting out
    }

    if (ANY_WORD(series) or ANY_STRING(series)) {
        *size_out = rebSpellInto(nullptr, 0, series);
        char *result = rebAllocN(char, (*size_out + 1));
        size_t check = rebSpellInto(result, *size_out, series);
        assert(check == *size_out);
        UNUSED(check);
        return cast(unsigned char*, result);
    }

    if (IS_BINARY(series)) {
        *size_out = rebBytesInto(nullptr, 0, series);
        unsigned char *result = rebAllocN(REBYTE, (*size_out + 1));
        size_t check = rebBytesInto(result, *size_out, series);
        assert(check == *size_out);
        UNUSED(check);
        return result;
    }

    fail ("rebBytes() only works with ANY-STRING!/ANY-WORD!/BINARY!");
}


//
//  rebBinary: RL_API
//
REBVAL *RL_rebBinary(const void *bytes, size_t size)
{
    REBSER *bin = Make_Binary(size);
    memcpy(BIN_HEAD(bin), bytes, size);
    TERM_BIN_LEN(bin, size);

    return Init_Binary(Alloc_Value(), bin);
}


//
//  rebSizedText: RL_API
//
// If utf8 does not contain valid UTF-8 data, this may fail().
//
REBVAL *RL_rebSizedText(const char *utf8, size_t size)
{
    return Init_Text(Alloc_Value(), Make_Sized_String_UTF8(utf8, size));
}


//
//  rebText: RL_API
//
REBVAL *RL_rebText(const char *utf8)
{
    return rebSizedText(utf8, strsize(utf8));
}


//
//  rebLengthedTextW: RL_API
//
REBVAL *RL_rebLengthedTextW(const REBWCHAR *wstr, unsigned int num_chars)
{
    DECLARE_MOLD (mo);
    Push_Mold(mo);

    for (; num_chars != 0; --num_chars, ++wstr)
        Append_Utf8_Codepoint(mo->series, *wstr);

    return Init_Text(Alloc_Value(), Pop_Molded_String(mo));
}


//
//  rebTextW: RL_API
//
REBVAL *RL_rebTextW(const REBWCHAR *wstr)
{
    DECLARE_MOLD (mo);
    Push_Mold(mo);

    for (; *wstr != 0; ++wstr)
        Append_Utf8_Codepoint(mo->series, *wstr);

    return Init_Text(Alloc_Value(), Pop_Molded_String(mo));
}


//
//  rebManage: RL_API
//
// The "friendliest" default for the API is to assume you want handles to be
// tied to the lifetime of the frame they're in.  Long-running top-level
// processes like the C code running the console would eventually exhaust
// memory if that were the case...so there should be some options for metrics
// as a form of "leak detection" even so.
//
REBVAL *RL_rebManage(REBVAL *v)
{
    assert(Is_Api_Value(v));

    REBARR *a = Singular_From_Cell(v);
    assert(GET_SERIES_FLAG(a, ROOT));

    if (GET_SERIES_FLAG(a, MANAGED))
        fail ("Attempt to rebManage() a handle that's already managed.");

    SET_SERIES_FLAG(a, MANAGED);
    assert(not LINK(a).owner);
    LINK(a).owner = NOD(Context_For_Frame_May_Manage(FS_TOP));

    return v;
}


//
//  rebUnmanage: RL_API
//
// This converts an API handle value to indefinite lifetime.
//
void RL_rebUnmanage(void *p)
{
    REBNOD *nod = NOD(p);
    if (not (nod->header.bits & NODE_FLAG_CELL))
        fail ("rebUnmanage() not yet implemented for rebMalloc() data");

    REBVAL *v = cast(REBVAL*, nod);
    assert(Is_Api_Value(v));

    REBARR *a = Singular_From_Cell(v);
    assert(GET_SERIES_FLAG(a, ROOT));

    if (NOT_SERIES_FLAG(a, MANAGED))
        fail ("Attempt to rebUnmanage() a handle with indefinite lifetime.");

    // It's not safe to convert the average series that might be referred to
    // from managed to unmanaged, because you don't know how many references
    // might be in cells.  But the singular array holding API handles has
    // pointers to its cell being held by client C code only.  It's at their
    // own risk to do this, and not use those pointers after a free.
    //
    CLEAR_SERIES_FLAG(a, MANAGED);
    assert(GET_ARRAY_FLAG(LINK(a).owner, IS_VARLIST));
    LINK(a).owner = UNBOUND;
}


//
//  rebRelease: RL_API
//
// An API handle is only 4 platform pointers in size (plus some bookkeeping),
// but it still takes up some storage.  The intended default for API handles
// is that they live as long as the function frame they belong to, but there
// will be several lifetime management tricks to ease releasing them.
//
// !!! For the time being, we lean heavily on explicit release.  Near term
// leak avoidance will need to at least allow for GC of handles across errors
// for their associated frames.
//
void RL_rebRelease(const REBVAL *v)
{
    if (not v)
        return; // less rigorous, but makes life easier for C programmers

    if (not Is_Api_Value(v))
        panic ("Attempt to rebRelease() a non-API handle");

    Free_Value(m_cast(REBVAL*, v));
}


//
//  rebDeflateAlloc: RL_API
//
// Exposure of the deflate() of the built-in zlib.  Assumes no envelope.
//
// Uses zlib's recommended default for compression level.
//
// See rebRepossess() for the ability to mutate the result into a BINARY!
//
void *RL_rebDeflateAlloc(
    size_t *out_len,
    const void *input,
    size_t in_len
){
    REBSTR *envelope = Canon(SYM_NONE);
    return Compress_Alloc_Core(out_len, input, in_len, envelope);
}


//
//  rebZdeflateAlloc: RL_API
//
// Variant of rebDeflateAlloc() which adds a zlib envelope...which is a 2-byte
// header and 32-bit ADLER32 CRC at the tail.
//
void *RL_rebZdeflateAlloc(
    size_t *out_len,
    const void *input,
    size_t in_len
){
    REBSTR *envelope = Canon(SYM_ZLIB);
    return Compress_Alloc_Core(out_len, input, in_len, envelope);
}


//
//  rebGzipAlloc: RL_API
//
// Slight variant of deflate() which stores the uncompressed data's size
// implicitly in the returned data, and a CRC32 checksum.
//
void *RL_rebGzipAlloc(
    size_t *out_len,
    const void *input,
    size_t in_len
){
    REBSTR *envelope = nullptr; // see notes in Gunzip on why GZIP is default
    return Compress_Alloc_Core(out_len, input, in_len, envelope);
}


//
//  rebInflateAlloc: RL_API
//
// Exposure of the inflate() of the built-in zlib.  Assumes no envelope.
//
// Use max = -1 to guess decompressed size, or for best memory efficiency,
// specify `max` as the precise size of the original data.
//
// See rebRepossess() for the ability to mutate the result into a BINARY!
//
void *RL_rebInflateAlloc(
    size_t *len_out,
    const void *input,
    size_t len_in,
    int max
){
    REBSTR *envelope = Canon(SYM_NONE);
    return Decompress_Alloc_Core(len_out, input, len_in, max, envelope);
}


//
//  rebZinflateAlloc: RL_API
//
// Variant of rebInflateAlloc() which assumes a zlib envelope...checking for
// the 2-byte header and verifying the 32-bit ADLER32 CRC at the tail.
//
void *RL_rebZinflateAlloc(
    size_t *len_out,
    const void *input,
    size_t len_in,
    int max
){
    REBSTR *envelope = Canon(SYM_ZLIB);
    return Decompress_Alloc_Core(len_out, input, len_in, max, envelope);
}


//
//  rebGunzipAlloc: RL_API
//
// Slight variant of inflate() which is compatible with gzip, and checks its
// CRC32.  For data whose original size was < 2^32 bytes, the gzip envelope
// stored that size...so memory efficiency is achieved even if max = -1.
//
// Note: That size guarantee exists for data compressed with rebGzipAlloc() or
// adhering to the gzip standard.  However, archives created with the GNU
// gzip tool make streams with possible trailing zeros or concatenations:
//
// http://stackoverflow.com/a/9213826
//
void *RL_rebGunzipAlloc(
    size_t *len_out,
    const void *input,
    size_t len_in,
    int max
){
    // Note: Because GZIP is what Rebol uses for booting, `nullptr` means
    // use GZIP.  That's because symbols in %words.r haven't been loaded yet,
    // so a call to Canon(SYM_XXX) would fail.
    //
    REBSTR *envelope = nullptr; // GZIP is the default
    return Decompress_Alloc_Core(len_out, input, len_in, max, envelope);
}


//
//  rebDeflateDetectAlloc: RL_API
//
// Does DEFLATE with detection, and also ignores the size information in a
// gzip file, due to the reasoning here:
//
// http://stackoverflow.com/a/9213826
//
void *RL_rebDeflateDetectAlloc(
    size_t *len_out,
    const void *input,
    size_t len_in,
    int max
){
    REBSTR *envelope = Canon(SYM_DETECT);
    return Decompress_Alloc_Core(len_out, input, len_in, max, envelope);
}


// !!! Although it is very much the goal to get all OS-specific code out of
// the core (including the API), this particular hook is extremely useful to
// have available to all clients.  It might be done another way (e.g. by
// having hosts HIJACK the FAIL native with an adaptation that processes
// integer arguments).  But for now, stick it in the API just to get the
// wide availability.
//
#ifdef TO_WINDOWS
    #undef IS_ERROR // windows has its own meaning for this.
    #include <windows.h>
#else
    #include <errno.h>
    #define MAX_POSIX_ERROR_LEN 1024
#endif

//
//  rebFail_OS: RL_API [
//      #noreturn
//  ]
//
// Produce an error from an OS error code, by asking the OS for textual
// information it knows internally from its database of error strings.
//
// This function is called via a macro which adds DEAD_END; after it.
//
// Note that error codes coming from WSAGetLastError are the same as codes
// coming from GetLastError in 32-bit and above Windows:
//
// https://stackoverflow.com/q/15586224/
//
// !!! Should not be in core, but extensions need a way to trigger the
// common functionality one way or another.
//
void RL_rebFail_OS(int errnum)
{
    REBCTX *error;

  #ifdef TO_WINDOWS
    if (errnum == 0)
        errnum = GetLastError();

    WCHAR *lpMsgBuf; // FormatMessage writes allocated buffer address here

    // Specific errors have %1 %2 slots, and if you know the error ID and
    // that it's one of those then this lets you pass arguments to fill
    // those in.  But since this is a generic error, we have no more
    // parameterization (hence FORMAT_MESSAGE_IGNORE_INSERTS)
    //
    va_list *Arguments = nullptr;

    // Apparently FormatMessage can find its error strings in a variety of
    // DLLs, but we don't have any context here so just use the default.
    //
    LPCVOID lpSource = nullptr;

    DWORD ok = FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER // see lpMsgBuf
            | FORMAT_MESSAGE_FROM_SYSTEM // e.g. ignore lpSource
            | FORMAT_MESSAGE_IGNORE_INSERTS, // see Arguments
        lpSource,
        errnum, // message identifier
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // default language
        cast(WCHAR*, &lpMsgBuf), // allocated buffer address written here
        0, // buffer size (not used since FORMAT_MESSAGE_ALLOCATE_BUFFER)
        Arguments
    );

    if (ok == 0) {
        //
        // Might want to show the value of GetLastError() in this message,
        // but trying to FormatMessage() on *that* would be excessive.
        //
        error = Error_User("FormatMessage() gave no error description");
    }
    else {
        REBVAL *message = rebTextW(lpMsgBuf);
        LocalFree(lpMsgBuf);

        error = Error(SYM_0, SYM_0, message, END_NODE);
    }
  #else
    // strerror() is not thread-safe, but strerror_r is. Unfortunately, at
    // least in glibc, there are two different protocols for strerror_r(),
    // depending on whether you are using the POSIX-compliant implementation
    // or the GNU implementation.
    //
    // The convoluted test below is the inversion of the actual test glibc
    // suggests to discern the version of strerror_r() provided. As other,
    // non-glibc implementations (such as OS X's libSystem) also provide the
    // POSIX-compliant version, we invert the test: explicitly use the
    // older GNU implementation when we are sure about it, and use the
    // more modern POSIX-compliant version otherwise. Finally, we only
    // attempt this feature detection when using glibc (__GNU_LIBRARY__),
    // as this particular combination of the (more widely standardised)
    // _POSIX_C_SOURCE and _XOPEN_SOURCE defines might mean something
    // completely different on non-glibc implementations.
    //
    // (Note that undefined pre-processor names arithmetically compare as 0,
    // which is used in the original glibc test; we are more explicit.)

    #ifdef USE_STRERROR_NOT_STRERROR_R
        char *shared = strerror(errnum);
        error = Error_User(shared);
    #elif defined(__GNU_LIBRARY__) \
            && (defined(_GNU_SOURCE) \
                || ((!defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200112L) \
                    && (!defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < 600)))

        // May return an immutable string instead of filling the buffer

        char buffer[MAX_POSIX_ERROR_LEN];
        char *maybe_str = strerror_r(errnum, buffer, MAX_POSIX_ERROR_LEN);
        if (maybe_str != buffer)
            strncpy(buffer, maybe_str, MAX_POSIX_ERROR_LEN);
        error = Error_User(buffer);
    #else
        // Quoting glibc's strerror_r manpage: "The XSI-compliant strerror_r()
        // function returns 0 on success. On error, a (positive) error number
        // is returned (since glibc 2.13), or -1 is returned and errno is set
        // to indicate the error (glibc versions before 2.13)."

        char buffer[MAX_POSIX_ERROR_LEN];
        int result = strerror_r(errnum, buffer, MAX_POSIX_ERROR_LEN);

        // Alert us to any problems in a debug build.
        assert(result == 0);

        if (result == 0)
            error = Error_User(buffer);
        else if (result == EINVAL)
            error = Error_User("EINVAL: bad errno passed to strerror_r()");
        else if (result == ERANGE)
            error = Error_User("ERANGE: insufficient buffer size for error");
        else
            error = Error_User("Unknown problem with strerror_r() message");
    #endif
  #endif

    DECLARE_LOCAL (temp);
    Init_Error(temp, error);
    rebJumps("fail", temp, rebEND);
}


//=//// TRANSITIONAL TOOLS FOR REBREQ/DEVICE MIGRATION ////////////////////=//
//
// !!! To do I/O, R3-Alpha had the concept of "simple" devices, which would
// represent abstractions of system services (Dev_Net would abstract the
// network layer, Dev_File the filesystem, etc.)
//
// There were a fixed list of commands these devices would handle (OPEN,
// CONNECT, READ, WRITE, CLOSE, QUERY).  Further parameterization was done
// with the fields of a specialized C structure called a REBREQ.
//
// This layer was code solely used by Rebol, and needed access to data
// resident in Rebol types.  For instance: if one is to ask to read from a
// file, it makes sense to use Rebol's FILE!.  And if one is reading into an
// existing BINARY! buffer, it makes sense to give the layer the BINARY!.
// But there was an uneasy situation of saying that these REBREQ could not
// speak in Rebol types, resulting in things like picking pointers out of
// the guts of Rebol cells and invoking unknown interactions with the GC by
// putting them into a C struct.
//
// Ren-C is shifting the idea to where a REBREQ is actually a REBARR, and
// able to hold full values (for starters, a REBSER* containing binary data
// of what used to be in a REBREQ...which is actually how PORT!s held a
// REBREQ in their state previously).  However, the way the device layer was
// written it does not have access to the core API.
//


//
//  rebMake_Rebreq: RL_API
//
// !!! Another transitional tool.
//
REBREQ *RL_rebMake_Rebreq(int device) {
    assert(device < RDI_MAX);

    REBDEV *dev = Devices[device];
    assert(dev != NULL);

    REBREQ *req = Make_Binary(dev->req_size);
    memset(BIN_HEAD(req), 0, dev->req_size);
    TERM_BIN_LEN(req, dev->req_size);

    SET_SERIES_INFO(req, LINK_IS_CUSTOM_NODE);
    LINK(req).custom.node = nullptr;

    SET_SERIES_INFO(req, MISC_IS_CUSTOM_NODE);
    MISC(req).custom.node = nullptr;

    Req(req)->device = device;

    return req;
}


#if defined(NDEBUG)
    #define ASSERT_REBREQ(req) \
        NOOP
#else
    inline static void ASSERT_REBREQ(REBREQ *req) {  // basic sanity check
        assert(BYTE_SIZE(req) and BIN_LEN(req) >= sizeof(struct rebol_devreq));
        assert(GET_SERIES_INFO(req, LINK_IS_CUSTOM_NODE));
        assert(GET_SERIES_INFO(req, MISC_IS_CUSTOM_NODE));
    }
#endif


//
//  rebReq: RL_API
//
// !!! Transitional - extract content pointer for REBREQ
//
void *RL_rebReq(REBREQ *req) {
    ASSERT_REBREQ(req);
    return BIN_HEAD(req);  // Req() casts this to `struct rebol_devreq*`
}


//
//  rebAddrOfNextReq: RL_API
//
// !!! Transitional - get `next_req` field hidden in REBSER structure LINK().
// Being in this spot (instead of inside the binary content of the request)
// means the chain of requests can be followed by GC.
//
void **RL_rebAddrOfNextReq(REBREQ *req) {
    ASSERT_REBREQ(req);
    return (void**)&LINK(req).custom.node;  // NextReq() dereferences
}


//
//  rebAddrOfReqPortCtx: RL_API
//
// !!! Transitional - get `port_ctx` field hidden in REBSER structure MISC().
// Being in this spot (instead of inside the binary content of the request)
// means the chain of requests can be followed by GC.
//
void **RL_rebAddrOfReqPortCtx(REBREQ *req) {
    ASSERT_REBREQ(req);
    return (void**)&MISC(req).custom.node;  // ReqPortCtx() dereferences
}


//
//  rebEnsure_Req_Managed: RL_API
//
// !!! Transitional - Lifetime management of REBREQ in R3-Alpha was somewhat
// unclear, with them being created sometimes on the stack, and sometimes
// linked into a pending list if a request turned out to be synchronous and
// not need the request to live longer.  To try and design for efficiency,
// Append_Request() currently is the only place that manages the request for
// asynchronous handling...other clients are expected to free.
//
// !!! Some requests get Append_Request()'d multiple times, apparently.
// Review the implications, but just going with making it legal to manage
// something multiple times for now.
//
void RL_rebEnsure_Req_Managed(REBREQ *req) {
    ASSERT_REBREQ(req);
    ENSURE_SERIES_MANAGED(req);
}


//
//  rebFree_Req: RL_API
//
// !!! Another transitional tool - see notes on rebManageReq()
//
void RL_rebFree_Req(REBREQ *req) {
    ASSERT_REBREQ(req);
    Free_Unmanaged_Series(req);
}


// We wish to define a table of the above functions to pass to clients.  To
// save on typing, the declaration of the table is autogenerated as a file we
// can include here.
//
// It doesn't make a lot of sense to expose this table to clients via an API
// that returns it, because that's a chicken-and-the-egg problem.  The reason
// a table is being used in the first place is because extensions can't link
// to an EXE (in a generic way).  So the table is passed to them, in that
// extension's DLL initialization function.
//
// !!! Note: if Rebol is built as a DLL or LIB, the story is different.
//
extern RL_LIB Ext_Lib;
#include "tmp-reb-lib-table.inc" // declares Ext_Lib
