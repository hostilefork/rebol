//
//  File: %n-loop.c
//  Summary: "native functions for loops"
//  Section: natives
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

#include "sys-core.h"
#include "sys-int-funcs.h" //REB_I64_ADD_OF

typedef enum {
    LOOP_FOR_EACH,
    LOOP_EVERY,
    LOOP_MAP_EACH,
    LOOP_MAP_EACH_SPLICED
} LOOP_MODE;


//
//  Catching_Break_Or_Continue: C
//
// Determines if a thrown value is either a break or continue.  If so, `val`
// is mutated to become the throw's argument.  Sets `broke` flag if BREAK.
//
// Returning false means the throw was neither BREAK nor CONTINUE.
//
bool Catching_Break_Or_Continue(REBVAL *val, bool *broke)
{
    const REBVAL *label = VAL_THROWN_LABEL(val);

    // Throw /NAME-s used by CONTINUE and BREAK are the actual native
    // function values of the routines themselves.
    //
    if (not IS_ACTION(label))
        return false;

    if (VAL_ACT_DISPATCHER(label) == &N_break) {
        *broke = true;
        CATCH_THROWN(val, val);
        assert(IS_NULLED(val)); // BREAK must always return NULL
        return true;
    }

    if (VAL_ACT_DISPATCHER(label) == &N_continue) {
        //
        // !!! Currently continue with no argument acts the same as asking
        // for CONTINUE NULL (the form with an argument).  This makes sense
        // in cases like MAP-EACH (one wants a continue to not add any value,
        // as opposed to a void) but may not make sense for all cases.
        //
        *broke = false;
        CATCH_THROWN(val, val);
        return true;
    }

    return false; // caller should let all other thrown values bubble up
}


//
//  break: native [
//
//  {Exit the current iteration of a loop and stop iterating further}
//
//  ]
//
REBNATIVE(break)
//
// BREAK is implemented via a thrown signal that bubbles up through the stack.
// It uses the value of its own native function as the name of the throw,
// like `throw/name null :break`.
{
    INCLUDE_PARAMS_OF_BREAK;

    return Init_Thrown_With_Label(D_OUT, NULLED_CELL, NATIVE_VAL(break));
}


//
//  continue: native [
//
//  "Throws control back to top of loop for next iteration."
//
//      value "If provided, act as if loop body finished with this value"
//          [<end> <opt> any-value!]
//  ]
//
REBNATIVE(continue)
//
// CONTINUE is implemented via a thrown signal that bubbles up through the
// stack.  It uses the value of its own native function as the name of the
// throw, like `throw/name value :continue`.
{
    INCLUDE_PARAMS_OF_CONTINUE;

    return Init_Thrown_With_Label(
        D_OUT,
        ARG(value), // null if missing, e.g. `do [continue]`
        NATIVE_VAL(continue)
    );
}


// Virtual_Bind_To_New_Context() allows LIT-WORD! syntax to reuse an existing
// variables binding:
//
//     x: 10
//     for-each 'x [20 30 40] [...]
//     ; The 10 will be overwritten, and x will be equal to 40, here
//
// It accomplishes this by putting a word into the "variable" slot, and having
// a flag to indicate a dereference is necessary.
//
REBVAL *Real_Var_From_Pseudo(REBVAL *pseudo_var) {
    if (NOT_CELL_FLAG(pseudo_var, BIND_MARKED_REUSE))
        return pseudo_var;
    if (IS_BLANK(pseudo_var))  // e.g. `for-each _ [1 2 3] [...]`
        return nullptr;  // signal to throw generated quantity away

    // Note: these variables are fetched across running arbitrary user code.
    // So the address cannot be cached...e.g. the object it lives in might
    // expand and invalidate the location.  (The `context` for fabricated
    // variables is locked at fixed size.)
    //
    assert(IS_QUOTED_WORD(pseudo_var));
    return Lookup_Mutable_Word_May_Fail(pseudo_var, SPECIFIED);
}


struct Loop_Each_State {
    REBVAL *out;  // where to write the output data (must be GC safe)
    const REBVAL *body;  // body to run on each loop iteration
    LOOP_MODE mode;  // FOR-EACH, MAP-EACH, EVERY
    REBCTX *pseudo_vars_ctx;  // vars made by Virtual_Bind_To_New_Context()
    REBVAL *data;  // the data argument passed in
    REBSER *data_ser;  // series data being enumerated (if applicable)
    REBLEN data_idx;  // index into the data for filling current variable
    REBLEN data_len;  // length of the data
};

