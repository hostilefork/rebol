//
//  File: %mod-jpg.c
//  Summary: "JPEG codec natives (dependent on %sys-core.h)"
//  Section: Extension
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
// The original JPEG encoder and decoder did not include sys-core.h.   But
// after getting rid of the REBCDI-based interface and converting codecs to
// be natives, it's necessary to include the core.
//

#include "sys-core.h"

#include "tmp-mod-jpg.h"

// These routines live in %u-jpg.c, which doesn't depend on %sys-core.h, but
// has a minor dependency on %reb-c.h

extern jmp_buf jpeg_state;
extern void jpeg_info(char *buffer, int nbytes, int *w, int *h);
extern void jpeg_load(char *buffer, int nbytes, char *output);


//
//  identify-jpeg?: native [
//
//  {Codec for identifying BINARY! data for a JPEG}
//
//      return: [logic!]
//      data [binary!]
//  ]
//
REBNATIVE(identify_jpeg_q)
{
    JPG_INCLUDE_PARAMS_OF_IDENTIFY_JPEG_Q;

    // Handle JPEG error throw:
    if (setjmp(jpeg_state)) {
        return Init_False(D_OUT);
    }

    REBYTE *data = VAL_BIN_AT(ARG(data));
    REBLEN len = VAL_LEN_AT(ARG(data));

    int w, h;
    jpeg_info(s_cast(data), len, &w, &h); // may longjmp above
    return Init_True(D_OUT);
}


//
//  decode-jpeg: native [
//
//  {Codec for decoding BINARY! data for a JPEG}
//
//      return: [image!]
//      data [binary!]
//  ]
//
REBNATIVE(decode_jpeg)
{
    JPG_INCLUDE_PARAMS_OF_DECODE_JPEG;

    // Handle JPEG error throw:
    if (setjmp(jpeg_state))
        fail (Error_Bad_Media_Raw()); // generic

    REBYTE *data = VAL_BIN_AT(ARG(data));
    REBLEN len = VAL_LEN_AT(ARG(data));

    int w, h;
    jpeg_info(s_cast(data), len, &w, &h); // may longjmp above

    char *image_bytes = rebAllocN(char, (w * h) * 4);  // RGBA is 4 bytes

    jpeg_load(s_cast(data), len, image_bytes);

    REBVAL *binary = rebRepossess(image_bytes, (w * h) * 4);

    REBVAL *image = rebValue(
        "make image! compose [",
            "(make pair! [", rebI(w), rebI(h), "])",
            binary,
        "]",
    rebEND);

    rebRelease(binary);

    return image;
}
