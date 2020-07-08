//
//  File: %sys-track.h
//  Summary: "*VERY USEFUL* Debug Tracking Capabilities for Cell Payloads"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Revolt Open Source Contributors
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
// `Reb_Track_Payload` is the value payload in debug builds for any REBVAL
// whose VAL_TYPE() doesn't need any information beyond the header.  This
// offers a chance to inject some information into the payload to help
// know where the value originated.  It is used by NULL cells, VOID!, BLANK!,
// LOGIC!, and BAR!.
//
// In addition to the file and line number where the assignment was made,
// the "tick count" of the DO loop is also saved.  This means that it can
// be possible in a repro case to find out which evaluation step produced
// the value--and at what place in the source.  Repro cases can be set to
// break on that tick count, if it is deterministic.
//
// If tracking information is desired for all cell types, that means the cell
// size has to be increased.  See DEBUG_TRACK_EXTEND_CELLS for this setting,
// which can be useful in extreme debugging cases.
//
// In the debug build, "Trash" cells (NODE_FLAG_FREE) can use their payload to
// store where and when they were initialized.  This also applies to some
// datatypes like BLANK!, BAR!, LOGIC!, or VOID!--since they only use their
// header bits, they can also use the payload for this in the debug build.
//
// (Note: The release build does not canonize unused bits of payloads, so
// they are left as random data in that case.)
//
// View this information in the debugging watchlist under the `track` union
// member of a value's payload.  It is also reported by panic().
//

#if defined(DEBUG_TRACK_CELLS)
    #if defined(DEBUG_COUNT_TICKS) && defined(DEBUG_TRACK_EXTEND_CELLS)
        #define TOUCH_CELL(c) \
            ((c)->touch = TG_Tick)
    #endif

    inline static void Set_Track_Payload_Extra_Debug(
        RELVAL *c,
        const char *file,
        int line
    ){
      #ifdef DEBUG_TRACK_EXTEND_CELLS // cell is made bigger to hold it
        c->track.file = file;
        c->track.line = line;

        #ifdef DEBUG_COUNT_TICKS
            c->extra.tick = c->tick = TG_Tick;
            c->touch = 0;
        #else
            c->extra.tick = 1; // unreadable blank needs for debug payload
        #endif
      #else // in space that is overwritten for cells that fill in payloads 
        PAYLOAD(Track, c).file = file;
        PAYLOAD(Track, c).line = line;
          
        #ifdef DEBUG_COUNT_TICKS
            c->extra.tick = TG_Tick;
        #else
            c->extra.tick = 1; // unreadable blank needs for debug payload
        #endif
      #endif
    }

    #define TRACK_CELL_IF_DEBUG(c,file,line) \
        Set_Track_Payload_Extra_Debug((c), (file), (line))

#elif !defined(NDEBUG)

    #define TRACK_CELL_IF_DEBUG(c,file,line) \
        ((c)->extra.tick = 1) // unreadable blank needs for debug payload

#else

    #define TRACK_CELL_IF_DEBUG(c,file,line) \
        NOOP

#endif
