//
//  File: %sys-trap.h
//  Summary: "CPU and Interpreter State Snapshot/Restore"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2020 Revolt Open Source Contributors
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
// Rebol is settled upon a stable and pervasive implementation baseline of
// ANSI-C (C89).  That commitment provides certain advantages.
//
// One of the *disadvantages* is that there is no safe way to do non-local
// jumps with stack unwinding (as in C++).  If you've written some code that
// performs a raw malloc and then wants to "throw" via a `longjmp()`, that
// will leak the malloc.
//
// In order to mitigate the inherent failure of trying to emulate stack
// unwinding via longjmp, the macros in this file provide an abstraction
// layer.  These allow Rebol to clean up after itself for some kinds of
// "dangling" state--such as manually memory managed series that have been
// made with Make_Series() but never passed to either Free_Unmanaged_Series()
// or Manage_Series().  This covers several potential leaks known-to-Rebol,
// but custom interception code is needed for any generalized resource
// that might be leaked in the case of a longjmp().
//
// The triggering of the longjmp() is done via "fail()", and it's important
// to know the distinction between a "fail" and a "throw".  In Rebol
// terminology, a `throw` is a cooperative concept, which does *not* use
// longjmp(), and instead must cleanly pipe the thrown value up through
// the OUT pointer that each function call writes into.  The `throw` will
// climb the stack until somewhere in the backtrace, one of the calls
// chooses to intercept the thrown value instead of pass it on.
//
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * Mixing C++ and C code using longjmp is a recipe for disaster.  The plan
//   is that API primitives like rebRescue() will be able to abstract the
//   mechanism for fail, but for the moment only longjmp is implemented.
//
// * `fail` is a macro, e.g. `#define fail`, in order to capture __FILE__
//   and __LINE__ (in some debugging modes).  This creates a notable conflict
//   with `std::ios::fail()` in C++, which means that if standard library
//   streaming headers are included after %sys-core.h, there will be errors...
//   unless fail is #undef'd.  (It's not a problem when just using %rebol.h)
//


// R3-Alpha set up a separate `jmp_buf` at each point in the stack that wanted
// to be able to do a TRAP.  With stackless Ren-C, only one jmp_buf is needed
// per instance of the Trampoline on the stack.  (The codebase ideally does
// not invoke more than one trampoline to implement its native code, but if
// it is to call out to C code that wishes to use synchronous forms of API
// calls then nested trampolines may occur.)
//
struct Reb_Jump {
    //
    // We put the jmp_buf first, since it has alignment specifiers on Windows
    //
  #ifdef HAS_POSIX_SIGNAL
    sigjmp_buf cpu_state;
  #else
    jmp_buf cpu_state;
  #endif

    struct Reb_Jump *last_jump;

    REBCTX *error;  // longjmp only takes `int`, pointer passed back via this
};


// "Under FreeBSD 5.2.1 and Mac OS X 10.3, setjmp and longjmp save and restore
// the signal mask. Linux 2.4.22 and Solaris 9, however, do not do this.
// FreeBSD and Mac OS X provide the functions _setjmp and _longjmp, which do
// not save and restore the signal mask."
//
// "To allow either form of behavior, POSIX.1 does not specify the effect of
// setjmp and longjmp on signal masks. Instead, two new functions, sigsetjmp
// and siglongjmp, are defined by POSIX.1. These two functions should always
// be used when branching from a signal handler."
//
// Note: longjmp is able to pass a value (though only an integer on 64-bit
// platforms, and not enough to pass a pointer).  This can be used to
// dictate the value setjmp returns in the longjmp case, though the code
// does not currently use that feature.
//
// Also note: with compiler warnings on, it can tell us when values are set
// before the setjmp and then changed before a potential longjmp:
//
//     http://stackoverflow.com/q/7721854/211160
//
// Because of this longjmp/setjmp "clobbering", it's a useful warning to
// have enabled in.  One option for suppressing it would be to mark
// a parameter as 'volatile', but that is implementation-defined.
// It is best to use a new variable if you encounter such a warning.
//
#if defined(__MINGW64__) && (__GNUC__ < 5)
    //
    // 64-bit builds made by MinGW in the 4.x range have an unfortunate bug in
    // the setjmp/longjmp mechanic, which causes hangs for reasons that are
    // seemingly random, like "using -O0 optimizations instead of -O2":
    //
    // https://sourceforge.net/p/mingw-w64/bugs/406/
    //
    // Bending to the bugs of broken compilers is usually not interesting, but
    // the Travis CI cross-platform builds on Linux targeting Windows were set
    // up on this old version--which otherwise is a good test the codebase
    // hasn't picked up dependencies that are too "modern".

    #define SET_JUMP(s) \
        __builtin_setjmp(s)

    #define LONG_JUMP(s,v) \
        __builtin_longjmp((s), (v))

#elif defined(HAS_POSIX_SIGNAL)
    #define SET_JUMP(s) \
        sigsetjmp((s), 1)

    #define LONG_JUMP(s,v) \
        siglongjmp((s), (v))
#else
    #define SET_JUMP(s) \
        setjmp(s)

    #define LONG_JUMP(s,v) \
        longjmp((s), (v))
#endif