// Isolation of central logic for FOR-EACH, MAP-EACH, and EVERY so that it
// can be rebRescue()'d in case of failure (to remove SERIES_INFO_HOLD, etc.)
//
// Returns nullptr or R_THROWN, where the relevant result is in les->out.
// (That result may be IS_NULLED() if there was a break during the loop)
//
static REB_R Loop_Each_Core(struct Loop_Each_State *les) {

    bool more_data = true;
    bool broke = false;
    bool no_falseys = true;  // not "all_truthy" because body *may* not run

    do {
        // Sub-loop: set variables.  This is a loop because blocks with
        // multiple variables are allowed, e.g.
        //
        //      >> for-each [a b] [1 2 3 4] [-- a b]]
        //      -- a: 1 b: 2
        //      -- a: 3 b: 4
        //
        // ANY-CONTEXT! and MAP! allow one var (keys) or two vars (keys/vals)
        //
        REBVAL *pseudo_var = CTX_VAR(les->pseudo_vars_ctx, 1);
        for (; NOT_END(pseudo_var); ++pseudo_var) {
            REBVAL *var = Real_Var_From_Pseudo(pseudo_var);

            // Even if data runs out, we could still have one last loop body
            // incarnation to run...with some variables unset.  Null those
            // variables here.
            //
            //     >> for-each [x y] [1] [-- x y]
            //     -- x: 1 y: \null\  ; Seems like an okay rendering
            //
            if (not more_data) {
                Init_Nulled(var);
                continue;
            }

            enum Reb_Kind kind = VAL_TYPE(les->data);
            switch (kind) {
              case REB_BLOCK:
              case REB_GROUP:
              case REB_PATH:
              case REB_SET_PATH:
              case REB_GET_PATH:
                if (var)
                    Derelativize(
                        var,
                        ARR_AT(ARR(les->data_ser), les->data_idx),
                        VAL_SPECIFIER(les->data)
                    );
                if (++les->data_idx == les->data_len)
                    more_data = false;
                break;

              case REB_OBJECT:
              case REB_ERROR:
              case REB_PORT:
              case REB_MODULE:
              case REB_FRAME: {
                REBVAL *key;
                REBVAL *val;
                REBLEN bind_index;
                while (true) {  // find next non-hidden key (if any)
                    key = VAL_CONTEXT_KEY(les->data, les->data_idx);
                    val = VAL_CONTEXT_VAR(les->data, les->data_idx);
                    bind_index = les->data_idx;
                    if (++les->data_idx == les->data_len)
                        more_data = false;
                    if (not Is_Param_Hidden(key))
                        break;
                    if (not more_data)
                        goto finished;
                }

                if (var)
                    Init_Any_Word_Bound(  // key is typeset, user wants word
                        var,
                        REB_WORD,
                        VAL_PARAM_SPELLING(key),
                        VAL_CONTEXT(les->data),
                        bind_index
                    );

                if (CTX_LEN(les->pseudo_vars_ctx) == 1) {
                    //
                    // Only wanted the key (`for-each key obj [...]`)
                }
                else if (CTX_LEN(les->pseudo_vars_ctx) == 2) {
                    //
                    // Want keys and values (`for-each key val obj [...]`)
                    //
                    ++pseudo_var;
                    var = Real_Var_From_Pseudo(pseudo_var);
                    Move_Value(var, val);
                }
                else
                    fail ("Loop enumeration of contexts must be 1 or 2 vars");
                break; }

              case REB_MAP: {
                assert(les->data_idx % 2 == 0);  // should be on key slot

                REBVAL *key;
                REBVAL *val;
                while (true) {  // pass over the unused map slots
                    key = KNOWN(ARR_AT(ARR(les->data_ser), les->data_idx));
                    ++les->data_idx;
                    val = KNOWN(ARR_AT(ARR(les->data_ser), les->data_idx));
                    ++les->data_idx;
                    if (les->data_idx == les->data_len)
                        more_data = false;
                    if (not IS_NULLED(val))
                        break;
                    if (not more_data)
                        goto finished;
                } while (IS_NULLED(val));

                if (var)
                    Move_Value(var, key);

                if (CTX_LEN(les->pseudo_vars_ctx) == 1) {
                    //
                    // Only wanted the key (`for-each key map [...]`)
                }
                else if (CTX_LEN(les->pseudo_vars_ctx) == 2) {
                    //
                    // Want keys and values (`for-each key val map [...]`)
                    //
                    ++pseudo_var;
                    var = Real_Var_From_Pseudo(pseudo_var);
                    Move_Value(var, val);
                }
                else
                    fail ("Loop enumeration of contexts must be 1 or 2 vars");

                break; }

              case REB_BINARY:
                if (var)
                    Init_Integer(var, BIN_HEAD(les->data_ser)[les->data_idx]);
                if (++les->data_idx == les->data_len)
                    more_data = false;
                break;

              case REB_TEXT:
              case REB_TAG:
              case REB_FILE:
              case REB_EMAIL:
              case REB_URL:
                if (var)
                    Init_Char_Unchecked(
                        var,
                        GET_CHAR_AT(STR(les->data_ser), les->data_idx)
                    );
                if (++les->data_idx == les->data_len)
                    more_data = false;
                break;

              case REB_ACTION: {
                REBVAL *generated = rebValue(les->data, rebEND);
                if (generated) {
                    if (var)
                        Move_Value(var, generated);
                    rebRelease(generated);
                }
                else {
                    more_data = false;  // any remaining vars must be unset
                    if (pseudo_var == CTX_VARS_HEAD(les->pseudo_vars_ctx)) {
                        //
                        // If we don't have at least *some* of the variables
                        // set for this body loop run, don't run the body.
                        //
                        goto finished;
                    }
                    if (var)
                        Init_Nulled(var);
                }
                break; }

              default:
                panic ("Unsupported type");
            }
        }

        if (Do_Branch_Throws(les->out, les->body)) {
            if (not Catching_Break_Or_Continue(les->out, &broke))
                return R_THROWN;  // non-loop-related throw

            if (broke) {
                Init_Nulled(les->out);
                return nullptr;
            }
        }

        switch (les->mode) {
          case LOOP_FOR_EACH:
            Voidify_If_Nulled_Or_Blank(les->out);  // null=BREAK, blank=empty
            break;

          case LOOP_EVERY:
            no_falseys = no_falseys and IS_TRUTHY(les->out);
            break;

          case LOOP_MAP_EACH:
          case LOOP_MAP_EACH_SPLICED:
            if (IS_NULLED(les->out))
                Init_Void(les->out);  // nulled used to signal breaking only
            else if (
                les->mode == LOOP_MAP_EACH_SPLICED
                and IS_BLOCK(les->out)
            ){
                RELVAL *v = VAL_ARRAY_AT(les->out);
                for (; NOT_END(v); ++v)
                    Derelativize(DS_PUSH(), v, VAL_SPECIFIER(les->out));
            }
            else
                Move_Value(DS_PUSH(), les->out);  // non nulls added to result
            break;
        }
    } while (more_data and not broke);

  finished:;

    if (les->mode == LOOP_EVERY and not no_falseys)
        Init_Logic(les->out, false);

    // We use nullptr to signal the result is in out.  If we returned les->out
    // it would be subject to the rebRescue() rules, and the loop could not
    // return an ERROR! value normally.
    //
    return nullptr;
}


