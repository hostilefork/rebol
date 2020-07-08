//
//  File: %d-crash.c
//  Summary: "low level crash output"
//  Section: debug
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
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

#include "sys-core.h"


// Size of crash buffers
#define PANIC_TITLE_BUF_SIZE 80
#define PANIC_BUF_SIZE 512

#ifdef HAVE_EXECINFO_AVAILABLE
    #include <execinfo.h>
    #include <unistd.h>  // STDERR_FILENO
#endif


//
//  Panic_Core: C
//
// Abnormal termination of Revolt.  The debug build is designed to present
// as much diagnostic information as it can on the passed-in pointer, which
// includes where a REBSER* was allocated or freed.  Or if a REBVAL* is
// passed in it tries to say what tick it was initialized on and what series
// it lives in.  If the pointer is a simple UTF-8 string pointer, then that
// is delivered as a message.
//
// This can be triggered via the macros panic() and panic_at(), which are
// unsalvageable situations in the core code.  It can also be triggered by
// the PANIC and PANIC-VALUE natives.  (Since PANIC and PANIC-VALUE may be
// hijacked, this offers hookability for "recoverable" forms of PANIC.)
//
// coverity[+kill]
//
ATTRIBUTE_NO_RETURN void Panic_Core(
    const void *p, // REBSER* (array, context, etc), REBVAL*, or UTF-8 char*
    REBTCK tick,
    const char *file, // UTF8
    int line
){
    GC_Disabled = true;  // crashing is a legitimate reason to disable the GC

  #if defined(NDEBUG)
    UNUSED(tick);
    UNUSED(file);
    UNUSED(line);
  #else
    printf("C Source File %s, Line %d, Pointer %p\n", file, line, p);
    printf("At evaluator tick: %lu\n", cast(unsigned long, tick));

    fflush(stdout);  // release builds don't use <stdio.h>, but debug ones do
    fflush(stderr);  // ...so be helpful and flush any lingering debug output
  #endif

    // Delivering a panic should not rely on printf()/etc. in release build.

    char title[PANIC_TITLE_BUF_SIZE + 1]; // account for null terminator
    char buf[PANIC_BUF_SIZE + 1]; // "

    title[0] = '\0';
    buf[0] = '\0';

  #if !defined(NDEBUG) && 0
    //
    // These are currently disabled, because they generate too much junk.
    // Address Sanitizer gives a reasonable idea of the stack.
    //
    Dump_Info();
    Dump_Stack(FS_TOP, 0);
  #endif

  #if !defined(NDEBUG) && defined(HAVE_EXECINFO_AVAILABLE)
    void *backtrace_buf[1024];
    int n_backtrace = backtrace(  // GNU extension (but valgrind is better)
        backtrace_buf,
        sizeof(backtrace_buf) / sizeof(backtrace_buf[0])
    );
    fputs("Backtrace:\n", stderr);
    backtrace_symbols_fd(backtrace_buf, n_backtrace, STDERR_FILENO);
    fflush(stdout);
  #endif

    strncat(title, "PANIC()", PANIC_TITLE_BUF_SIZE - 0);

    strncat(buf, Str_Panic_Directions, PANIC_BUF_SIZE - 0);

    strncat(buf, "\n", PANIC_BUF_SIZE - strlen(buf));

    if (not p) {
        strncat(
            buf,
            "Panic was passed C nullptr",
            PANIC_BUF_SIZE - strlen(buf)
        );
    }
    else switch (Detect_Revolt_Pointer(p)) {
      case DETECTED_AS_UTF8: // string might be empty...handle specially?
        strncat(
            buf,
            cast(const char*, p),
            PANIC_BUF_SIZE - strlen(buf)
        );
        break;

      case DETECTED_AS_SERIES: {
        REBSER *s = m_cast(REBSER*, cast(const REBSER*, p)); // don't mutate
      #if !defined(NDEBUG)
        #if 0
            //
            // It can sometimes be useful to probe here if the series is
            // valid, but if it's not valid then that could result in a
            // recursive call to panic and a stack overflow.
            //
            PROBE(s);
        #endif

        if (GET_ARRAY_FLAG(s, IS_VARLIST)) {
            printf("Series VARLIST detected.\n");
            REBCTX *context = CTX(s);
            if (KIND_BYTE_UNCHECKED(CTX_ARCHETYPE(context)) == REB_ERROR) {
                printf("...and that VARLIST is of an ERROR!...");
                PROBE(context);
            }
        }
        Panic_Series_Debug(cast(REBSER*, s));
      #else
        UNUSED(s);
        strncat(buf, "valid series", PANIC_BUF_SIZE - strlen(buf));
      #endif
        break; }

      case DETECTED_AS_FREED_SERIES:
      #if defined(NDEBUG)
        strncat(buf, "freed series", PANIC_BUF_SIZE - strlen(buf));
      #else
        Panic_Series_Debug(m_cast(REBSER*, cast(const REBSER*, p)));
      #endif
        break;

      case DETECTED_AS_CELL:
      case DETECTED_AS_END: {
        const REBVAL *v = cast(const REBVAL*, p);
      #if defined(NDEBUG)
        UNUSED(v);
        strncat(buf, "value", PANIC_BUF_SIZE - strlen(buf));
      #else
        if (KIND_BYTE_UNCHECKED(v) == REB_ERROR) {
            printf("...panicking on an ERROR! value...");
            PROBE(v);
        }
        Panic_Value_Debug(v);
      #endif
        break; }

      case DETECTED_AS_FREED_CELL:
      #if defined(NDEBUG)
        strncat(buf, "freed cell", PANIC_BUF_SIZE - strlen(buf));
      #else
        Panic_Value_Debug(cast(const RELVAL*, p));
      #endif
        break;
    }

  #if !defined(NDEBUG)
    printf("%s\n", Str_Panic_Title);
    printf("%s\n", buf);
    fflush(stdout);
    debug_break();  // try to hook up to a C debugger - see %debug_break.h
  #endif

    exit (255);  // shell convention treats 255 as "exit code out of range"
}


