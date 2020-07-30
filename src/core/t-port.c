//
//  File: %t-port.c
//  Summary: "port datatype"
//  Section: datatypes
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


//
//  CT_Port: C
//
REBINT CT_Port(const REBCEL *a, const REBCEL *b, REBINT mode)
{
    if (mode < 0) return -1;
    return VAL_CONTEXT(a) == VAL_CONTEXT(b);
}


//
//  MAKE_Port: C
//
// Create a new port. This is done by calling the MAKE_PORT
// function stored in the system/intrinsic object.
//
REB_R MAKE_Port(
    REBVAL *out,
    enum Reb_Kind kind,
    const REBVAL *opt_parent,
    const REBVAL *arg
){
    assert(kind == REB_PORT);
    if (opt_parent)
        fail (Error_Bad_Make_Parent(kind, opt_parent));

    const bool fully = true; // error if not all arguments consumed

    REBVAL *make_port_helper = Get_Sys_Function(MAKE_PORT_P);
    assert(IS_ACTION(make_port_helper));

    assert(not IS_NULLED(arg)); // would need to DEVOID it otherwise
    if (RunQ_Throws(out, fully, rebU1(make_port_helper), arg, rebEND))
        fail (Error_No_Catch_For_Throw(out));

    // !!! Shouldn't this be testing for !IS_PORT( ) ?
    if (IS_BLANK(out))
        fail (Error_Invalid_Spec_Raw(arg));

    return out;
}


//
//  TO_Port: C
//
REB_R TO_Port(REBVAL *out, enum Reb_Kind kind, const REBVAL *arg)
{
    assert(kind == REB_PORT);
    UNUSED(kind);

    if (!IS_OBJECT(arg))
        fail (Error_Bad_Make(REB_PORT, arg));

    // !!! cannot convert TO a PORT! without copying the whole context...
    // which raises the question of why convert an object to a port,
    // vs. making it as a port to begin with (?)  Look into why
    // system/standard/port is made with CONTEXT and not with MAKE PORT!
    //
    REBCTX *context = Copy_Context_Shallow_Managed(VAL_CONTEXT(arg));
    RESET_VAL_HEADER(
        CTX_ARCHETYPE(context),
        REB_PORT,
        CELL_MASK_CONTEXT
    );

    return Init_Port(out, context);
}


//
//  Retrigger_Append_As_Write: C
//
// !!! In R3-Alpha, for the convenience of being able to APPEND to something
// that may be a FILE!-based PORT! or a BINARY! or STRING! with a unified
// interface, the APPEND command was re-interpreted as a WRITE/APPEND.  But
// it was done with presumption that APPEND and WRITE had compatible frames,
// which generally speaking they do not.
//
// This moves the functionality to an actual retriggering which calls whatever
// WRITE/APPEND would do in a generic fashion with a new frame.  Not all
// ports do this, as some have their own interpretation of APPEND.  It's
// hacky, but still not as bad as it was.  Review.
//
REB_R Retrigger_Append_As_Write(REBFRM *frame_) {
    INCLUDE_PARAMS_OF_APPEND;

    // !!! Something like `write/append %foo.txt "data"` knows to convert
    // %foo.txt to a port before trying the write, but if you say
    // `append %foo.txt "data"` you get `%foo.txtdata`.  Some actions are like
    // this, e.g. PICK, where they can't do the automatic conversion.
    //
    assert(IS_PORT(ARG(series))); // !!! poorly named
    UNUSED(ARG(series));
    if (not (
        IS_BINARY(ARG(value))
        or IS_TEXT(ARG(value))
        or IS_BLOCK(ARG(value)))
    ){
        fail (PAR(value));
    }

    if (REF(part) or REF(only) or REF(dup) or REF(line))
        fail (Error_Bad_Refines_Raw());

    return rebValueQ("write/append", D_ARG(1), D_ARG(2), rebEND);
}