//
//  Loop_Each: C
//
// Common implementation code of FOR-EACH, MAP-EACH, and EVERY.
//
// !!! This routine has been slowly clarifying since R3-Alpha, and can
// likely be factored in a better way...pushing more per-native code into the
// natives themselves.
//
static REB_R Loop_Each(REBFRM *frame_, LOOP_MODE mode)
{
    INCLUDE_PARAMS_OF_FOR_EACH;  // MAP-EACH & EVERY must subset interface

    Init_Blank(D_OUT);  // result if body never runs (MAP-EACH gives [])

    struct Loop_Each_State les;
    les.mode = mode;
    les.out = D_OUT;
    les.data = ARG(data);
    les.body = ARG(body);

    Virtual_Bind_Deep_To_New_Context(
        ARG(body),  // may be updated, will still be GC safe
        &les.pseudo_vars_ctx,
        ARG(vars)
    );
    Init_Object(ARG(vars), les.pseudo_vars_ctx);  // keep GC safe

    // Currently the data stack is only used by MAP-EACH to accumulate results
    // but it's faster to just save it than test the loop mode.
    //
    REBDSP dsp_orig = DSP;

    // Extract the series and index being enumerated, based on data type

    REB_R r;

    bool took_hold;
    if (IS_ACTION(les.data)) {
        //
        // The value is generated each time by calling the data action.
        // Assign values to avoid compiler warnings.
        //
        les.data_ser = nullptr;
        les.data_idx = 0;
        les.data_len = 0;
        took_hold = false;
    }
    else {
        if (ANY_SERIES(les.data)) {
            les.data_ser = VAL_SERIES(les.data);
            les.data_idx = VAL_INDEX(les.data);
            les.data_len = VAL_LEN_HEAD(les.data);  // has HOLD, won't change
        }
        else if (ANY_CONTEXT(les.data)) {
            les.data_ser = SER(CTX_VARLIST(VAL_CONTEXT(les.data)));
            les.data_idx = 1;
            les.data_len = SER_USED(les.data_ser);  // has HOLD, won't change
        }
        else if (ANY_PATH(les.data)) {
            les.data_ser = VAL_SERIES(les.data);
            assert(VAL_INDEX(les.data) == 0);
            les.data_idx = 0;
            les.data_len = SER_USED(les.data_ser);  // has HOLD, won't change
        }
        else if (IS_MAP(les.data)) {
            les.data_ser = SER(MAP_PAIRLIST(VAL_MAP(les.data)));
            les.data_idx = 0;
            les.data_len = SER_USED(les.data_ser);  // has HOLD, won't change
        }
        else
            panic ("Illegal type passed to Loop_Each()");

        // HOLD so length can't change

        took_hold = NOT_SERIES_INFO(les.data_ser, HOLD);
        if (took_hold)
            SET_SERIES_INFO(les.data_ser, HOLD);

        if (les.data_idx >= les.data_len) {
            assert(IS_BLANK(D_OUT));  // result if loop body never runs
            r = nullptr;
            goto cleanup;
        }
    }

    // If there is a fail() and we took a SERIES_INFO_HOLD, that hold needs
    // to be released.  For this reason, the code has to trap errors.

    r = rebRescue(cast(REBDNG*, &Loop_Each_Core), &les);

    //=//// CLEANUPS THAT NEED TO BE DONE DESPITE ERROR, THROW, ETC. //////=//

  cleanup:;

    if (took_hold)  // release read-only lock
        CLEAR_SERIES_INFO(les.data_ser, HOLD);

    if (IS_DATATYPE(les.data))
        Free_Unmanaged_Array(ARR(les.data_ser));  // temp array of instances

    //=//// NOW FINISH UP /////////////////////////////////////////////////=//

    if (r == R_THROWN) {  // generic THROW/RETURN/QUIT (not BREAK/CONTINUE)
        if (mode == LOOP_MAP_EACH or mode == LOOP_MAP_EACH_SPLICED)
            DS_DROP_TO(dsp_orig);
        return R_THROWN;
    }

    if (r) {
        assert(IS_ERROR(r));
        if (mode == LOOP_MAP_EACH or mode == LOOP_MAP_EACH_SPLICED)
            DS_DROP_TO(dsp_orig);
        rebJumps ("FAIL", rebR(r), rebEND);
    }

    // Otherwise, nullptr signals result in les.out (a.k.a. D_OUT)

    switch (mode) {
      case LOOP_FOR_EACH:
        //
        // nulled output means there was a BREAK
        // blank output means loop body never ran
        // void means the last body evaluation returned null or blank
        // any other value is the plain last body result
        //
        return D_OUT;

      case LOOP_EVERY:
        //
        // nulled output means there was a BREAK
        // blank means body never ran (`_ = every x [] [<unused>]`)
        // #[false] means loop ran, and at least one body result was "falsey"
        // any other value is the last body result, and is truthy
        // only illegal value here is void (would cause error if body gave it)
        //
        assert(not IS_VOID(D_OUT));
        return D_OUT;

      case LOOP_MAP_EACH:
      case LOOP_MAP_EACH_SPLICED:
        if (IS_NULLED(D_OUT)) {  // e.g. there was a BREAK. *must* return null
            DS_DROP_TO(dsp_orig);
            return nullptr;
        }

        // !!! MAP-EACH always returns a block except in cases of BREAK, but
        // paralleling some changes to COLLECT, it may be better if the body
        // never runs it returns blank (?)
        //
        return Init_Block(D_OUT, Pop_Stack_Values(dsp_orig));
    }

    DEAD_END;  // all branches handled in enum switch
}


