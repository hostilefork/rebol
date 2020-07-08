//
//  File: %d-stats.c
//  Summary: "Statistics gathering for performance analysis"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
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
// These routines are for gathering statistics and metrics.  While some of
// the metrics-gathering may require custom code in the memory allocator,
// it is hoped that many services can be built as an optional extension by
// taking advantage of hooks provided in DO and APPLY.
//

#include "sys-core.h"


//
//  stats: native [
//
//  {Provides status and statistics information about the interpreter.}
//
//      return: [<opt> time! integer!]
//      /show "Print formatted results to console"
//      /profile "Returns profiler object"
//      /evals "Number of values evaluated by interpreter"
//      /pool "Dump all series in pool"
//          [integer!]
//  ]
//
REBNATIVE(stats)
{
    INCLUDE_PARAMS_OF_STATS;

    if (REF(evals)) {
        REBI64 n = Eval_Cycles + Eval_Dose - Eval_Count;
        return Init_Integer(D_OUT, n);
    }

#ifdef NDEBUG
    UNUSED(REF(show));
    UNUSED(REF(profile));
    UNUSED(ARG(pool));

    fail (Error_Debug_Only_Raw());
#else
    if (REF(profile)) {
        REBVAL *obj = rebValue("make object! [",
            "evals:",
            "eval-actions:",
            "series-made:",
            "series-freed:",
            "series-expanded:",
            "series-bytes:",
            "series-recycled:",
            "made-blocks:",
            "made-objects:",
            "recycles:",
                "_",
        "]", rebEND);

        Move_Value(D_OUT, obj);
        rebRelease(obj);

        if (IS_OBJECT(D_OUT)) {
            REBVAL *stats = VAL_CONTEXT_VAR(D_OUT, 1);

            Init_Integer(stats, Eval_Cycles + Eval_Dose - Eval_Count);
            stats++;
            Init_Integer(stats, 0); // no such thing as natives, only functions

            stats++;
            Init_Integer(stats, PG_Reb_Stats->Series_Made);
            stats++;
            Init_Integer(stats, PG_Reb_Stats->Series_Freed);
            stats++;
            Init_Integer(stats, PG_Reb_Stats->Series_Expanded);
            stats++;
            Init_Integer(stats, PG_Reb_Stats->Series_Memory);
            stats++;
            Init_Integer(stats, PG_Reb_Stats->Recycle_Series_Total);

            stats++;
            Init_Integer(stats, PG_Reb_Stats->Blocks);
            stats++;
            Init_Integer(stats, PG_Reb_Stats->Objects);

            stats++;
            Init_Integer(stats, PG_Reb_Stats->Recycle_Counter);
        }

        return D_OUT;
    }

    if (REF(pool)) {
        REBVAL *pool_id = ARG(pool);
        Dump_Series_In_Pool(VAL_INT32(pool_id));
        return nullptr;
    }

    if (REF(show))
        Dump_Pools();

    return Init_Integer(D_OUT, Inspect_Series(did REF(show)));
#endif
}


enum {
    // A WORD! name for the first non-anonymous symbol with which a function
    // has been invoked.  This may turn into a BLOCK! of all the names a
    // function has been invoked with.
    //
    IDX_STATS_SYMBOL = 0,

    // Number of times the function has been called.
    //
    IDX_STATS_NUMCALLS = 1,

    // !!! More will be added here when timing data is included, but timing
    // is tricky to do meaningfully while subtracting the instrumentation
    // itself out.

    IDX_STATS_MAX
};


