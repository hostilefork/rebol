//
//  File: %c-state.c
//  Summary: "Memoization of Status of Global Interpreter State"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
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
// See remarks in %sys-state.h
//
//=//// NOTES ////////////////////////////////////////////////////////////=//
//
// * WORK IN PROGRESS: "stackless" features are adding demands to tighten up
//   the adjustment and rollback of global state.
//

#include "sys-core.h"


//
//  Snap_State_Core: C
//
// **Note:** Modifying this routine likely means a necessary modification to
// `Assert_State_Balanced_Debug()`.
//
void Snap_State_Core(struct Reb_State *s)
{
    s->dsp = DSP;

    // There should not be a Collect_Keys in progress.  (We use a non-zero
    // length of the collect buffer to tell if a later fail() happens in
    // the middle of a Collect_Keys.)
    //
    assert(ARR_LEN(BUF_COLLECT) == 0);

    s->guarded_len = SER_USED(GC_Guarded);

    s->manuals_len = SER_USED(GC_Manuals);
    s->mold_buf_len = STR_LEN(STR(MOLD_BUF));
    s->mold_buf_size = STR_SIZE(STR(MOLD_BUF));
    s->mold_loop_tail = ARR_LEN(TG_Mold_Stack);

    s->saved_sigmask = Eval_Sigmask;
}


//
//  Rollback_Globals_To_State: C
//
// This routine is used by things like Abort_Frame() when a fail occurs, to
// automatically restore the state of globals to how they were at the time
// the passed-in state was SNAP_STATE()'d.
//
void Rollback_Globals_To_State(struct Reb_State *s)
{
    DS_DROP_TO(s->dsp);

    // Free any manual series that were extant (e.g. Make_Series() nodes
    // which weren't created with NODE_FLAG_MANAGED and were not transitioned
    // into the managed state).  This will include the series used as backing
    // store for rebMalloc() calls.
    //
    assert(SER_USED(GC_Manuals) >= s->manuals_len);
    while (SER_USED(GC_Manuals) != s->manuals_len) {
        Free_Unmanaged_Series(
            *SER_AT(REBSER*, GC_Manuals, SER_USED(GC_Manuals) - 1)
        );  // ^-- Free_Unmanaged_Series will decrement SER_USED(GC_Manuals)
    }

    SET_SERIES_LEN(GC_Guarded, s->guarded_len);

    TERM_STR_LEN_SIZE(STR(MOLD_BUF), s->mold_buf_len, s->mold_buf_size);

  #if !defined(NDEBUG)
    //
    // Because reporting errors in the actual Push_Mold process leads to
    // recursion, this debug flag helps make it clearer what happens if
    // that does happen...and can land on the right comment.  If there's
    // a fail of some kind, the flag for the warning needs to be cleared.
    //
    TG_Pushing_Mold = false;
  #endif

    SET_SERIES_LEN(TG_Mold_Stack, s->mold_loop_tail);

    Eval_Sigmask = s->saved_sigmask;
}