//
//  for: native [
//
//  {Evaluate a block over a range of values.}
//
//      return: [<opt> any-value!]
//      'var "Variable to hold current value"
//          [word!]
//      start "Starting value"
//          [any-number!]
//      end "Ending value"
//          [any-number!]
//      bump "Amount to skip each time"
//          [any-number!]
//      body "Code to evaluate"
//          [<const> block! action!]
//  ]
//
REBNATIVE(for)
{
    INCLUDE_PARAMS_OF_FOR;

    enum {
        ST_FOR_INITIAL_ENTRY = 0,
        ST_FOR_SINGLE_RUN,  // edge case when start = end
        ST_FOR_RUNNING_BODY
    };

    REBVAL *start = ARG(start);
    REBVAL *end = ARG(end);
    REBVAL *bump = ARG(bump);

    REBVAL *counting_up = D_SPARE;

    switch (D_STATE_BYTE) {
      case ST_FOR_INITIAL_ENTRY: goto initial_entry;
      case ST_FOR_SINGLE_RUN: goto body_finished_or_threw;  // checks state
      case ST_FOR_RUNNING_BODY: goto body_finished_or_threw;  // checks state
      default: assert(false);
    }

    REBCTX *context;
    REBVAL *var;  // not movable, see #2274

  initial_entry: {
    Init_Blank(D_OUT);  // result if body never runs

    Virtual_Bind_Deep_To_New_Context(
        ARG(body),  // may be updated, will still be GC safe
        &context,
        ARG(var)
    );
    Init_Object(ARG(var), context);  // keep GC safe

    var = CTX_VAR(context, 1);
    Move_Value(var, start);

    // We actually hold the state in a cell; so each loop iteration it must
    // be checked to ensure it's still a number...or risk erroring

    // Run only once if start is equal to end...edge case.
    //
    if (rebDid(NATIVE_VAL(equal_q), start, end, rebEND)) {
        D_STATE_BYTE = ST_FOR_SINGLE_RUN;
        CONTINUE_CATCHABLE (ARG(body));
    }

    // As per #1993, start relative to end determines the "direction" of the
    // FOR loop.  (R3-Alpha used the sign of the bump, which meant it did not
    // have a clear plan for what to do with 0.)
    //
    Init_Logic(counting_up, rebDid(NATIVE_VAL(lesser_q), start, end, rebEND));
    if (
        (VAL_LOGIC(counting_up) and rebDid(bump, "<= 0", rebEND))
        or (not VAL_LOGIC(counting_up) and rebDid(bump, ">= 0", rebEND))
    ){
        return Init_Blank(D_OUT);  // avoid infinite loop, blank -> never ran
    }

    goto decide_whether_to_run_body;
  }

  body_finished_or_threw: {
    if (D_THROWING) {
        bool broke;
        if (not Catching_Break_Or_Continue(D_OUT, &broke))
            return R_THROWN;
        if (broke)
            return nullptr;
    }

    Voidify_If_Nulled_Or_Blank(D_OUT);  // null->BREAK, blank->empty

    if (D_STATE_BYTE == ST_FOR_SINGLE_RUN)
        return D_OUT;

    context = VAL_CONTEXT(ARG(var));
    var = CTX_VAR(context, 1);

    if (not IS_DECIMAL(var) and not IS_INTEGER(var))
        fail (Error_Invalid_Type(VAL_TYPE(var)));

    REBVAL *sum = rebValue(var, "+", bump, rebEND);
    Move_Value(var, sum);
    rebRelease(sum);

    goto decide_whether_to_run_body;
  }

  decide_whether_to_run_body: {
    if (VAL_LOGIC(counting_up)) {
        if (rebDid(NATIVE_VAL(greater_q), var, end, rebEND))
            return D_OUT;
    }
    else {
        if (rebDid(NATIVE_VAL(lesser_q), var, end, rebEND))
            return D_OUT;
    }

    D_STATE_BYTE = ST_FOR_RUNNING_BODY;
    CONTINUE_CATCHABLE (ARG(body));
  }
}


//
//  for-skip: native [
//
//  "Evaluates a block for periodic values in a series"
//
//      return: "Last body result, or null if BREAK"
//          [<opt> any-value!]
//      'word "Variable set to each position in the series at skip distance"
//          [word! 'word! blank!]
//      series "The series to iterate over"
//          [<blank> any-series!]
//      skip "Number of positions to skip each time"
//          [<blank> integer!]
//      body "Code to evaluate each time"
//          [<const> block! action!]
//  ]
//
REBNATIVE(for_skip)
{
    INCLUDE_PARAMS_OF_FOR_SKIP;

    REBVAL *series = ARG(series);
    REBINT skip = Int32(ARG(skip));

    REBVAL *pseudo_var;
    REBVAL *var;
    TRASH_POINTER_IF_DEBUG(pseudo_var);
    TRASH_POINTER_IF_DEBUG(var);

    enum {
        ST_FOR_SKIP_INITIAL_ENTRY = 0,
        ST_FOR_SKIP_EVALUATING_BODY
    };

    switch (D_STATE_BYTE) {
      case ST_FOR_SKIP_INITIAL_ENTRY:
        goto initial_entry;

      case ST_FOR_SKIP_EVALUATING_BODY:
        pseudo_var = CTX_VAR(VAL_CONTEXT(D_SPARE), 1);  // not movable, #2274
        var = Real_Var_From_Pseudo(pseudo_var);
        goto body_was_evaluated;

      default: assert(false);
    }

  initial_entry: {
    Init_Blank(D_OUT);  // result if body never runs, `while [null] [...]`

    if (skip == 0) {
        //
        // !!! https://forum.rebol.info/t/infinite-loops-vs-errors/936
        //
        return D_OUT;  // blank is loop protocol if body never ran
    }

    REBCTX *context;
    Virtual_Bind_Deep_To_New_Context(
        ARG(body),  // may be updated, will still be GC safe
        &context,
        ARG(word)
    );

    pseudo_var = CTX_VAR(context, 1);  // not movable, see #2274
    var = Real_Var_From_Pseudo(pseudo_var);
    Move_Value(var, series);

    // Starting location when past end with negative skip:
    //
    if (skip < 0 and VAL_INDEX(var) >= VAL_LEN_HEAD(var))
        VAL_INDEX(var) = VAL_LEN_HEAD(var) + skip;

    Init_Object(D_SPARE, context);  // GC protect and perist context

    D_STATE_BYTE = ST_FOR_SKIP_EVALUATING_BODY;
    goto next_step;
  }

  next_step: {
    REBINT len = VAL_LEN_HEAD(var);  // VAL_LEN_HEAD() always >= 0
    REBINT index = VAL_INDEX(var);  // (may have been set to < 0 below)

    if (index < 0)
        return D_OUT;
    if (index >= len) {
        if (skip >= 0)
            return D_OUT;
        index = len + skip;  // negative
        if (index < 0)
            return D_OUT;
        VAL_INDEX(var) = index;
    }

    CONTINUE_CATCHABLE (ARG(body));
  }

  body_was_evaluated: {
    if (Is_Throwing(D_FRAME)) {
        bool broke;
        if (not Catching_Break_Or_Continue(D_OUT, &broke))
            return R_THROWN;
        if (broke)
            return nullptr;
    }
    Voidify_If_Nulled_Or_Blank(D_OUT);  // null->BREAK, blank->empty

    // Modifications to var are allowed, to another ANY-SERIES! value.
    //
    // If `var` is movable (e.g. specified via LIT-WORD!) it must be
    // refreshed each time arbitrary code runs, since the context may
    // expand and move the address, may get PROTECTed, etc.
    //
    var = Real_Var_From_Pseudo(pseudo_var);

    if (IS_NULLED(var))
        fail (PAR(word));
    if (not ANY_SERIES(var))
        fail (var);

    VAL_INDEX(var) += skip;
    goto next_step;
  }
}


