//
//  File: %mod-serial.c
//  Summary: "serial port interface"
//  Section: ports
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2013 REBOL Technologies
// Copyright 2013-2017 Revolt Open Source Contributors
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

#include "tmp-mod-serial.h"

#include "req-serial.h"

#define MAX_SERIAL_DEV_PATH 128

//
//  Serial_Actor: C
//
static REB_R Serial_Actor(REBFRM *frame_, REBVAL *port, const REBVAL *verb)
{
    FAIL_IF_BAD_PORT(port);

    REBCTX *ctx = VAL_CONTEXT(port);
    REBVAL *spec = CTX_VAR(ctx, STD_PORT_SPEC);
    REBVAL *path = Obj_Value(spec, STD_PORT_SPEC_HEAD_REF);
    if (path == NULL)
        fail (Error_Invalid_Spec_Raw(spec));

    REBREQ *serial = Ensure_Port_State(port, &Dev_Serial);
    struct rebol_devreq *req = Req(serial);

    // Actions for an unopened serial port:
    if (not (req->flags & RRF_OPEN)) {
        switch (VAL_WORD_SYM(verb)) {
          case SYM_REFLECT: {
            INCLUDE_PARAMS_OF_REFLECT;

            UNUSED(ARG(value));
            REBSYM property = VAL_WORD_SYM(ARG(property));
            assert(property != SYM_0);

            switch (property) {
              case SYM_OPEN_Q:
                return Init_False(D_OUT);

              default:
                break; }

            fail (Error_On_Port(SYM_NOT_OPEN, port, -12)); }

        case SYM_OPEN: {
            // !!! Note: GROUP! should not be necessary around MATCH:
            //
            // https://github.com/metaeducation/ren-c/issues/820

            ReqSerial(serial)->path = rebValue("use [path] ["
                "path: try pick", spec, "'serial-path",
                "match [file! text! binary!] path else [",
                    "fail [{Invalid SERIAL-PATH} path]",
                "] ]", rebEND);  // !!! handle needs release somewhere...

            ReqSerial(serial)->baud = rebUnbox("use [speed] [",
                "speed: try pick", spec, "'serial-speed",
                "match integer! speed else [",
                    "fail [{Invalid SERIAL-SPEED} speed]",
                "] ]", rebEND);

            ReqSerial(serial)->data_bits = rebUnbox("use [size] [",
                "size: try pick", spec, "'serial-data-size",
                "all [integer? size | size >= 5 | size <= 8 | size] else [",
                    "fail [{SERIAL-DATA-SIZE is [5..8], not} size]",
                "] ]", rebEND);

            ReqSerial(serial)->stop_bits = rebUnbox("use [stop] [",
                "stop: try pick", spec, "'serial-stop-bits",
                "first <- find [1 2] stop else [",
                    "fail [{SERIAL-STOP-BITS should be 1 or 2, not} stop]",
                "] ]", rebEND);

            ReqSerial(serial)->parity = rebUnbox("use [parity] [",
                "parity: try pick", spec, "'serial-parity",
                "switch parity [",
                    "_ [", rebI(SERIAL_PARITY_NONE), "]",
                    "'odd [", rebI(SERIAL_PARITY_ODD), "]",
                    "'even [", rebI(SERIAL_PARITY_EVEN), "]",
                "] else [",
                    "fail [{SERIAL-PARITY should be ODD/EVEN, not} parity]",
                "] ]", rebEND);

            ReqSerial(serial)->flow_control = rebUnbox("use [flow] [",
                "flow: try pick", spec, "'serial-flow-control",
                "switch flow [",
                    "_ [", rebI(SERIAL_FLOW_CONTROL_NONE), "]",
                    "'hardware", rebI(SERIAL_FLOW_CONTROL_HARDWARE), "]",
                    "'software", rebI(SERIAL_FLOW_CONTROL_SOFTWARE), "]",
                "] else [",
                    "fail [",
                        "{SERIAL-FLOW-CONTROL should be HARDWARE/SOFTWARE,}",
                        "{not} flow",
                    "]",
                "] ]", rebEND);

            OS_DO_DEVICE_SYNC(serial, RDC_OPEN);

            req->flags |= RRF_OPEN;
            RETURN (port); }

          case SYM_CLOSE:
            RETURN (port);

          default:
            fail (Error_On_Port(SYM_NOT_OPEN, port, -12));
        }
    }

    // Actions for an open socket:

    switch (VAL_WORD_SYM(verb)) {
      case SYM_REFLECT: {
        INCLUDE_PARAMS_OF_REFLECT;

        UNUSED(ARG(value));
        REBSYM property = VAL_WORD_SYM(ARG(property));
        assert(property != SYM_0);

        switch (property) {
          case SYM_OPEN_Q:
            return Init_True(D_OUT);

          default:
            break;
        }

        break; }

      case SYM_READ: {
        INCLUDE_PARAMS_OF_READ;

        UNUSED(PAR(source));

        if (REF(part) or REF(seek))
            fail (Error_Bad_Refines_Raw());

        UNUSED(PAR(string)); // handled in dispatcher
        UNUSED(PAR(lines)); // handled in dispatcher

        // Setup the read buffer (allocate a buffer if needed):
        REBVAL *data = CTX_VAR(ctx, STD_PORT_DATA);
        if (!IS_BINARY(data))
            Init_Binary(data, Make_Binary(32000));

        REBSER *ser = VAL_SERIES(data);
        req->length = SER_AVAIL(ser); // space available
        if (req->length < 32000 / 2)
            Extend_Series(ser, 32000);
        req->length = SER_AVAIL(ser);

        req->common.data = BIN_TAIL(ser); // write at tail

        req->actual = 0; // Actual for THIS read, not for total.

      #ifdef DEBUG_SERIAL
        printf("(max read length %d)", req->length);
      #endif

        // "recv can happen immediately"
        //
        OS_DO_DEVICE_SYNC(serial, RDC_READ);

      #ifdef DEBUG_SERIAL
        for (len = 0; len < req->actual; len++) {
            if (len % 16 == 0) printf("\n");
            printf("%02x ", req->common.data[len]);
        }
        printf("\n");
      #endif
        RETURN (port); }

      case SYM_WRITE: {
        INCLUDE_PARAMS_OF_WRITE;

        UNUSED(PAR(destination));

        if (REF(seek) or REF(append) or REF(allow) or REF(lines))
            fail (Error_Bad_Refines_Raw());

        // Determine length. Clip /PART to size of binary if needed.

        REBVAL *data = ARG(data);
        REBLEN len = VAL_LEN_AT(data);
        if (REF(part)) {
            REBLEN n = Int32s(ARG(part), 0);
            if (n <= len)
                len = n;
        }

        Move_Value(CTX_VAR(ctx, STD_PORT_DATA), data); // keep it GC safe
        req->length = len;
        req->common.data = VAL_BIN_AT(data);
        req->actual = 0;

        // "send can happen immediately"
        //
        OS_DO_DEVICE_SYNC(serial, RDC_WRITE);

        RETURN (port); }

      case SYM_ON_WAKE_UP: {
        // Update the port object after a READ or WRITE operation.
        // This is normally called by the WAKE-UP function.

        REBVAL *data = CTX_VAR(ctx, STD_PORT_DATA);
        if (req->command == RDC_READ) {
            if (IS_BINARY(data)) {
                SET_SERIES_LEN(
                    VAL_SERIES(data),
                    VAL_LEN_HEAD(data) + req->actual
                );
            }
        }
        else if (req->command == RDC_WRITE) {
            Init_Blank(data);  // Write is done.
        }
        return Init_Void(D_OUT); }

      case SYM_CLOSE:
        if (req->flags & RRF_OPEN) {
            OS_DO_DEVICE_SYNC(serial, RDC_CLOSE);

            req->flags &= ~RRF_OPEN;
        }
        RETURN (port);

      default:
        break;
    }

    return R_UNHANDLED;
}


//
//  get-serial-actor-handle: native [
//
//  {Retrieve handle to the native actor for the serial port}
//
//      return: [handle!]
//  ]
//
REBNATIVE(get_serial_actor_handle)
{
    OS_Register_Device(&Dev_Serial);

    Make_Port_Actor_Handle(D_OUT, &Serial_Actor);
    return D_OUT;
}