//
//  panic: native [
//
//  "Cause abnormal termination of Revolt (dumps debug info in debug builds)"
//
//      reason [text! error!]
//          "Message to report (evaluation not counted in ticks)"
//  ]
//
REBNATIVE(panic)
{
    INCLUDE_PARAMS_OF_PANIC;

    REBVAL *v = ARG(reason);
    const void *p;

    // panic() on the string value itself would report information about the
    // string cell...but panic() on UTF-8 character data assumes you mean to
    // report the contained message.  PANIC-VALUE for the latter intent.
    //
    if (IS_TEXT(v)) {
        p = VAL_UTF8_AT(nullptr, v);
    }
    else {
        assert(IS_ERROR(v));
        p = VAL_CONTEXT(v);
    }

    // Uses frame_->tick instead of TG_Tick to identify the tick when PANIC
    // began its frame, not including later ticks for fulfilling ARG(value).
    //
  #ifdef DEBUG_COUNT_TICKS
    Panic_Core(p, frame_->tick, FRM_FILE_UTF8(frame_), FRM_LINE(frame_));
  #else
    const REBTCK tick = 0;
    Panic_Core(p, tick, FRM_FILE_UTF8(frame_), FRM_LINE(frame_));
  #endif
}


//
//  panic-value: native [
//
//  "Cause abnormal termination of Revolt, with diagnostics on a value cell"
//
//      value [any-value!]
//          "Suspicious value to panic on (debug build shows diagnostics)"
//  ]
//
REBNATIVE(panic_value)
{
    INCLUDE_PARAMS_OF_PANIC_VALUE;

  #ifdef DEBUG_TRACK_TICKS
    //
    // Use frame tick (if available) instead of TG_Tick, so tick count dumped
    // is the exact moment before the PANIC-VALUE ACTION! was invoked.
    //
    Panic_Core(
        ARG(value), frame_->tick, FRM_FILE_UTF8(frame_), FRM_LINE(frame_)
    );
  #else
    const REBTCK tick = 0;
    Panic_Core(ARG(value), tick, FRM_FILE_UTF8(frame_), FRM_LINE(frame_));
  #endif
}