//
//  REBTYPE: C
//
// !!! The concept of port dispatch from R3-Alpha is that it delegates to a
// handler which may be native code or user code.
//
REBTYPE(Port)
{
    // !!! The ability to transform some BLOCK!s into PORT!s for some actions
    // was hardcoded in a fairly ad-hoc way in R3-Alpha, which was based on
    // an integer range of action numbers.  Revolt turned these numbers into
    // symbols, where order no longer applied.  The mechanism needs to be
    // rethought, see:
    //
    // https://github.com/metaeducation/ren-c/issues/311
    //
    if (not IS_PORT(D_ARG(1))) {
        switch (VAL_WORD_SYM(verb)) {

        case SYM_READ:
        case SYM_WRITE:
        case SYM_QUERY:
        case SYM_OPEN:
        case SYM_CREATE:
        case SYM_DELETE:
        case SYM_RENAME: {
            //
            // !!! We are going to "re-apply" the call frame with routines we
            // are going to read the D_ARG(1) slot *implicitly* regardless of
            // what value points to.
            //
            const REBVAL *made = rebValueQ("make port!", D_ARG(1), rebEND);
            assert(IS_PORT(made));
            Move_Value(D_ARG(1), made);
            rebRelease(made);
            break; }

        case SYM_ON_WAKE_UP:
            break;

        // Once handled SYM_REFLECT here by delegating to T_Context(), but
        // common reflectors now in Context_Common_Action_Or_End()

        default:
            break;
        }
    }

    if (not IS_PORT(D_ARG(1)))
        fail (D_ARG(1));

    REBVAL *port = D_ARG(1);

    REB_R r = Context_Common_Action_Maybe_Unhandled(frame_, verb);
    if (r != R_UNHANDLED)
        return r;

    return Do_Port_Action(frame_, port, verb);
}


//=//// EXPERIMENTAL CHANNEL WORK FOR STACKLESS BUILD /////////////////////=//
//
// Looking at models for "coroutine" interoperability in producer/consumer
// type scenarios, Go seems to have a reasonable model based on "deep
// coroutine" mechanics which Revolt seeks to achieve.
//
// This probably ties into PORT! (if PORT! had a coherent design).  But as
// a test of being able to schedule "goroutine"-like things, this is just
// an experiment which uses HANDLE! or whatever.
//


inline static void Ensure_Channel(const REBVAL *chan) {
    if (rebNot("@channel = pick try match object!", chan, "'type", rebEND))
        fail ("Not a channel");
}


//
//  make-chan: native [
//      return: [object!]
//      /capacity [integer!]
//  ]
//
REBNATIVE(make_chan)
{
    INCLUDE_PARAMS_OF_MAKE_CHAN;

    if (not REF(capacity))
        Init_Integer(ARG(capacity), 0);  // unbuffered cap(chan) in Go is 0

    return rebValue(
        "make object! [",
            "type: @channel",
            "capacity:", REF(capacity),
            "closed: false",
            "buffer: copy []",  // !!! should be made a circular buffer
        "]",
    rebEND);
}


//
//  receive-chan: native [
//      return: [<opt> any-value!]
//      chan [object!]
//  ]
//
REBNATIVE(receive_chan)
//
// * a receive operation on a closed channel can always proceed immediately
// * can receive from closed channels so long as they are not empty
//
// Golang uses channel type's "zero value" if nothing received, we use NULL
{
    INCLUDE_PARAMS_OF_RECEIVE_CHAN;

    enum {
        ST_RECEIVE_CHAN_INITIAL_ENTRY = 0,
        ST_RECEIVE_CHAN_BLOCKING
    };

    REBVAL* chan = ARG(chan);
    Ensure_Channel(chan);

    switch (D_STATE_BYTE) {
      case ST_RECEIVE_CHAN_INITIAL_ENTRY: goto check_for_data;
      case ST_RECEIVE_CHAN_BLOCKING: goto check_for_data;
      default: assert(false);
    }

  check_for_data: {
    REBVAL *v = rebValue("take pick", chan, "'buffer", rebEND);
    if (v)
        return v;

    if (rebDid("pick", chan, "'closed", rebEND))
        return nullptr;

    // Here we have to block until a value is ready.  This requires us to put
    // ourselves into a suspended state and allow other functions to run.
    // Ideally we would communicate some sort of list of dependencies, but
    // for now just say that we block.  The Trampoline will unwind us and
    // then go try someone else.
    //
    D_STATE_BYTE = ST_RECEIVE_CHAN_BLOCKING;
    return R_BLOCKING;
  }
}


