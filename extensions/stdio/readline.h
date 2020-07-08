//
//  File: %readline.h
//  Summary: "Shared Definitions for Windows/POSIX Console Line Reading"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2020 Revolt Open Source Contributors
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
// Windows has a monolithic facility for reading a line of input from the
// user.  This single command call is blocking (also known as "cooked" as
// opposed to "raw") and very limited.  As an initial goal of updating some
// of the very old R3-Alpha input code, the more granular POSIX code for
// implementing a "GNU libreadline"-type facility is being abstracted to
// share pieces of implementation with Windows.
//
// This file will attempt to define the hooks that are shared between the
// Windows and POSIX smart consoles.
//


// !!! Note: %reb-c.h must be included prior to this for proper CPLUSPLUS_11
// detection (MSVC doesn't do this correctly).  We do not do it here because
// of lack of include guards, but maybe we should put include guards in?
//
/* #include %reb-c.h */


// If a POSIX system does not offer Termios features, we assume it might also
// be old enough (or simple/embedded) to not support C99 and variadic macros.
// Without variadic macros, we have to use REVOLT_EXPLICIT_END and it makes the
// code less pleasant.  We tie the two things together to say that there is
// no concern for a "smart-terminal-based C89 build"...if you can only build
// with C89 then another aspect of that constraint is that you aren't going
// to get features like command history or tab completion.
//
#if !defined(__STDC_VERSION__) && !defined(__cplusplus)
    // ...compiler doesn't follow the foundational standard of spec versioning
    // e.g. too old to be at or after C99 or C++11 (--std=gnu89 acts like this)
#elif defined (__STDC_VERSION__) && __STDC_VERSION__ < 199901L
    // ...claims to be a C compiler, but too old to guarantee variadic macros
#elif defined(__cplusplus) && !defined(CPLUSPLUS_11)
    // ...C++11 standardized variadic macros in sync with C99's version...
#elif !defined(TO_WINDOWS) and defined(NO_TTY_ATTRIBUTES)
    // ...couldn't do terminal code even if we bothered with C89 support...
#else
// ...good enough to use both REVOLT_IMPLICIT_END and terminal functions...

#define REVOLT_SMART_CONSOLE

#include "revolt.h"

// !!! The history mechanism will be disconnected from the line editing
// mechanism--but for the moment, the line editing is the only place we
// get an Init() and Shutdown() opportunity.
//
extern REBVAL *Line_History;  // BLOCK! of TEXT!s


// The terminal is an opaque type which varies per operating system.  This
// is in C for now, but what it should evolve into is some kind of terminal
// PORT! which would have asynchronous events and behavior.
//
struct Reb_Terminal_Struct;
typedef struct Reb_Terminal_Struct STD_TERM;


extern int Term_Pos(STD_TERM *t);
extern REBVAL *Term_Buffer(STD_TERM *t);

extern STD_TERM *Init_Terminal();

extern void Term_Insert(STD_TERM *t, const REBVAL *v);
extern void Term_Seek(STD_TERM *t, unsigned int pos);
extern void Move_Cursor(STD_TERM *t, int count);
extern void Delete_Char(STD_TERM *t, bool back);
extern void Term_Clear_To_End(STD_TERM *t);

extern void Term_Beep(STD_TERM *t);

extern void Quit_Terminal(STD_TERM *t);

// This attempts to get one unit of "event" from the console.  It does not
// use the R3-Alpha-style EVENT! datatype at this time.  Instead it returns:
//
//    CHAR!, TEXT! => printable characters (includes space, but not newline)
//    WORD! => keystroke or control code
//    VOID! => interrupted by HALT or Ctrl-C
//
// It does not do any printing or handling while fetching the event.
//
// The reason it returns accrued TEXT! in runs (vs. always returning each
// character individually) is because of pasting.  Taking the read() buffer
// in per-line chunks is much faster than trying to process each character
// insertion with its own code (it's noticeably slow).  But at typing speed
// it's fine.
//
// Note Ctrl-C comes from the SIGINT signal and not from the physical detection
// of the key combination "Ctrl + C", which this routine should not receive
// due to deferring to the default UNIX behavior for that (otherwise, scripts
// could not be cancelled unless they were waiting at an input prompt).
//
// !!! The idea is that if there is no event available, this routine will
// return a nullptr.  That would allow some way of exiting the read() to
// do another operation (process network requests for a real-time chat, etc.)
// This is at the concept stage at the moment.
//
extern REBVAL *Try_Get_One_Console_Event(STD_TERM *t, bool buffered);

// !!! This is what ESCAPE does; it's probably something that should be
// done at a more granular level of spooling ahead "peeked" console events
// vs. needing a separate API entry point.
//
extern void Term_Abandon_Pending_Events(STD_TERM *t);


#endif  // end guard against readline in pre-C99 compilers (would need rebEND)
