//
//  File: %mod-console.c
//  Summary: "Read/Eval/Print Loop (REPL) Skinnable Console for Revolt"
//  Section: Extension
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2016-2019 Revolt Open Source Contributors
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

#ifdef TO_WINDOWS

    #undef _WIN32_WINNT  // https://forum.rebol.info/t/326/4
    #define _WIN32_WINNT 0x0501  // Minimum API target: WinXP
    #define WIN32_LEAN_AND_MEAN  // trim down the Win32 headers
    #include <windows.h>

    #undef IS_ERROR  // %windows.h defines this, but so does %sys-core.h

#elif defined(TO_EMSCRIPTEN)
    //
    // Nothing needed here yet...
    //
#else

    #include <signal.h>  // needed for SIGINT, SIGTERM, SIGHUP

#endif


#include "sys-core.h"

#include "tmp-mod-console.h"


//=//// USER-INTERRUPT/HALT HANDLING (Ctrl-C, Escape, etc.) ///////////////=//
//
// There's clearly contention for what a user-interrupt key sequence should
// be, given that "Ctrl-C" is copy in GUI applications.  Yet handling escape
// is not necessarily possible on all platforms and situations.
//
// For console applications, we assume that the program starts with user
// interrupting enabled by default...so we have to ask for it not to be when
// it would be bad to have the Revolt stack interrupted--during startup, or
// when in the "kernel" of the host console.
//
// (Note: If halting is done via Ctrl-C, technically it may be set to be
// ignored by a parent process or context, in which case conventional wisdom
// is that we should not be enabling it ourselves.  Review.)
//

bool halting_enabled = true;

#if defined(TO_EMSCRIPTEN)  //=//// EMSCRIPTEN ///////////////////////////=//

// !!! Review how an emscripten console extension should be hooking something
// like a keyboard shortcut for breaking.  With the pthread model, there may
// be shared memory for the GUI to be able to poke a value in that the running
// code can see to perceive a halt.

void Disable_Halting(void) {}
void Enable_Halting(void) {}


#elif defined(TO_WINDOWS)  //=//// WINDOWS ////////////////////////////////=//

// Windows handling is fairly simplistic--this is the callback passed to
// `SetConsoleCtrlHandler()`.  The most annoying thing about cancellation in
// windows is the limited signaling possible in the terminal's readline.
//
BOOL WINAPI Handle_Break(DWORD dwCtrlType)
{
    switch (dwCtrlType) {
      case CTRL_C_EVENT:
      case CTRL_BREAK_EVENT:
        rebHalt();
        return TRUE;  // TRUE = "we handled it"

      case CTRL_CLOSE_EVENT:
        //
        // !!! Theoretically the close event could confirm that the user
        // wants to exit, if there is possible unsaved state.  As a UI
        // premise this is probably less good than persisting the state
        // and bringing it back.
        //
      case CTRL_LOGOFF_EVENT:
      case CTRL_SHUTDOWN_EVENT:
        //
        // They pushed the close button, did a shutdown, etc.  Exit.
        //
        // !!! Review arbitrary "100" exit code here.
        //
        exit(100);

      default:
        return FALSE;  // FALSE = "we didn't handle it"
    }
}

BOOL WINAPI Handle_Nothing(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT)
        return TRUE;

    return FALSE;
}

void Disable_Halting(void)
{
    assert(halting_enabled);

    SetConsoleCtrlHandler(Handle_Break, FALSE);
    SetConsoleCtrlHandler(Handle_Nothing, TRUE);

    halting_enabled = false;
}

void Enable_Halting(void)
{
    assert(not halting_enabled);

    SetConsoleCtrlHandler(Handle_Break, TRUE);
    SetConsoleCtrlHandler(Handle_Nothing, FALSE);

    halting_enabled = true;
}

#else  //=//// POSIX, LINUX, MAC, etc. ////////////////////////////////////=//

// SIGINT is the interrupt usually tied to "Ctrl-C".  Note that if you use
// just `signal(SIGINT, Handle_Signal);` as R3-Alpha did, this means that
// blocking read() calls will not be interrupted with EINTR.  One needs to
// use sigaction() if available...it's a slightly newer API.
//
// http://250bpm.com/blog:12
//
// !!! What should be done about SIGTERM ("polite request to end", default
// unix kill) or SIGHUP ("user's terminal disconnected")?  Is it useful to
// register anything for these?  R3-Alpha did, and did the same thing as
// SIGINT.  Not clear why.  It did nothing for SIGQUIT:
//
// SIGQUIT is used to terminate a program in a way that is designed to
// debug it, e.g. a core dump.  Receiving SIGQUIT is a case where
// program exit functions like deletion of temporary files may be
// skipped to provide more state to analyze in a debugging scenario.
//
// SIGKILL is the impolite signal for shutdown; cannot be hooked/blocked

static void Handle_Signal(int sig)
{
    UNUSED(sig);
    rebHalt();
}

struct sigaction old_action;

void Disable_Halting(void)
{
    assert(halting_enabled);

    sigaction(SIGINT, nullptr, &old_action); // fetch current handler
    if (old_action.sa_handler != SIG_IGN) {
        struct sigaction new_action;
        new_action.sa_handler = SIG_IGN;
        sigemptyset(&new_action.sa_mask);
        new_action.sa_flags = 0;
        sigaction(SIGINT, &new_action, nullptr);
    }

    halting_enabled = false;
}

void Enable_Halting(void)
{
    assert(not halting_enabled);

    if (old_action.sa_handler != SIG_IGN) {
        struct sigaction new_action;
        new_action.sa_handler = &Handle_Signal;
        sigemptyset(&new_action.sa_mask);
        new_action.sa_flags = 0;
        sigaction(SIGINT, &new_action, nullptr);
    }

    halting_enabled = true;
}