//
//  send-chan: native [
//      return: [any-value!]
//      chan [object!]
//      value [any-value!]
//  ]
//
REBNATIVE(send_chan)
//
// * channel and value are eval uated before communication begins
// * communication blocks until the send can proceed
// * a send on an unbuffered channel can proceed if a receiver is ready
// * a send on a buffered channel can proceed if there is room in the buffer
// * a send on a closed channel proceeds by causing a run-time panic
// * a send on a nil channel blocks forever  !!! (how to have "nil" channels?)
//
// !!! Go does not return values from a send().  Should we return what was
// put in?  Should the channel be returned?  consider `wait [c <- x [...]]`
//
// !!! Should there be a way for sending a BLOCK! to mean "send the items in
// the block individually"?  Should that be the default, overridden by /ONLY?
{
    INCLUDE_PARAMS_OF_SEND_CHAN;

    enum {
        ST_SEND_CHAN_INITIAL_ENTRY = 0,
        ST_SEND_CHAN_BLOCKING
    };

    REBVAL *chan = ARG(chan);
    Ensure_Channel(chan);

    REBVAL *v = ARG(value);

    switch (D_STATE_BYTE) {
      case ST_SEND_CHAN_INITIAL_ENTRY: goto check_for_capacity;
      case ST_SEND_CHAN_BLOCKING: goto check_for_capacity;
      default: assert(false);
    }

  check_for_capacity: {
    switch (rebUnboxInteger(
        "case [",
            "pick", chan, "'closed [0]",
            "(length of pick", chan, "'buffer)",
                    "<= (capacity-of-chan", chan, ") [",
                "append/only pick", chan, "'buffer", rebQ1(v),
                "1",
            "]",
            "true [2]"
        "]",
    rebEND)) {
      case 0:
        fail ("Attempt to SEND to a closed channel");

      case 1:
        RETURN (v);

      case 2:
        break;

      default:
        assert(false);
    }

    D_STATE_BYTE = ST_SEND_CHAN_BLOCKING;
    return R_BLOCKING;
  }
}


//
//  close-chan: native [
//      return: [void!]
//      chan [object!]
//  ]
//
REBNATIVE(close_chan)
//
// * don't close (or send values to) closed channels
// * don't close a channel from the receiver side
// * don't close a channel if the channel has multiple concurrent senders
// * only close a channel in a sender goroutine if the sender is the
//   only sender of the channel.
// * "channels aren't like files; you don't usually need to close them--
//   only when the receiver must be told there are no more values coming"
//
// Go has no test for if a channel is closed because it is believed that such
// a call would be of limited use, as it could not be used for decision making
// since the status could be different immediately after getting the return.
{
    INCLUDE_PARAMS_OF_CLOSE_CHAN;

    REBVAL *chan = ARG(chan);

    if (rebDid("pick", chan, "'closed", rebEND))
        fail ("Closing an already closed channel");

    rebElide("poke", chan, "'closed true", rebEND);

    return Init_Void(D_OUT);
}


//
//  length-of-chan: native [
//      return: [integer!]
//      chan [object!]
//  ]
//
REBNATIVE(length_of_chan)
//
// * length of unbuffered channel in Go is returned as 0
//
// Having an API querying the length of a channel is of limited use to
// receivers, as it could be different immediately after getting the return
// value back.
{
    INCLUDE_PARAMS_OF_LENGTH_OF_CHAN;

    REBVAL *chan = ARG(chan);

    REBLEN capacity = rebUnboxInteger("pick", chan, "'capacity", rebEND);

    REBVAL *buffer = rebValue("pick", chan, "'buffer", rebEND);
    REBLEN length = VAL_LEN_AT(buffer);
    rebRelease(buffer);

    if (length > capacity) {
        assert(length == capacity + 1);  // only allowed when blocking
        return Init_Integer(D_OUT, length - 1);
    }

    return Init_Integer(D_OUT, length);
}


//
//  capacity-of-chan: native [
//
//      return: [integer!]
//      chan [object!]
//  ]
//
REBNATIVE(capacity_of_chan)
//
// * capacity of unbuffered channel in Go is returned as 0
{
    INCLUDE_PARAMS_OF_CAPACITY_OF_CHAN;

    REBVAL *chan = ARG(chan);

    REBLEN capacity = rebUnboxInteger("pick", chan, "'capacity", rebEND);

    return Init_Integer(D_OUT, capacity);
}