//
//  stop: native [
//
//  {End the current iteration of CYCLE and return a value (nulls allowed)}
//
//      value "If no argument is provided, assume VOID!"
//          [<opt> <end> any-value!]
//  ]
//
REBNATIVE(stop)
//
// Most loops are not allowed to explicitly return a value and stop looping,
// because that would make it impossible to tell from the outside whether
// they'd requested a stop or if they'd naturally completed.  It would be
// impossible to propagate a value-bearing break-like request to an aggregate
// looping construct without invasively rebinding the break.
//
// CYCLE is different because it doesn't have any loop exit condition.  Hence
// it responds to a STOP request, which lets it return any value.
//
// Coupled with the unusualness of CYCLE, NULL is allowed to come from a STOP
// request because it is given explicitly.  STOP NULL thus seems identical
// to the outside to a BREAK.
{
    INCLUDE_PARAMS_OF_STOP;

    return Init_Thrown_With_Label(
        D_OUT,
        IS_ENDISH_NULLED(ARG(value))
            ? VOID_VALUE  // `if true [stop]`
            : ARG(value),  // `if true [stop 5]`, etc.
        NATIVE_VAL(stop)
    );
}


//
//  cycle: native [
//
//  "Evaluates a block endlessly, until a BREAK or a STOP is hit"
//
//      return: [<opt> any-value!]
//          {Null if BREAK, or non-null value passed to STOP}
//      body [<const> block! action!]
//          "Block or action to evaluate each time"
//  ]
//
REBNATIVE(cycle)
{
    INCLUDE_PARAMS_OF_CYCLE;

    enum {
        ST_CYCLE_INITIAL_ENTRY = 0,
        ST_CYCLE_EVALUATING_BODY
    };

    switch (D_STATE_BYTE) {
      case ST_CYCLE_INITIAL_ENTRY: goto initial_entry;
      case ST_CYCLE_EVALUATING_BODY: goto body_was_evaluated;
      default: assert(false);
    }

  initial_entry: {
    D_STATE_BYTE = ST_CYCLE_EVALUATING_BODY;
    CONTINUE_CATCHABLE (ARG(body));
  }

  body_was_evaluated: {
    if (Is_Throwing(D_FRAME)) {
        bool broke;
        if (not Catching_Break_Or_Continue(D_OUT, &broke)) {
            const REBVAL *label = VAL_THROWN_LABEL(D_OUT);
            if (
                IS_ACTION(label)
                and VAL_ACT_DISPATCHER(label) == &N_stop
            ){
                // See notes on STOP for why CYCLE is unique among loop
                // constructs, with a BREAK variant that returns a value.
                //
                CATCH_THROWN(D_OUT, D_OUT);
                return D_OUT;  // special case: null allowed (like break)
            }

            return R_THROWN;
        }
        if (broke)
            return nullptr;
    }

    CONTINUE_CATCHABLE (ARG(body));  // no break or stop, so keep going
  }
}


//
//  for-each: native [
//
//  {Evaluates a block for each value(s) in a series.}
//
//      return: "Last body result, or null if BREAK"
//          [<opt> any-value!]
//      'vars "Word or block of words to set each time, no new var if quoted"
//          [blank! word! 'word! block!]
//      data "The series to traverse"
//          [<blank> any-series! any-context! map! any-path!
//           action!]  ; experimental
//      body "Block to evaluate each time"
//          [<const> block! action!]
//  ]
//
REBNATIVE(for_each)
{
    return Loop_Each(frame_, LOOP_FOR_EACH);
}


//
//  every: native [
//
//  {Iterate and return false if any previous body evaluations were false}
//
//      return: [<opt> any-value!]
//          {null on BREAK, blank on empty, false or the last truthy value}
//      'vars [word! block!]
//          "Word or block of words to set each time (local)"
//      data [<blank> any-series! any-context! map! datatype! action!]
//          "The series to traverse"
//      body [<const> block! action!]
//          "Block to evaluate each time"
//  ]
//
REBNATIVE(every)
{
    return Loop_Each(frame_, LOOP_EVERY);
}


// For important reasons of semantics and performance, the REMOVE-EACH native
// does not actually perform removals "as it goes".  It could run afoul of
// any number of problems, including the mutable series becoming locked during
// the iteration.  Hence the iterated series is locked, and the removals are
// applied all at once atomically.
//
// However, this means that there's state which must be finalized on every
// possible exit path...be that BREAK, THROW, FAIL, or just ordinary finishing
// of the loop.  That finalization is done by this routine, which will clean
// up the state and remove any indicated items.  (It is assumed that all
// forms of exit, including raising an error, would like to apply any
// removals indicated thus far.)
//
// Because it's necessary to intercept, finalize, and then re-throw any
// fail() exceptions, rebRescue() must be used with a state structure.
//
struct Remove_Each_State {
    REBVAL *out;
    REBVAL *data;
    REBSER *series;
    bool broke;  // e.g. a BREAK ran
    const REBVAL *body;
    REBCTX *context;
    REBLEN start;
    REB_MOLD *mo;
};


