//
//  File: %sys-throw.h
//  Summary: {Simulated datatype for throws, in lieu of REB_THROWN}
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
// All THROWN values have two parts: the REBVAL arg being thrown and
// a REBVAL indicating the /NAME of a labeled throw.  (If the throw was
// created with plain THROW instead of THROW/NAME then its name is BLANK!).
//
// You cannot fit both values into a single value's bits of course.  One way
// to approach the problem would be to create a new REB_THROWN type with
// two fields (like a PAIR!).  But since there can only be one thrown value
// on an evaluator thread at a time, a more efficient trick is used.  The
// throw label is shuffled up the stack via the output cell, with the arg
// put off to the side.
//
// There are important technical reasons for favoring the label as the output:
//
// * RETURN is implemented as a throw whose label is a FRAME!.  That FRAME!
//   value can store either a REBFRM* which costs nothing extra, or a REBCTX*
//   which requires "reifying" the frame and making it GC-visible.  Reifying
//   would happen unconditionally if the frame is put into a global variable,
//   but so long as the FRAME! value bubbles up no higher than the REBFRM*
//   it points to, it can be used as-is.  With RETURN, it will be exactly the
//   right lifetime--since the originating frame is right where it stops.
//
// * When various stack levels are checking for their interest in a thrown
//   value, they look at the label...and if it's not what they want, they
//   pass it on.  So the label is checked many times, while the arg is only
//   caught once at its final location.
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * While similar to ERROR!s that are "raised" with FAIL, throwing is a
//   lighter-weight mechanism and doesn't subvert the C call stack.
//   
// * ALL calls into the evaluator to generate values must check for the
//   thrown flag.  This is helped by naming conventions, e.g. `XXX_Throws()`
//   to remind callers they have to handle it, pass it up the stack, or
//   raise an uncaught throw exception.
//
// * VAL_THROWN_LABEL() should be used vs. direct access of a thrown out
//   cell.  This abstracts the mechanism and allows the debug build to do
//   more checking that thrown values aren't being dropped or misused.
//

inline static bool Is_Throwing(REBFRM *f) {
    //
    // !!! An original constraint on asking if something was throwing was
    // that only the top frame could be asked about.  But Action_Executor()
    // is called to re-dispatch when there may be a frame above (kept there
    // by request from something like REDUCE).  We relax the constraint to
    // only be able to return *true* to a throw request if there are no
    // frames above on the stack.
    //
    if (NOT_END(&TG_Thrown_Arg)) {
        /*assert(f == FS_TOP);*/  // forget even that check
        UNUSED(f);  // currently only used for debug build check
        return true;
    }
    return false;
}

#if defined(NDEBUG)
    #define VAL_THROWN_LABEL(thrown) \
        (thrown)
#else
    inline static const REBVAL *VAL_THROWN_LABEL(const REBVAL *thrown) {
        if (IS_END(&TG_Thrown_Label_Debug))
            return thrown;
        assert(IS_UNREADABLE_DEBUG(thrown));
        return &TG_Thrown_Label_Debug;
    }
#endif

inline static REB_R Init_Thrown_With_Label(
    REBVAL *out,
    const REBVAL *arg,
    const REBVAL *label // Note: is allowed to be same as `out`
){
  #if defined(NDEBUG)
    if (out != label)
        Move_Value(out, label);
  #else
    assert(IS_END(&TG_Thrown_Arg));
    assert(IS_END(&TG_Thrown_Label_Debug));

    // Help avoid accidental uses of thrown output as misunderstood plain
    // outputs, by forcing thrown label access through VAL_THROWN_LABEL()...
    // but still test the release code path half the time.  (Causes different
    // reifications, but outside performance should still work the same.)
    //
    if (SPORADICALLY(2)) {
        Move_Value(&TG_Thrown_Label_Debug, label);
        Init_Unreadable_Blank(out);
    }
    else {
        if (out != label)
            Move_Value(out, label);
    }
  #endif

    Move_Value(&TG_Thrown_Arg, arg);
    return R_THROWN; // for chaining to dispatcher output
}


// Failures can be abrupt (if issued via `fail()`), in which case they must
// be caught by the Trampoline or rebRescue().  The frame in which the failure
// occurred will be given a chance to clean up, but the frame also carries
// the EVAL_FLAG_ABRUPT_FAILURE...which forgives API leaks and manuals.  Then
// a thrown value with an ERROR! as a label will propagate up cooperatively.
//
// To avoid the overhead of the abrupt failure, another option is to just
// cooperatively fail in the first place.  This will not forgive any leaks,
// but if an Executor or Dispatcher wishes to use this mechanism then it
// need not worry about getting called back to do cleanup.  It's cleaner and
// a little bit faster.
//
#define Init_Thrown_Failure(out,error) \
    Init_Thrown_With_Label((out), NULLED_CELL, CTX_ARCHETYPE(error))


static inline void CATCH_THROWN(
    RELVAL *arg_out,
    REBVAL *thrown // Note: may be same pointer as arg_out
){
    ASSERT_NOT_END(&TG_Thrown_Arg);

    Move_Value(arg_out, &TG_Thrown_Arg);
    SET_END(&TG_Thrown_Arg);

  #if defined(NDEBUG)
    UNUSED(thrown);
  #else
    // The debug build may have taken the throw label out of the output slot
    // and put it into a variable on the side (this helps avoid non-throw
    // aware reads of f->out).  If this was done, put it back after catch
    // (it is done SPORADICALLY()).  Only do so if they weren't trying to
    // overwrite the frame's output slot and didn't care about the label.
    //
    if (NOT_END(&TG_Thrown_Label_Debug)) {
        if (arg_out != thrown)
            Move_Value(thrown, &TG_Thrown_Label_Debug);
        SET_END(&TG_Thrown_Label_Debug);
    }
  #endif
}