#endif  //=///////////////////////////////////////////////////////////////=//


//
//  export console: native [
//
//  {Runs customizable Read-Eval-Print Loop}
//
//      return: "Exit code, RESUME instruction, or handle to evaluator hook"
//          [integer! sym-group! handle!]
//      /resumable "Allow RESUME instruction (will return a SYM-GROUP!)"
//      /skin "File containing console skin, or MAKE CONSOLE! derived object"
//          [file! object!]
//      <local> responses requests old-console was-halting-enabled
//  ]
//
REBNATIVE(console)
//
// !!! The idea behind the console is that it can be called with skinning;
// so that if BREAKPOINT wants to spin up a console, it can...but with a
// little bit of injected information like telling you the current stack
// level it's focused on.  How that's going to work is still pretty up
// in the air.
//
// What it will return will be either an exit code (INTEGER!), a signal for
// cancellation (BLANK!), or a debugging instruction (BLOCK!).
{
    CONSOLE_INCLUDE_PARAMS_OF_CONSOLE;

    enum {
        ST_CONSOLE_INITIAL_ENTRY = 0,
        ST_CONSOLE_RECEIVING_REQUEST,
        ST_CONSOLE_EVALUATING_REQUEST
    };

    switch (D_STATE_BYTE) {
      case ST_CONSOLE_INITIAL_ENTRY: goto initial_entry;
      case ST_CONSOLE_RECEIVING_REQUEST: goto process_request;
      case ST_CONSOLE_EVALUATING_REQUEST: goto send_response;
      default: assert(false);
    }

  initial_entry: {
    //
    // !!! The initial usermode console implementation was geared toward a
    // single `system/console` object.  But the debugger raised the issue of
    // nested sessions which might have a different skin.  So save whatever
    // the console object was if it is being overridden.

    REBVAL *old_console = rebValue(":system/console", rebEND);
    if (REF(skin))
        rebElide("system/console: _", rebEND);  // !!! needed for now
    Move_Value(ARG(old_console), old_console);
    rebRelease(old_console);

    // We only enable halting (e.g. Ctrl-C, or Escape, or whatever) when user
    // code is running...not when the CONSOLE-IMPL function itself is, or
    // during startup.  (Enabling it during startup would require a special
    // "kill" mode that did not call rebHalt(), as basic startup cannot
    // meaningfully be halted--the system would be in an incomplete state.)
    //
    bool was_halting_enabled = halting_enabled;
    if (was_halting_enabled)
        Disable_Halting();
    Init_Logic(ARG(was_halting_enabled), was_halting_enabled);

    REBVAL *responses = rebValueQ("make-chan/capacity 1", rebEND);
    REBVAL *requests = rebValueQ("make-chan/capacity 1", rebEND);

    // !!! TBD: Make sure `debuggable` is not enabled on this GO
    //
    rebElideQ(
        "go console-impl",  // action! that takes 2 args, run it
        requests,
        responses,  // prior result quoted, or error (or blank!)
        rebL(did REF(resumable)),
        REF(skin),
        rebEND
    );

    Move_Value(ARG(responses), responses);
    Move_Value(ARG(requests), requests);
    
    rebRelease(responses);
    rebRelease(requests);

    goto receive_request;
  }

  receive_request: {
    assert(not halting_enabled);  // Don't want CONSOLE-IMPL on the stack

    D_STATE_BYTE = ST_CONSOLE_RECEIVING_REQUEST;
    CONTINUE_WITH (NATIVE_VAL(receive_chan), ARG(requests));
  }

  process_request: {
    if (rebDidQ("integer?", D_OUT, rebEND))
        goto return_exit_code;  // INTEGER! request means exit code

    /*if (rebDidQ("match [sym-group! handle!]", code, rebEND)) {
        assert(REF(resumable));
        break;
    }*/   // resume requests from nested consoles...being rethought

    assert(IS_GROUP(D_OUT) or IS_BLOCK(D_OUT));  // current invariant

    bool debuggable = rebDidQ("block?", D_OUT, rebEND);
    UNUSED(debuggable);  // !!! Should be used, somehow

    // Both console-initiated and user-initiated code is cancellable with
    // Ctrl-C (though it's up to HOST-CONSOLE on the next iteration to
    // decide whether to accept the cancellation or consider it an error
    // condition or a reason to fall back to the default skin).
    //
    Enable_Halting();

    D_STATE_BYTE = ST_CONSOLE_EVALUATING_REQUEST;
    CONTINUE_CATCHABLE (D_OUT);
  }

  send_response: {
    //
    // !!! halting was enabled while the sandbox was run, but should not be
    // the CONSOLE-IMPL gets the result.  This should be a property tied to
    // that particular routine as being "kernel" and done by the trampoline.
    //
    Disable_Halting();

    Quotify(D_OUT, 1);

    rebElideQ("send-chan", ARG(responses), D_OUT, rebEND);

    goto receive_request;
  }

  return_exit_code: {
    rebElideQ("system/console:", ARG(old_console), rebEND);

    // !!! Go lore says "don't close a channel from the receiver side".  This
    // means we should not close `requests`?
    //
    rebElideQ("close-chan", ARG(responses), rebEND);

    // Exit code is now an INTEGER! or a resume instruction PATH!

    if (VAL_LOGIC(ARG(was_halting_enabled)))
        Enable_Halting();

    return D_OUT;  // http://stackoverflow.com/q/1101957/
  }
}