// See notes on Remove_Each_State
//
static inline REBLEN Finalize_Remove_Each(struct Remove_Each_State *res)
{
    assert(GET_SERIES_INFO(res->series, HOLD));
    CLEAR_SERIES_INFO(res->series, HOLD);

    // If there was a BREAK, we return NULL to indicate that as part of
    // the loop protocol.  This prevents giving back a return value of
    // how many removals there were, so we don't do the removals.

    REBLEN count = 0;
    if (ANY_ARRAY(res->data)) {
        if (res->broke) {  // cleanup markers, don't do removals
            RELVAL *temp = VAL_ARRAY_AT(res->data);
            for (; NOT_END(temp); ++temp) {
                if (GET_CELL_FLAG(temp, MARKED_REMOVE))
                    CLEAR_CELL_FLAG(temp, MARKED_REMOVE);
            }
            return 0;
        }

        REBLEN len = VAL_LEN_HEAD(res->data);

        RELVAL *dest = VAL_ARRAY_AT(res->data);
        RELVAL *src = dest;

        // avoid blitting cells onto themselves by making the first thing we
        // do is to pass up all the unmarked (kept) cells.
        //
        while (NOT_END(src) and NOT_CELL_FLAG(src, MARKED_REMOVE)) {
            ++src;
            ++dest;
        }

        // If we get here, we're either at the end, or all the cells from here
        // on are going to be moving to somewhere besides the original spot
        //
        for (; NOT_END(dest); ++dest, ++src) {
            while (NOT_END(src) and GET_CELL_FLAG(src, MARKED_REMOVE)) {
                ++src;
                --len;
                ++count;
            }
            if (IS_END(src)) {
                TERM_ARRAY_LEN(VAL_ARRAY(res->data), len);
                return count;
            }
            Blit_Cell(dest, src);  // same array--rare place we can do this
        }

        // If we get here, there were no removals, and length is unchanged.
        //
        assert(count == 0);
        assert(len == VAL_LEN_HEAD(res->data));
    }
    else if (IS_BINARY(res->data)) {
        if (res->broke) { // leave data unchanged
            Drop_Mold(res->mo);
            return 0;
        }

        // If there was a THROW, or fail() we need the remaining data
        //
        REBLEN orig_len = VAL_LEN_HEAD(res->data);
        assert(res->start <= orig_len);
        Append_Ascii_Len(
            res->mo->series,
            cs_cast(BIN_AT(res->series, res->start)),
            orig_len - res->start
        );

        // !!! We are reusing the mold buffer, but *not putting UTF-8 data*
        // into it.  Revisit if this inhibits cool UTF-8 based tricks the
        // mold buffer might do otherwise.
        //
        REBSER *popped = Pop_Molded_Binary(res->mo);

        assert(BIN_LEN(popped) <= VAL_LEN_HEAD(res->data));
        count = VAL_LEN_HEAD(res->data) - BIN_LEN(popped);

        // We want to swap out the data properties of the series, so the
        // identity of the incoming series is kept but now with different
        // underlying data.
        //
        Swap_Series_Content(popped, VAL_SERIES(res->data));

        Free_Unmanaged_Series(popped);  // now frees incoming series's data
    }
    else {
        assert(ANY_STRING(res->data));
        if (res->broke) { // leave data unchanged
            Drop_Mold(res->mo);
            return 0;
        }

        // If there was a BREAK, THROW, or fail() we need the remaining data
        //
        REBLEN orig_len = VAL_LEN_HEAD(res->data);
        assert(res->start <= orig_len);

        for (; res->start != orig_len; ++res->start) {
            Append_Codepoint(
                res->mo->series,
                GET_CHAR_AT(STR(res->series), res->start)
            );
        }

        REBSTR *popped = Pop_Molded_String(res->mo);

        assert(STR_LEN(popped) <= VAL_LEN_HEAD(res->data));
        count = VAL_LEN_HEAD(res->data) - STR_LEN(popped);

        // We want to swap out the data properties of the series, so the
        // identity of the incoming series is kept but now with different
        // underlying data.
        //
        Swap_Series_Content(SER(popped), VAL_SERIES(res->data));

        Free_Unmanaged_Series(SER(popped));  // frees incoming series's data
    }

    return count;
}


// See notes on Remove_Each_State
//
static REB_R Remove_Each_Core(struct Remove_Each_State *res)
{
    // Set a bit saying we are iterating the series, which will disallow
    // mutations (including a nested REMOVE-EACH) until completion or failure.
    // This flag will be cleaned up by Finalize_Remove_Each(), which is run
    // even if there is a fail().
    //
    SET_SERIES_INFO(res->series, HOLD);

    REBLEN index = res->start;  // up here to avoid longjmp clobber warnings

    REBLEN len = SER_USED(res->series);  // temp read-only, this won't change
    while (index < len) {
        assert(res->start == index);

        REBVAL *var = CTX_VAR(res->context, 1);  // not movable, see #2274
        for (; NOT_END(var); ++var) {
            if (index == len) {
                //
                // The second iteration here needs x = #"c" and y as void.
                //
                //     data: copy "abc"
                //     remove-each [x y] data [...]
                //
                Init_Nulled(var);
                continue;  // the `for` loop setting variables
            }

            if (ANY_ARRAY(res->data))
                Derelativize(
                    var,
                    VAL_ARRAY_AT_HEAD(res->data, index),
                    VAL_SPECIFIER(res->data)
                );
            else if (IS_BINARY(res->data))
                Init_Integer(var, cast(REBI64, BIN_HEAD(res->series)[index]));
            else {
                assert(ANY_STRING(res->data));
                Init_Char_Unchecked(
                    var,
                    GET_CHAR_AT(STR(res->series), index)
                );
            }
            ++index;
        }

        if (Do_Branch_Throws(res->out, res->body)) {
            if (not Catching_Break_Or_Continue(res->out, &res->broke)) {
                REBLEN removals = Finalize_Remove_Each(res);
                UNUSED(removals);

                return R_THROWN; // we'll bubble it up, but will also finalize
            }

            if (res->broke) {
                //
                // BREAK; this means we will return nullptr and not run any
                // removals (we couldn't report how many if we did)
                //
                assert(res->start < len);
                REBLEN removals = Finalize_Remove_Each(res);
                UNUSED(removals);

                Init_Nulled(res->out);
                return nullptr;
            }
            else {
                // CONTINUE - res->out may not be void if /WITH refinement used
            }
        }
        if (IS_VOID(res->out))
            fail (Error_Void_Conditional_Raw());  // neither true nor false

        if (ANY_ARRAY(res->data)) {
            if (IS_NULLED(res->out) or IS_FALSEY(res->out)) {
                res->start = index;
                continue;  // keep requested, don't mark for culling
            }

            do {
                assert(res->start <= len);
                SET_CELL_FLAG(
                    VAL_ARRAY_AT_HEAD(res->data, res->start),
                    MARKED_REMOVE
                );
                ++res->start;
            } while (res->start != index);
        }
        else {
            if (not IS_NULLED(res->out) and IS_TRUTHY(res->out)) {
                res->start = index;
                continue;  // remove requested, don't save to buffer
            }

            do {
                assert(res->start <= len);
                if (IS_BINARY(res->data)) {
                    Append_Ascii_Len(
                        res->mo->series,
                        cs_cast(BIN_AT(res->series, res->start)),
                        1
                    );
                }
                else {
                    Append_Codepoint(
                        res->mo->series,
                        GET_CHAR_AT(STR(res->series), res->start)
                    );
                }
                ++res->start;
            } while (res->start != index);
        }
    }

    // We get here on normal completion (THROW and BREAK will return above)

    assert(not res->broke and res->start == len);

    REBLEN removals = Finalize_Remove_Each(res);
    Init_Integer(res->out, removals);

    return nullptr;
}


