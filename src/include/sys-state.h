//
//  File: %sys-state.h
//  Summary: "Memoization of Status of Global Interpreter State"
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
// One historical idea from Rebol is to make use of a number of "hot" global
// buffers.  This is useful when the produced data is only temporary, or when
// the precise size of an output is not known in advance.  (For instance: a
// REDUCE can't accurately predict the number of elements for the result block
// based on the number in the input block, so pushing to a cached memory
// location first to get an accurate count can avoid wasteful reallocations
// or unused memory in the result.)
//
// Some buffers cannot be used across recursions, and must be cleared out
// before requesting an arbitrary evaluation.  Others can "stack", so they
// require each evaluator recursion wishing to use them must mark the limit
// used at the beginning, complete their work, and then restore the buffer's
// position to where it was.  
//
// The handling of stackable buffers became more complicated with the addition
// of features like YIELD.  That means evaluator recursions can be suspended
// and resumed at will.  So there has to be enough smarts in the code for
// hibernating a portion of the buffer (in a GC-safe location), and restoring
// it to the right "baseline" for the stack level that is being unwound, e.g.:
//
//     g: generator [yield reduce [yield "a" yield "b"]]
//
//     >> g  ; imagine DSP is 0 here
//     == "a"
//
//     >> reduce [g g]  ; REDUCE changes the DSP as it accrues values
//     == ["b" ["a" "b"]]
//
// The REDUCE inside the generator and outside the generator both need to
// have a concept of baseline, but they're also pushing values to the data
// stack.  This means that baseline must be adjusted for each call to the
// generator based on the delta in stack position between each call.  Similar
// principles apply to adjusting markers for the mold buffer and other
// nestable global state.
//
//=//// NOTES ////////////////////////////////////////////////////////////=//
//
// * WORK IN PROGRESS: "stackless" features are adding demands to tighten up
//   the adjustment and rollback of global state.
//
// * Each evaluator stack frame currently stores a Reb_State structure in its
//   `REBFRM->baseline` field.  There are likely ways to compact and conserve
//   this information, e.g. by using a small fixed size structure that can
//   "pop out" into a dynamic structure if need be.  But, correctness first!
//


struct Reb_State {
    REBDSP dsp;
    REBLEN guarded_len;

    REBLEN manuals_len; // Where GC_Manuals was when state started
    REBLEN mold_buf_len;
    REBSIZ mold_buf_size;
    REBLEN mold_loop_tail;

    // Some operations disable the ability to halt, e.g. remove SIG_HALT
    // from Eval_Sigmask...and then restore it when they are done.  If one of
    // these operations is running and then there is a longjmp past the place
    // where the restore is going to happen, they'd have to pay the cost of
    // a PUSH_TRAP to put it back.  We save effort for that case by saving
    // the signal mask and restoring it at the trap states.
    //
    REBFLGS saved_sigmask;
};


// !!! Experimental concept of being able to cycle through tasks.  Currently
// the only way to think of this is in terms of plugs; if a task is unplugged
// then it is necessary to plug it back in before it can run.
//
// As a first implementation, the main execution path is left plugged in so
// its stack levels are above the GO routines...this prevents having to climb
// above the console's stackful calls.  It also avoids needing to reify the
// main execution path and flush its global state.  Arguably stacks could keep
// themselves plugged in until they had to be unplugged; it could be more
// efficient, but right now each task unplugs itself when it blocks.
//
struct Reb_Task {
    REBFRM *go_frame;  // frame of the ACTION! at the root of the GO

    // !!! Should `plug_frame` be a FRAME! value that's part of plug?  It
    // would ensure the top frame after applying is part-and-parcel of the
    // plug.  On the downside, this would force reification if it were made a
    // FRAME! value (and take up 4 platform pointers instead of 1, also).
    // As it happens, the other usage in YIELD currently forces reification
    // because it reifies the REBFRM* into state inside the yielder to pass
    // it back...this is not strictly needed.
    //
    RELVAL plug;
    REBFRM *plug_frame;

    struct Reb_Task *next;
};


// Capture a measure of the current global state.
//
// !!! This is a macro because it may be that since snapping the state is
// done on every frame push, that code should be in the header so it could
// get inlined.  However, header dependencies currently put definitions like
// DSP and MOLD_BUF later.  Review if it's worth it to break this out
// in a different way.
//
#define SNAP_STATE(s) \
    Snap_State_Core(s)


// When a stack is "unplugged", global state might need to be rolled back
// and saved into a value for later re-plugging.  Notably the data stack
// can be captured as an array easily with Pop_Stack_Values().  Rather than
// create a generic object to encompass other things (like mold buffer
// "residue"), they are just added to the array and popped off before the
// data stack gets popped off.
//
#define ARRAY_FLAG_PLUG_HAS_MOLD            ARRAY_FLAG_23
#define ARRAY_FLAG_PLUG_HAS_DATA_STACK      ARRAY_FLAG_24


// Check that the current global state lines up with the passed-in state.
//
#ifdef NDEBUG
    #define ASSERT_STATE_BALANCED(s) NOOP
#else
    #define ASSERT_STATE_BALANCED(s) \
        Assert_State_Balanced_Debug((s), __FILE__, __LINE__)
#endif