// PUSH_TRAP is a construct which is used to catch errors that have been
// triggered by the Fail_Core() function.  This can be triggered by a usage
// of the `fail` pseudo-"keyword" in C code, and in Rebol user code by the
// REBNATIVE(fail).  To call the push, you need a `struct Reb_Jump` to be
// passed which it will write into--which is a black box that clients
// shouldn't inspect.
//
// Jump buffers contain a pointer-to-a-REBCTX which represents an error.
// Using the tricky mechanisms of setjmp/longjmp, there will be a first pass
// of execution where the line of code after the PUSH_TRAP will see the
// `jump->error` pointer as being `nullptr`.  If a trap occurs during code
// before the paired DROP_TRAP happens, then the C state will be magically
// teleported back to the line after the PUSH_TRAP with the error context now
// non-null and usable.
//
// Note: The implementation of this macro was chosen stylistically to
// hide the result of the setjmp call.  That's because you really can't
// put "setjmp" in arbitrary conditions like `setjmp(...) ? x : y`.  That's
// against the rules.  So although the preprocessor abuse below is a bit
// ugly, it helps establish that anyone modifying this code later not be
// able to avoid the truth of the limitation:
//
// http://stackoverflow.com/questions/30416403/
//
// !!! THIS CAN'T BE INLINED due to technical limitations of using setjmp()
// in inline functions (at least in gcc)
//
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=24556
//
// According to the developers, "This is not a bug as if you inline it, the
// place setjmp goes to could be not where you want to goto."
//
#define PUSH_TRAP_SO_FAIL_CAN_JUMP_BACK_HERE(j) \
    do { \
        assert(TG_Jump_List or DSP == 0); \
        (j)->last_jump = TG_Jump_List; \
        TG_Jump_List = (j); \
        if (0 == SET_JUMP((j)->cpu_state))  /* initial setjmp branch */ \
            (j)->error = nullptr;  /* this branch will always be run */ \
        else {  /* the longjmp happened */ \
            Trapped_Helper(j); \
        } \
    } while (0)


// DROP_TRAP_SAME_STACKLEVEL_AS_PUSH has a long and informative name to
// remind you that you must DROP_TRAP from the same scope you PUSH_TRAP
// from.  (So do not call PUSH_TRAP in a function, then return from that
// function and DROP_TRAP at another stack level.)
//
//      "If the function that called setjmp has exited (whether by return
//      or by a different longjmp higher up the stack), the behavior is
//      undefined. In other words, only long jumps up the call stack
//      are allowed."
//
//      http://en.cppreference.com/w/c/program/longjmp
//
inline static void DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(struct Reb_Jump *j) {
    assert(j->error == nullptr);
    TG_Jump_List = j->last_jump;
}


//
// FAIL
//
// The fail() macro implements a form of error which is "trappable" with the
// macros above:
//
//     if (Foo_Type(foo) == BAD_FOO) {
//         fail (Error_Bad_Foo_Operation(...));
//
//         /* this line will never be reached, because it
//            longjmp'd up the stack where execution continues */
//     }
//
// Errors that originate from C code are created via Make_Error, and are
// defined in %errors.r.  These definitions contain a formatted message
// template, showing how the arguments will be displayed in FORMing.
//
// NOTE: It's desired that there be a space in `fail (...)` to make it look
// more "keyword-like" and draw attention to the fact it is a `noreturn` call.
//

#ifdef NDEBUG
    #ifdef DEBUG_PRINTF_FAIL_LOCATIONS  // see remarks in %reb-config.h
        #define fail(error) do { \
            printf("fail() @ %s %d tick =", __FILE__, __LINE__); \
            Fail_Core(error); /* prints the actual tick */ \
        } while (0)
    #else
        #define fail(error) \
            Fail_Core(error)
    #endif
#else
    #ifdef CPLUSPLUS_11
        //
        // We can do a bit more checking in the C++ build, for instance to
        // make sure you don't pass a RELVAL* into fail().  This could also
        // be used by a strict build that wanted to get rid of all the hard
        // coded string fail()s, by triggering a compiler error on them.
        
        template <class T>
        inline static ATTRIBUTE_NO_RETURN void Fail_Core_Cpp(T *p) {
            static_assert(
                std::is_same<T, REBCTX>::value
                or std::is_same<T, const char>::value
                or std::is_same<T, const REBVAL>::value
                or std::is_same<T, REBVAL>::value,
                "fail() works on: REBCTX*, REBVAL*, const char*"
            );
            Fail_Core(p);
        }

        #define fail(error) \
            do { \
                Fail_Core_Cpp(error); \
            } while (0)
    #else
        #define fail(error) \
            do { \
                Fail_Core(error); \
            } while (0)
    #endif
#endif


// Do insertion into single-linked circular list of tasks (if there's a list
// already, the point where it loops back to PG_Tasks has to be found and
// updated to the new PG_Tasks we are inserting as task).
//
inline static void Circularly_Link_Task(REBTSK *task) {
    if (PG_Tasks == nullptr)
        task->next = task;
    else {
        REBTSK *temp = PG_Tasks;
        while (temp->next != PG_Tasks)
            temp = temp->next;
        temp->next = task;

        task->next = PG_Tasks;
    }
    PG_Tasks = task;
}


// Reverse the process in Circularly_Link_Task().  Currently only works for
// the topmost task.
//
inline static void Circularly_Unlink_Task(REBTSK *task) {
    assert(PG_Tasks == task);
    assert(task->plug_frame == nullptr);
    if (PG_Tasks->next == task)
        PG_Tasks = nullptr;
    else {
        REBTSK *temp = PG_Tasks;
        while (temp->next != PG_Tasks)
            temp = temp->next;
        temp->next = PG_Tasks->next;

        PG_Tasks = PG_Tasks->next;
    }
}