//
//  remove-each: native [
//
//  {Removes values for each block that returns true.}
//
//      return: [<opt> integer!]
//          {Number of removed series items, or null if BREAK}
//      'vars [blank! word! block!]
//          "Word or block of words to set each time (local)"
//      data [<blank> any-series!]
//          "The series to traverse (modified)" ; should BLANK! opt-out?
//      body [<const> block! action!]
//          "Block to evaluate (return TRUE to remove)"
//  ]
//
REBNATIVE(remove_each)
{
    INCLUDE_PARAMS_OF_REMOVE_EACH;

    struct Remove_Each_State res;
    res.data = ARG(data);
    FAIL_IF_READ_ONLY(res.data);

    // !!! Currently there is no support for VECTOR!, or IMAGE! (what would
    // that even *mean*?) yet these are in the ANY-SERIES! typeset.
    //
    if (not (
        ANY_ARRAY(res.data) or ANY_STRING(res.data) or IS_BINARY(res.data)
    )){
        fail (res.data);
    }

    // Check the series for whether it is read only, in which case we should
    // not be running a REMOVE-EACH on it.  This check for permissions applies
    // even if the REMOVE-EACH turns out to be a no-op.
    //
    res.series = VAL_SERIES(res.data);

    if (VAL_INDEX(res.data) >= SER_USED(res.series)) {
        //
        // If index is past the series end, then there's nothing removable.
        //
        // !!! Should REMOVE-EACH follow the "loop conventions" where if the
        // body never gets a chance to run, the return value is void?
        //
        return Init_Integer(D_OUT, 0);
    }

    // Create a context for the loop variables, and bind the body to it.
    // Do this before PUSH_TRAP, so that if there is any failure related to
    // memory or a poorly formed ARG(vars) that it doesn't try to finalize
    // the REMOVE-EACH, as `res` is not ready yet.
    //
    Virtual_Bind_Deep_To_New_Context(
        ARG(body),  // may be updated, will still be GC safe
        &res.context,
        ARG(vars)
    );
    Init_Object(ARG(vars), res.context);  // keep GC safe
    res.body = ARG(body);

    res.start = VAL_INDEX(res.data);

    REB_MOLD mold_struct;
    if (ANY_ARRAY(res.data)) {
        //
        // We're going to use NODE_FLAG_MARKED on the elements of data's
        // array for those items we wish to remove later.
        //
        // !!! This may not be better than pushing kept values to the data
        // stack and then creating a precisely-sized output blob to swap as
        // the underlying memory for the array.  (Imagine a large array from
        // which there are many removals, and the ensuing wasted space being
        // left behind).  But worth testing the technique of marking in case
        // it's ever required for other scenarios.
        //
        TRASH_POINTER_IF_DEBUG(res.mo);
    }
    else {
        // We're going to generate a new data allocation, but then swap its
        // underlying content to back the series we were given.  (See notes
        // above on how this might be the better way to deal with arrays too.)
        //
        // !!! Uses the mold buffer even for binaries, and since we know
        // we're never going to be pushing a value bigger than 0xFF it will
        // not require a wide string.  So the series we pull off should be
        // byte-sized.  In a sense this is wasteful and there should be a
        // byte-buffer-backed parallel to mold, but the logic for nesting mold
        // stacks already exists and the mold buffer is "hot", so it's not
        // necessarily *that* wasteful in the scheme of things.
        //
        CLEARS(&mold_struct);
        res.mo = &mold_struct;
        Push_Mold(res.mo);
    }

    res.out = D_OUT;

    res.broke = false;  // will be set to true if there is a BREAK

    REB_R r = rebRescue(cast(REBDNG*, &Remove_Each_Core), &res);

    if (r == R_THROWN)
        return R_THROWN;

    if (r) {  // Remove_Each_Core() couldn't finalize in this case due to fail
        assert(IS_ERROR(r));

        // !!! Because we use the mold buffer to achieve removals from strings
        // and the mold buffer has to equalize at the end of rebRescue(), we
        // cannot mutate the string here to account for the removals.  So
        // FAIL means no removals--but we need to get in and take out the
        // marks on the array cells.
        //
        REBLEN removals = Finalize_Remove_Each(&res);
        UNUSED(removals);

        rebJumps("FAIL", rebR(r), rebEND);
    }

    if (res.broke)
        assert(IS_NULLED(D_OUT));  // BREAK in loop
    else
        assert(IS_INTEGER(D_OUT));  // no break--plain removal count

    return D_OUT;
}


//
//  map-each: native [
//
//  {Evaluate a block for each value(s) in a series and collect as a block.}
//
//      return: [<opt> block!]
//          {Collected block (BREAK/WITH can add a final result to block)}
//      'vars [blank! word! block!]
//          "Word or block of words to set each time (local)"
//      data [<blank> any-series! any-path! action!]
//          "The series to traverse"
//      body [<const> <modal> block!]
//          "Block to evaluate each time"
//      /splice "Splice body result if it's a block"
//  ]
//
REBNATIVE(map_each)
{
    INCLUDE_PARAMS_OF_MAP_EACH;
    UNUSED(PAR(vars));
    UNUSED(PAR(data));
    UNUSED(PAR(body));

    return Loop_Each(
        frame_,
        REF(splice) ? LOOP_MAP_EACH_SPLICED : LOOP_MAP_EACH
    );
}