//
//  Measured_Dispatch_Hook: C
//
// This is the function which is swapped in for Dispatch_Internal() when stats
// are enabled.
//
// In order to actually be accurate, it would need some way to subtract out
// its own effect on the timing of functions above on the stack.
//
REB_R Measured_Dispatch_Hook(REBFRM * const f)
{
    REBMAP *m = VAL_MAP(Root_Stats_Map);

    REBACT *phase = FRM_PHASE(f);
    bool is_first_phase = (phase == f->original);

    // We can only tell if it's the last phase *before* the apply; because if
    // we check *after* it may change to become the last and need R_REDO_XXX.
    //
    bool is_last_phase = (ACT_UNDERLYING(phase) == phase);

    if (is_first_phase) {
        //
        // Currently we get a call for each "phase" of a composite function.
        // Whether this is good or bad remains to be seen, but doing otherwise
        // would require restructuring the evaluator in a way that would
        // compromise its efficiency.  But as a result, if we want to store
        // the accumulated time for this function run we need to have a map
        // from frame to start time.
        //
        // This is where we would be starting a timer.  A simpler case is
        // being studied for starters...of just counting.
    }

    REB_R r = Dispatch_Internal(f);
    assert(r->header.bits & NODE_FLAG_CELL);

    if (is_last_phase) {
        //
        // Finalize the inclusive time if it's the last phase.  Timing info
        // is being skipped for starters, just to increment a count of how
        // many times the function gets called.

        const bool cased = false;
        REBINT n = Find_Map_Entry(
            m,
            ACT_ARCHETYPE(f->original),
            SPECIFIED,
            NULL, // searching now, not inserting, so pass NULL
            SPECIFIED,
            cased // shouldn't matter
        );

        if (n == 0) {
            //
            // There's no entry yet for this ACTION!, initialize one.

            REBARR *a = Make_Array(IDX_STATS_MAX);
            if (f->opt_label != NULL)
                Init_Word(ARR_AT(a, IDX_STATS_SYMBOL), f->opt_label);
            else
                Init_Blank(ARR_AT(a, IDX_STATS_SYMBOL));
            Init_Integer(ARR_AT(a, IDX_STATS_NUMCALLS), 1);
            TERM_ARRAY_LEN(a, IDX_STATS_MAX);

            DECLARE_LOCAL (stats);
            Init_Block(stats, a);

            n = Find_Map_Entry(
                m,
                ACT_ARCHETYPE(f->original),
                SPECIFIED,
                stats, // inserting now, so don't pass NULL
                SPECIFIED,
                cased // shouldn't matter
            );
            assert(n != 0); // should have inserted
        }
        else {
            REBVAL *stats = KNOWN(ARR_AT(MAP_PAIRLIST(m), ((n - 1) * 2) + 1));

            REBARR *a = IS_BLOCK(stats) ? VAL_ARRAY(stats) : NULL;

            if (
                a != NULL
                && ARR_LEN(a) == IDX_STATS_MAX
                && (
                    IS_WORD(ARR_AT(a, IDX_STATS_SYMBOL))
                    || IS_BLANK(ARR_AT(a, IDX_STATS_SYMBOL))
                )
                && IS_INTEGER(ARR_AT(a, IDX_STATS_NUMCALLS))
            ){
                if (
                    IS_BLANK(ARR_AT(a, IDX_STATS_SYMBOL))
                    && f->opt_label != NULL
                ){
                    Init_Word(ARR_AT(a, IDX_STATS_SYMBOL), f->opt_label);
                }
                Init_Integer(
                    ARR_AT(a, IDX_STATS_NUMCALLS),
                    VAL_INT64(ARR_AT(a, IDX_STATS_NUMCALLS)) + 1
                );
            }
            else if (not IS_ERROR(stats)) {
                //
                // The user might muck with the MAP! so we put an ERROR! in
                // to signal something went wrong, parameterized with the
                // invalid value...as long as it isn't already an error.
                //
                Init_Error(stats, Error_Bad_Value(stats));
            }
        }

        // Not clear if there's any statistical reason to process the r result
        // here, but leave the scaffold in case there is.
        //
        if (r == f->out) {
            // most common return, possibly thrown or not
        }
        if (not r) {
            // null
        }
        else switch (KIND_BYTE(r)) {
        case REB_R_REDO:
            assert(false); // shouldn't be possible for final phase
            break;

        case REB_R_INVISIBLE:
            break;

        case REB_R_REFERENCE:
        case REB_R_IMMEDIATE:
            assert(false); // internal use only, shouldn't be returned
            break;

        case REB_R_THROWN:
            break;

        default:
            assert(VAL_TYPE(r) < REB_MAX);  // does cell checking
            break;
        }
    }

    return r;
}


//
//  metrics: native [
//
//  {Track function calls and inclusive timings for those calls.}
//
//      return: [map!]
//      mode [logic!]
//          {Whether metrics should be on or off.}
//  ]
//
REBNATIVE(metrics)
{
    INCLUDE_PARAMS_OF_METRICS;

    REBVAL *mode = ARG(mode);

    Check_Security_Placeholder(Canon(SYM_DEBUG), SYM_READ, 0);

    // Note only the dispatcher is hooked.  If Eval_Core itself were hooked,
    // that could time things like SET-WORD! assignments; but there's nothing
    // the user could do about that.  And it just contaminates the timing they
    // are interested in, which is how long their functions take.

    if (VAL_LOGIC(mode))
        PG_Dispatch = &Measured_Dispatch_Hook;
    else
        PG_Dispatch = &Dispatch_Internal;

    return Root_Stats_Map;
}


#if defined(INCLUDE_CALLGRIND_NATIVE)
    #include <valgrind/callgrind.h>
#endif

//
//  callgrind: native [
//
//  {Provide access to services in <valgrind/callgrind.h>}
//
//      return: [void!]
//      'instruction [word!]
//          {Currently just either ON or OFF}
//  ]
//
REBNATIVE(callgrind)
//
// Note: In order to start callgrind without collecting data by default (so
// that you can instrument just part of the code) use:
//
//     valgrind --tool=callgrind --dump-instr=yes --collect-atstart=no ./r3
//
// The tool kcachegrind is very useful for reading the results.
{
    INCLUDE_PARAMS_OF_CALLGRIND;

  #if defined(INCLUDE_CALLGRIND_NATIVE)
    switch (VAL_WORD_SYM(ARG(instruction))) {
      case SYM_ON:
        PG_Callgrind_On = true;
        CALLGRIND_START_INSTRUMENTATION;
        CALLGRIND_TOGGLE_COLLECT;
        break;

      case SYM_OFF:
        PG_Callgrind_On = false;
        CALLGRIND_TOGGLE_COLLECT;
        CALLGRIND_STOP_INSTRUMENTATION;
        break;

      default:
        fail ("Currently CALLGRIND only supports ON and OFF");
    }
    return Init_Void(D_OUT);
  #else
    UNUSED(ARG(instruction));
    fail ("This executable wasn't compiled with INCLUDE_CALLGRIND_NATIVE");
  #endif
}