//
//  Unplug_Stack: C
//
// Pulls a stack out into an independent list of frames, subtracting out the
// base frame as a baseline.  The resulting frame stack will end in nullptr
// (instead of FS_BOTTOM).
//
// This is used by something like YIELD, which unplugs the series of frames
// all the way up to the GENERATOR (or YIELDER) that it's running under...
// restoring the stack so the generator is back on top and able to return
// a value.  Any global state (like mold buffer bits or the data stack) which
// may not be at the same point when the replug happens is moved into a
// cell managed by the caller.  This is referred to as a "plug".
//
void Unplug_Stack(
    RELVAL *plug,  // cell where global state differentials can be stored
    REBFRM *f,  // frame to unplug (currently can only unplug topmost frame)
    REBFRM *base  // base frame to unplug relative to
){
    assert(f == FS_TOP);

    assert(Is_Action_Frame(f));  // only actions can initiate unpluggings
    assert(not Is_Action_Frame_Fulfilling(f));  // ...and just running ones

    assert(Is_Action_Frame(base));  // ...same goes for the base
    assert(not Is_Action_Frame_Fulfilling(base));  // ...etc.

    REBFRM *temp = f;
    while (true) {
        if (temp->out == base->out) {
            //
            // Reassign to mark the output as something randomly bad, but
            // still GC safe.  When the stack gets patched back in, it will
            // be recognized and reset to the new base's out.
            //
            temp->out = m_cast(REBVAL*, TRUE_VALUE);
        }

        // We make the baseline stack pointers in each frame relative to the
        // base frame, with that frame as if it were 0.  When the frame
        // gets plugged in again, we'll add the new base's dsp back in.
        //
        // !!! This may confuse a fail() if it expects to climb the stack and
        // see all the f->baseline.dsp be sane.  But as far as interim state
        // is concerned, there's no good number to put here...leaving it as
        // it was would be wrong too.  This might suggest an EVAL_FLAG for
        // "don't believe the dsp".  Tricky.
        //
        temp->baseline.dsp -= base->baseline.dsp;

        if (temp->prior == base) {
            //
            // The frame below the base was not fulfilling an argument, it
            // should be writing into the base's out cell.  But when the
            // base goes off the stack, that cell will most likely be
            // gone.  We'll have to point it at the new base's out cell
            // when we plug it back in.  Also we have to set it to something
            // legal to mark in GC as the cell will go stale.
            //
            assert(STATE_BYTE(temp->prior) != 0);  // must be a continuation
            assert(temp->out == TRUE_VALUE);  // should have matched base
            temp->prior = nullptr;  // show where the fragment of stack ends
            break;
        }
        temp = temp->prior;
        assert(STATE_BYTE(temp) != 0);  // must be a continuation

        if (temp == FS_TOP)  // "alive", but couldn't find in the stack walk
            fail ("Cannot yield to a generator that is suspended");

        if (GET_EVAL_FLAG(temp, ROOT_FRAME))
            fail ("Cannot yield across frame that's not a continuation");
    }

    // If any data stack has been accrued, we capture it into an array.  We
    // will have to re-push the values when the frame is plugged back in.
    //
    // !!! We do not technically need to manage this array...just keep the
    // values in it alive during GC.  But for simplicity, we keep it in a
    // value cell, and manage it.
    //
    REBFLGS flags = ARRAY_FLAG_NULLEDS_LEGAL;  // be agnostic, to be generic!
    if (DSP > base->baseline.dsp)  // do this first (since other flags push)
        flags |= ARRAY_FLAG_PLUG_HAS_DATA_STACK;

    if (STR_SIZE(STR(MOLD_BUF)) > base->baseline.mold_buf_size) {
        flags |= ARRAY_FLAG_PLUG_HAS_MOLD;
        Init_Text(
            DS_PUSH(),
            Pop_Molded_String_Core(
                STR(MOLD_BUF),
                base->baseline.mold_buf_size,
                base->baseline.mold_buf_len
            )
        );
    }

    if (flags == 0)
        Init_Block(plug, EMPTY_ARRAY);
    else
        Init_Block(plug, Pop_Stack_Values_Core(base->baseline.dsp, flags));

    TG_Top_Frame = base;
}