//
//  loop: native [
//
//  "Evaluates a block a specified number of times."
//
//      return: [<opt> any-value!]
//          {Last body result, or null if BREAK}
//      count [<blank> any-number! logic!]
//          "Repetitions (true loops infinitely, false doesn't run)"
//      body [<const> block! action!]
//          "Block to evaluate or action to run."
//  ]
//
REBNATIVE(loop)
{
    INCLUDE_PARAMS_OF_LOOP;

    // The original count is left as-is in the frame for debugging clarity,
    // and the D_SPARE cell is used as the decrementing index (if needed).
    //
    const REBVAL *count = ARG(count);

    enum {
        ST_LOOP_INITIAL_ENTRY = 0,
        ST_LOOP_EVALUATING_BODY
    };

    switch (D_STATE_BYTE) {
      case ST_LOOP_INITIAL_ENTRY: goto initial_entry;
      case ST_LOOP_EVALUATING_BODY: goto body_was_evaluated;
      default: assert(false);
    }

  initial_entry: {
    //
    // Current loop protocol is that a loop index of blank passed in will
    // bypass calling the native dispatcher (blank-in-null-out).  Other
    // cases that are decided to never run the body return blank.
    //
    // !!! Rethink ramifications in usages.  Should this be VOID!?
    //
    Init_Blank(D_OUT);

    if (IS_LOGIC(count)) {
        if (VAL_LOGIC(count) == false)
            return D_OUT;  // treat false as "don't run"

        // treat #[true] as "infinite loop"
    }
    else {
        if (VAL_INT64(count) <= 0)
            return D_OUT;  // assume negative means "don't run" (vs. error)

        Move_Value(D_SPARE, count);
    }

    D_STATE_BYTE = ST_LOOP_EVALUATING_BODY;
    CONTINUE_CATCHABLE (ARG(body));
  }

  body_was_evaluated: {
    if (Is_Throwing(D_FRAME)) {
        bool broke;
        if (not Catching_Break_Or_Continue(D_OUT, &broke))
            return R_THROWN;
        if (broke)
            return nullptr;
    }

    if (IS_LOGIC(count)) {
        assert(VAL_LOGIC(count) == true);  // #[false] already returned
        CONTINUE_CATCHABLE (ARG(body));  // #[true] is infinite loop
    }

    if (0 != --VAL_INT64(D_SPARE))  // decrement by one, seen by next call
        CONTINUE_CATCHABLE (ARG(body));

    return Voidify_If_Nulled_Or_Blank(D_OUT);  // null: BREAK, blank: empty
  }
}


//
//  until: native [
//
//  {Evaluates the body until it produces a conditionally true value}
//
//      return: [<opt> any-value!]
//          {Last body result, or null if a BREAK occurred}
//      body [<const> block! action!]
//  ]
//
REBNATIVE(until)
{
    INCLUDE_PARAMS_OF_UNTIL;

    enum {
        ST_UNTIL_INITIAL_ENTRY = 0,
        ST_UNTIL_EVALUATING_BODY
    };

  switch (D_STATE_BYTE) {
    case ST_UNTIL_INITIAL_ENTRY: goto initial_entry;
    case ST_UNTIL_EVALUATING_BODY: goto body_was_evaluated;
    default: assert(false);
  }

  initial_entry: {
    D_STATE_BYTE = ST_UNTIL_EVALUATING_BODY;
    CONTINUE_CATCHABLE (ARG(body));
  }

  body_was_evaluated: {
    if (Is_Throwing(D_FRAME)) {
        bool broke;
        if (not Catching_Break_Or_Continue(D_OUT, &broke))
            return R_THROWN;
        if (broke)
            return Init_Nulled(D_OUT);

        // The way a CONTINUE with a value works is to act as if the loop
        // body evaluated to the value.  Since the condition and body are
        // the same in this case.  CONTINUE TRUE will stop the UNTIL and
        // return TRUE, CONTINUE 10 will stop and return 10, etc.
        //
        // Plain CONTINUE is interpreted as CONTINUE NULL, and hence will
        // continue to run the loop.
    }

    if (IS_TRUTHY(D_OUT))  // will fail on voids (neither true nor false)
        return D_OUT;  // body evaluated conditionally true, return value

    CONTINUE_CATCHABLE (ARG(body));
  }
}


//
//  while: native [
//
//  {While a condition is conditionally true, evaluates the body}
//
//      return: [<opt> any-value!]
//          "Last body result, or null if BREAK"
//      condition [<const> block! action!]
//      body [<const> block! action!]
//  ]
//
REBNATIVE(while)
{
    INCLUDE_PARAMS_OF_WHILE;

    enum {
        ST_WHILE_INITIAL_ENTRY = 0,
        ST_WHILE_EVALUATING_CONDITION,
        ST_WHILE_EVALUATING_BODY
    };

    switch (D_STATE_BYTE) {
      case ST_WHILE_INITIAL_ENTRY: goto initial_entry;
      case ST_WHILE_EVALUATING_CONDITION: goto condition_was_evaluated;
      case ST_WHILE_EVALUATING_BODY: goto body_was_evaluated;
      default: assert(false);
    }

  initial_entry: {
    Init_Blank(ARG(return));  // result if body never runs
    D_STATE_BYTE = ST_WHILE_EVALUATING_CONDITION;
    CONTINUE (ARG(condition));
  }

  condition_was_evaluated: {
    if (IS_FALSEY(D_OUT)) {  // will error if void, neither true nor false
        Move_Value(D_OUT, ARG(return));  // restore last body run
        Voidify_If_Nulled_Or_Blank(D_OUT);  // null->BREAK, blank->not run
        RETURN (ARG(return));  // condition false, return last body result
    }
    D_STATE_BYTE = ST_WHILE_EVALUATING_BODY;

    if (IS_ACTION(ARG(body)))
        CONTINUE_WITH_CATCHABLE (ARG(body), D_OUT);  // copies with (D_OUT)

    CONTINUE_CATCHABLE (ARG(body));
  }

  body_was_evaluated: {
    if (D_THROWING) {
        bool broke;
        if (not Catching_Break_Or_Continue(D_OUT, &broke))
            return R_THROWN;

        if (broke)
            return nullptr;  // unique signal that a break occurred

        // fall through for CONTINUE (D_OUT holds continue's argument, if any)
    }

    Move_Value(ARG(return), D_OUT);  // save body result
    D_STATE_BYTE = ST_WHILE_EVALUATING_CONDITION;
    CONTINUE (ARG(condition));  // run the condition, no catching of throws
  }
}