//
//  Replug_Stack: C
//
// This reverses the process of Unplug_Stack, patching a stack onto a new
// base location.
//
void Replug_Stack(REBFRM *f, REBFRM *base, REBVAL *plug) {
    assert(base == FS_TOP);  // currently can only plug in atop topmost frame

    assert(Is_Action_Frame(base));  // only actions can initiate repluggings
    assert(not Is_Action_Frame_Fulfilling(base));  // ...and just running ones

    assert(Is_Action_Frame(f));  // ...same goes for the plug going in
    assert(not Is_Action_Frame_Fulfilling(f));  // ...etc.

    REBFRM *temp = f;
    while (true) {
        //
        // The top frame for the base at time of unplug may have targeted any
        // output cell.  That output is likely gone (an argument fulfillment
        // for a now-finished function, an API cell that was released, etc.)
        // But more frames than that could have inherited the same f->out.
        // Unplug_Stack() put a bogus pointer to the read-only TRUE_VALUE
        // cell, which is good enough to be GC safe and also distinct.
        //
        // Anywhere we see that signal, replace it with the output that
        // this new base wants to write its output to.
        //
        if (temp->out == TRUE_VALUE)
            temp->out = base->out;

        // We're going to restore the values that were between the base and
        // the unplugged frame on the data stack.  But that means we have to
        // touch up the `dsp_orig` pointers as well in the frames.
        //
        temp->baseline.dsp += base->baseline.dsp;  // unplug made dsp 0-based

        if (temp->prior == nullptr)
            break;
        temp = temp->prior;
        assert(STATE_BYTE(temp) != 0);  // must be a continuation
    }

    // We chain the stack that was underneath the old base to the new base
    // so that it now considers this base the parent.  We also update
    // the outputs of that subframe to match the output of the current
    // frame (see assert in Unplug_Stack() proving subframe had same f->out).
    //
    assert(temp->out == base->out);  // frame under base was TRUE_VALUE
    temp->prior = base;

    // Now add in all the residual elements from the plug to global buffers
    // like the mold buffer and data stack.
    //
    // !!! This could be more efficient by not pushing and then popping
    // non-data-stack elements, but directly adding from the array and not
    // pushing.  But focusing on correctness first.
    //
    assert(IS_BLOCK(plug));
    assert(VAL_INDEX(plug) == 0);  // could store some number (?)
    REBARR *array = VAL_ARRAY(plug);
    REBVAL *item = KNOWN(ARR_HEAD(array));
    for (; NOT_END(item); ++item)
        Move_Value(DS_PUSH(), item);

    if (GET_ARRAY_FLAG(array, PLUG_HAS_MOLD)) {
        assert(IS_TEXT(DS_TOP));
        assert(VAL_INDEX(DS_TOP) == 0);
        Append_String(STR(MOLD_BUF), DS_TOP, VAL_LEN_HEAD(DS_TOP));
        DS_DROP();
    };

    Init_Blank(plug);  // no longer needed, let it be GC'd

    TG_Top_Frame = f;  // make the jump deeper into the stack official...
}


#if !defined(NDEBUG)

//
//  Assert_State_Balanced_Debug: C
//
// Check that all variables in `state` have returned to what they were at
// the time of snapshot.
//
void Assert_State_Balanced_Debug(
    struct Reb_State *s,
    const char *file,
    int line
){
    if (s->dsp != DSP) {
        printf(
            "DS_PUSH()x%d without DS_DROP()\n",
            cast(int, DSP - s->dsp)
        );
        panic_at (nullptr, file, line);
    }

    assert(ARR_LEN(BUF_COLLECT) == 0);

    if (s->guarded_len != SER_USED(GC_Guarded)) {
        printf(
            "PUSH_GC_GUARD()x%d without DROP_GC_GUARD()\n",
            cast(int, SER_USED(GC_Guarded) - s->guarded_len)
        );
        REBNOD *guarded = *SER_AT(
            REBNOD*,
            GC_Guarded,
            SER_USED(GC_Guarded) - 1
        );
        panic_at (guarded, file, line);
    }

    // !!! Note that this inherits a test that uses GC_Manuals->content.xxx
    // instead of SER_USED().  The idea being that although some series
    // are able to fit in the series node, the GC_Manuals wouldn't ever
    // pay for that check because it would always be known not to.  Review
    // this in general for things that may not need "series" overhead,
    // e.g. a contiguous pointer stack.
    //
    if (s->manuals_len > SER_USED(GC_Manuals)) {
        //
        // Note: Should this ever actually happen, panic() on the series won't
        // do any real good in helping debug it.  You'll probably need
        // additional checks in Manage_Series() and Free_Unmanaged_Series()
        // that check against the caller's manuals_len.
        //
        panic_at ("manual series freed outside checkpoint", file, line);
    }
    else if (s->manuals_len < SER_USED(GC_Manuals)) {
        printf(
            "Make_Series()x%d w/o Free_Unmanaged_Series or Manage_Series\n",
            cast(int, SER_USED(GC_Manuals) - s->manuals_len)
        );
        REBSER *manual = *(SER_AT(
            REBSER*,
            GC_Manuals,
            SER_USED(GC_Manuals) - 1
        ));
        panic_at (manual, file, line);
    }

    assert(s->mold_buf_len == STR_LEN(STR(MOLD_BUF)));
    assert(s->mold_buf_size == STR_SIZE(STR(MOLD_BUF)));
    assert(s->mold_loop_tail == ARR_LEN(TG_Mold_Stack));

    assert(s->saved_sigmask == Eval_Sigmask);  // !!! is this always true?
}

#endif
