//
//  File: %n-reduce.c
//  Summary: {REDUCE and COMPOSE natives and associated service routines}
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

#include "sys-core.h"

//
//  reduce: native [
//
//  {Evaluates expressions, keeping each result (DO only gives last result)}
//
//      return: "New array or value"
//          [<opt> any-value!]
//      value "GROUP! and BLOCK! evaluate each item, single values evaluate"
//          [any-value!]
//  ]
//
REBNATIVE(reduce)
{
    INCLUDE_PARAMS_OF_REDUCE;

    REBVAL *v = ARG(value);

    enum {
        ST_REDUCE_INITIAL_ENTRY = 0,
        ST_REDUCE_EVAL_STEP
    };

    switch (D_STATE_BYTE) {
      case ST_REDUCE_INITIAL_ENTRY: goto initial_entry;
      case ST_REDUCE_EVAL_STEP: goto eval_step_finished;
      default: assert(false);
    }

  initial_entry: {
    if (not ANY_BLOCK(v) and not ANY_GROUP(v)) {
        //
        // Single value REDUCE does a REEVALUATE, but doesn't allow arguments.
        // (R3-Alpha, would return the input, e.g. `reduce ':foo` => :foo)
        // This is a variant of REEVAL with an END feed.
        //
        // !!! Should error be "reduce-specific" if args were required?

        if (ANY_INERT(v))
            RETURN (v);  // save time if it's something like a TEXT!

        DECLARE_END_FRAME (subframe, EVAL_MASK_DEFAULT);
        subframe->executor = &Reevaluation_Executor;
        subframe->u.reval.value = v;
        Push_Frame(D_OUT, subframe);

        SET_EVAL_FLAG(frame_, DELEGATE_CONTROL);
        return R_CONTINUATION;
    }

    DECLARE_FRAME_AT (
        f,
        v,  // REB_BLOCK or REB_GROUP
        EVAL_MASK_DEFAULT
            | EVAL_FLAG_ALLOCATED_FEED
            | EVAL_FLAG_TRAMPOLINE_KEEPALIVE  // reused for each step
    );
    INIT_F_EXECUTOR(f, &New_Expression_Executor);
    SET_END(D_OUT);  // result if all invisibles
    Push_Frame(D_OUT, f);

    // We want the output newline status to mirror the newlines of the start
    // of the eval positions.  But when the evaluation callback happens, we
    // won't have the starting value anymore.  Cache newline flag in D_SPARE
    // (which doubles as a non-END signal that we are not on our first call).
    //
    bool newline_before = IS_END(F_VALUE(f))
        ? false
        : GET_CELL_FLAG(F_VALUE(f), NEWLINE_BEFORE);
    Init_Logic(D_SPARE, newline_before);
    D_STATE_BYTE = ST_REDUCE_EVAL_STEP;
    return R_CONTINUATION;
  }

  eval_step_finished: {
    if (Is_Throwing(D_FRAME)) {
        Abort_Frame(D_FRAME);
        return R_THROWN;
    }

    REBFRM *f = FS_TOP;
    assert(f->prior == frame_);  // review this guarantee
    assert(f->executor == nullptr);  // step should have completed
    assert(GET_EVAL_FLAG(f, TRAMPOLINE_KEEPALIVE));  // flag is not cleared

    if (IS_END(D_OUT)) {  // e.g. `reduce []` or `reduce [comment "hi"]`
        assert(IS_END(F_VALUE(f)));

        // !!! Could shortcut here, but assume it's uncommon, and it's more
        // important to keep the block-has-newline-end logic/etc. consistent
    }
    else {
        // We can't put nulls into array cells, so we put BLANK!.  This is
        // compatible with historical behavior of `reduce [if 1 = 2 [<x>]]`
        // which produced `[#[none]]`, and is generally more useful than
        // putting VOID!, as more operations skip blanks vs. erroring.
        //
        if (IS_NULLED(D_OUT))
            Init_Blank(DS_PUSH());
        else
            Move_Value(DS_PUSH(), D_OUT);

        // We pushed one item, so next subframe step needs a higher baseline
        //
        f->baseline.dsp += 1;

        if (VAL_LOGIC(D_SPARE))
            SET_CELL_FLAG(DS_TOP, NEWLINE_BEFORE);

        if (NOT_END(F_VALUE(f))) {
            SET_END(D_OUT);  // result if all invisibles
            Init_Logic(D_SPARE, GET_CELL_FLAG(F_VALUE(f), NEWLINE_BEFORE));
            INIT_F_EXECUTOR(f, &New_Expression_Executor);
            assert(D_STATE_BYTE == ST_REDUCE_EVAL_STEP);
            return R_CONTINUATION;
        }
    }

    Drop_Frame_Unbalanced(f);  // plain Drop_Frame() asserts on accumulation

    REBFLGS pop_flags = NODE_FLAG_MANAGED | ARRAY_MASK_HAS_FILE_LINE;
    if (GET_ARRAY_FLAG(VAL_ARRAY(v), NEWLINE_AT_TAIL))
        pop_flags |= ARRAY_FLAG_NEWLINE_AT_TAIL;

    return Init_Any_Array(
        D_OUT,
        VAL_TYPE(v),
        Pop_Stack_Values_Core(D_FRAME->baseline.dsp, pop_flags)
    );
  }
}


bool Match_For_Compose(const RELVAL *group, const REBVAL *label) {
    if (IS_NULLED(label))
        return true;

    assert(IS_TAG(label) or IS_FILE(label));

    if (VAL_LEN_AT(group) == 0) // you have a pattern, so leave `()` as-is
        return false;

    RELVAL *first = VAL_ARRAY_AT(group);
    if (VAL_TYPE(first) != VAL_TYPE(label))
        return false;

    return (CT_String(label, first, 1) > 0);
}


// Use rules of composition to do template substitutions on values matching
// `pattern` by evaluating those slots, leaving all other slots as is.
//
// Values are pushed to the stack because it is a "hot" preallocated large
// memory range, and the number of values can be calculated in order to
// accurately size the result when it needs to be allocated.  Not returning
// an array also offers more options for avoiding that intermediate if the
// caller wants to add part or all of the popped data to an existing array.
//
// Notes:
//
// Like PARSE, COMPOSE presents a challenge to the stackless model due to its
// recursive nature.  The desire to lock a series for enumeration means that
// the "feed" structure that would walk through an array has to be kept live
// on a recursion, and right now that's done by means of stack frames left
// live on the stack...each one of which needs an executor.
//
// The simple trick used here is to reify the original COMPOSE's frame at the
// top level into the spare cell, to get at the parameters, and leverage the
// trampoline to get the stackless behavior while still having the holds.
//
REB_R Composer_Executor(REBFRM *f) {
    if (Is_Throwing(f)) {
        //
        // The Composer_Executor() has no state it needs to free during an
        // unwind due to an error or a throw.
        //
        return R_THROWN;
    }

    REBFRM *frame_ = CTX_FRAME_IF_ON_STACK(VAL_CONTEXT(FRM_SPARE(f)));
    assert(frame_ != nullptr);

    enum {
        ST_COMPOSER_INITIAL_ENTRY = 0,
        ST_COMPOSER_GROUP_EVAL,
        ST_COMPOSER_DOUBLE_GROUP_EVAL,
        ST_COMPOSER_RECURSING
    };

    INCLUDE_PARAMS_OF_COMPOSE;

    UNUSED(ARG(value));  // Only top-level compose needed to start recursions

    const REBVAL *label = ARG(label);  // e.g. if <*>, only match `(<*> ...)`
    bool deep = did REF(deep);
    const REBVAL *predicate = REF(predicate);
    bool only = did REF(only);  // do not exempt (( )) from splicing

    assert(predicate == nullptr or IS_ACTION(predicate));

    switch (STATE_BYTE(f)) {  // note: not D_STATE (that's top-level COMPOSE!)
      case ST_COMPOSER_INITIAL_ENTRY: goto push_current;
      case ST_COMPOSER_GROUP_EVAL: goto any_group_eval_finished;
      case ST_COMPOSER_DOUBLE_GROUP_EVAL: goto any_group_eval_finished;
      case ST_COMPOSER_RECURSING: goto recursion_finished;
      default: assert(false);
    }

  push_next: {
    Fetch_Next_Forget_Lookback(f);
    goto push_current;
  }

  push_current: {
    if (IS_END(F_VALUE(f))) {
        Init_Logic(D_OUT, GET_CELL_FLAG(D_SPARE, SPARE_MARKED_CHANGED));
        INIT_F_EXECUTOR(f, nullptr);
        return D_OUT;  // TRUE if accumulated modifications, FALSE if not
    }

    const REBCEL *cell = VAL_UNESCAPED(F_VALUE(f));
    enum Reb_Kind kind = CELL_KIND(cell);  // notice `''(...)`

    if (not ANY_ARRAY_OR_PATH_KIND(kind)) {  // won't substitute/recurse
        Derelativize(DS_PUSH(), F_VALUE(f), F_SPECIFIER(f));
        goto push_next;   // ^-- Note: keeps newline flag
    }

    STATE_BYTE(f) = ST_COMPOSER_GROUP_EVAL;  // may override with ((...))

    REBSPC *match_specifier = nullptr;
    const RELVAL *match = nullptr;

    if (not ANY_GROUP_KIND(kind)) {
        //
        // Don't compose at this level, but may need to walk deeply to
        // find compositions inside it if /DEEP and it's an array
    }
    else if (not only and Is_Any_Doubled_Group(F_VALUE(f))) {
        RELVAL *inner = VAL_ARRAY_AT(F_VALUE(f));
        if (Match_For_Compose(inner, label)) {
            STATE_BYTE(f) = ST_COMPOSER_DOUBLE_GROUP_EVAL;
            match = inner;
            match_specifier = Derive_Specifier(F_SPECIFIER(f), inner);
        }
    }
    else {  // plain compose, if match
        if (Match_For_Compose(F_VALUE(f), label)) {
            match = F_VALUE(f);
            match_specifier = F_SPECIFIER(f);
        }
    }

    if (match) {
        //
        // If <*> is the label and (<*> 1 + 2) is found, run just (1 + 2).
        // Using feed interface vs plain Do_XXX to skip cheaply.
        //
        DECLARE_FEED_AT_CORE (groupfeed, match, match_specifier);
        if (not IS_NULLED(label))
            Fetch_Next_In_Feed(groupfeed, false);  // not possibly at END

        DECLARE_FRAME (
            groupframe,
            groupfeed,
            EVAL_MASK_DEFAULT
                | EVAL_FLAG_ALLOCATED_FEED
                | EVAL_FLAG_TO_END
        );
        Push_Frame(D_OUT, groupframe);

        INIT_F_EXECUTOR(groupframe, &New_Expression_Executor);
        Init_Nulled(f->out);  // want empty `()` to vanish as a null would
        return R_CONTINUATION;
    }
    else if (deep) {
        // compose/deep [does [(1 + 2)] nested] => [does [3] nested]

        DECLARE_FRAME_AT_CORE (
            deepframe,
            cast(const RELVAL*, cell),  // unescaped array (w/no QUOTEs),
            F_SPECIFIER(f),
            EVAL_MASK_DEFAULT
                | EVAL_FLAG_TRAMPOLINE_KEEPALIVE
        );
        Push_Frame(D_OUT, deepframe);

        // Allow the subframe to pick up on the initial parameterization
        // of the COMPOSE (even though it has no varlist of its own).
        //
        Move_Value(FRM_SPARE(deepframe), FRM_SPARE(f));

        // The subframe knows its own original DSP, and will remember it
        // and still be on the stack when it returns.  Everything else it
        // will get from the spare cell.  We'll know how much data by
        // the difference in DSPs.

        INIT_F_EXECUTOR(deepframe, &Composer_Executor);

        STATE_BYTE(f) = ST_COMPOSER_RECURSING;
        return R_CONTINUATION;
    }
    else {
        // compose [[(1 + 2)] (3 + 4)] => [[(1 + 2)] 7]  ; non-deep
        //
        Derelativize(DS_PUSH(), F_VALUE(f), F_SPECIFIER(f));
            // ^-- Note: keeps newline flag
    }

    goto push_next;
  }

  any_group_eval_finished: {  // looks at STATE_BYTE() for if doubled group
    REBLEN quotes = VAL_NUM_QUOTES(F_VALUE(f));
    const REBCEL *cell = VAL_UNESCAPED(F_VALUE(f));
    enum Reb_Kind kind = CELL_KIND(cell);  // notice `''(...)`

    REBVAL *insert;
    if (
        predicate
        and STATE_BYTE(f) != ST_COMPOSER_DOUBLE_GROUP_EVAL
        and VAL_ACTION(predicate) != NAT_ACTION(identity)
    ){
        insert = rebValue(predicate, rebQ1(f->out), rebEND);
    } else
        insert = IS_NULLED(f->out) ? nullptr : f->out;

    if (insert == nullptr and kind == REB_GROUP and quotes == 0) {
        //
        // compose [(unquoted "nulls *vanish*!" null)] => []
        // compose [(elide "so do 'empty' composes")] => []
    }
    else if (
        insert and IS_BLOCK(insert)
        and (predicate or STATE_BYTE(f) == ST_COMPOSER_DOUBLE_GROUP_EVAL)
    ){
        // We splice blocks if they were produced by a predicate
        // application, or if (( )) was used.

        // compose [(([a b])) merges] => [a b merges]

        if (quotes != 0 or kind != REB_GROUP)
            fail ("Currently can only splice plain unquoted GROUP!s");

        RELVAL *push = VAL_ARRAY_AT(insert);
        if (NOT_END(push)) {
            //
            // Only proxy newline flag from the template on *first*
            // value spliced in (it may have its own newline flag)
            //
            // !!! These rules aren't necessarily obvious.  If you
            // say `compose [thing ((block-of-things))]` did you want
            // that block to fit on one line?
            //
            Derelativize(DS_PUSH(), push, VAL_SPECIFIER(insert));
            if (GET_CELL_FLAG(F_VALUE(f), NEWLINE_BEFORE))
                SET_CELL_FLAG(DS_TOP, NEWLINE_BEFORE);
            else
                CLEAR_CELL_FLAG(DS_TOP, NEWLINE_BEFORE);

            while (++push, NOT_END(push))
                Derelativize(DS_PUSH(), push, VAL_SPECIFIER(insert));
        }
    }
    else {
        // !!! What about VOID!s?  REDUCE and other routines have
        // become more lenient, and let you worry about it later.

        // compose [(1 + 2) inserts as-is] => [3 inserts as-is]
        // compose [([a b c]) unmerged] => [[a b c] unmerged]

        if (insert == nullptr)
            Init_Nulled(DS_PUSH());
        else
            Move_Value(DS_PUSH(), insert);  // can't stack eval direct

        if (kind == REB_SET_GROUP)
            Setify(DS_TOP);
        else if (kind == REB_GET_GROUP)
            Getify(DS_TOP);
        else if (kind == REB_SYM_GROUP)
            Symify(DS_TOP);
        else
            assert(kind == REB_GROUP);

        Quotify(DS_TOP, quotes);  // match original quotes

        // Use newline intent from the GROUP! in the compose pattern
        //
        if (GET_CELL_FLAG(F_VALUE(f), NEWLINE_BEFORE))
            SET_CELL_FLAG(DS_TOP, NEWLINE_BEFORE);
        else
            CLEAR_CELL_FLAG(DS_TOP, NEWLINE_BEFORE);
    }

    if (insert != f->out)
        rebRelease(insert);

  #ifdef DEBUG_UNREADABLE_BLANKS
    Init_Unreadable_Blank(f->out);  // shouldn't leak temp eval
  #endif

    SET_CELL_FLAG(FRM_SPARE(f), SPARE_MARKED_CHANGED);  // signal not no-op
    goto push_next;  // keep going
  }

  recursion_finished: {
    REBFRM *subframe = FS_TOP;
    assert(subframe->prior == f);

    REBLEN quotes = VAL_NUM_QUOTES(F_VALUE(f));
    const REBCEL *cell = VAL_UNESCAPED(F_VALUE(f));
    enum Reb_Kind kind = CELL_KIND(cell);  // notice `''(...)`

    if (NOT_CELL_FLAG(FRM_SPARE(subframe), SPARE_MARKED_CHANGED)) {
        //
        // To save on memory usage, Ren-C does not make copies of
        // arrays that don't have some substitution under them.  This
        // may be controlled by a switch if it turns out to be needed.
        //
        DS_DROP_TO(subframe->baseline.dsp);
        Drop_Frame(subframe);
        Derelativize(DS_PUSH(), F_VALUE(f), F_SPECIFIER(f));
        goto push_next;  // keep going
    }

    REBFLGS pop_flags = NODE_FLAG_MANAGED | ARRAY_MASK_HAS_FILE_LINE;
    if (GET_ARRAY_FLAG(VAL_ARRAY(cell), NEWLINE_AT_TAIL))
        pop_flags |= ARRAY_FLAG_NEWLINE_AT_TAIL;

    REBARR *popped = Pop_Stack_Values_Core(subframe->baseline.dsp, pop_flags);
    if (ANY_PATH_KIND(kind))
        Init_Any_Path(
            DS_PUSH(),
            kind,
            popped  // can't push and pop in same step, need variable
        );
    else
        Init_Any_Array(
            DS_PUSH(),
            kind,
            popped  // can't push and pop in same step, need variable
        );

    Quotify(DS_TOP, quotes);  // match original quoting

    if (GET_CELL_FLAG(F_VALUE(f), NEWLINE_BEFORE))
        SET_CELL_FLAG(DS_TOP, NEWLINE_BEFORE);

    Drop_Frame_Unbalanced(subframe);
    SET_CELL_FLAG(FRM_SPARE(f), SPARE_MARKED_CHANGED);  // signal not no-op
    goto push_next;  // keep going
  }
}


//
//  compose: native [
//
//  {Evaluates only contents of GROUP!-delimited expressions in an array}
//
//      return: [any-array! any-path! any-word! action!]
//      :predicate [<skip> action!]  ; !!! PATH! may be meant as value (!)
//          "Function to run on composed slots (default: ENBLOCK)"
//      :label "Distinguish compose groups, e.g. [(plain) (<*> composed)]"
//          [<skip> tag! file!]
//      value "Array to use as the template (no-op if WORD! or ACTION!)"
//          [any-array! any-path! any-word! action!]
//      /deep "Compose deeply into nested arrays"
//      /only "Do not exempt ((...)) from predicate application"
//  ]
//
REBNATIVE(compose)
//
// Note: /INTO is intentionally no longer supported
// https://forum.rebol.info/t/stopping-the-into-virus/705
{
    INCLUDE_PARAMS_OF_COMPOSE;

    // The Composer_Executor() use these by virtue of being passed the FRAME!
    //
    UNUSED(ARG(label));
    UNUSED(ARG(deep));
    UNUSED(ARG(only));

    REBVAL *predicate = ARG(predicate);

    enum {
        ST_COMPOSE_INITIAL_ENTRY = 0,
        ST_COMPOSE_COMPOSING
    };

    switch (D_STATE_BYTE) {
      case ST_COMPOSE_INITIAL_ENTRY: goto initial_entry;
      case ST_COMPOSE_COMPOSING: goto composer_finished;
      default: assert(false);
    }

  initial_entry: {
    if (ANY_WORD(ARG(value)) or IS_ACTION(ARG(value)))
        RETURN (ARG(value));  // makes it easier to `set/hard compose target`

    if (not IS_NULLED(predicate)) {
        REBSTR *opt_label;
        if (Get_If_Word_Or_Path_Throws(
            D_OUT,
            &opt_label,
            predicate,
            SPECIFIED,
            false  // push_refinements = false, specialize for multiple uses
        )){
            return R_THROWN;
        }
        if (not IS_ACTION(D_OUT))
            fail ("PREDICATE provided to COMPOSE must look up to an ACTION!");

        Move_Value(predicate, D_OUT);
    }

    DECLARE_FRAME_AT_CORE (
        composer,
        ARG(value),
        VAL_SPECIFIER(ARG(value)),
        EVAL_MASK_DEFAULT |
            EVAL_FLAG_TRAMPOLINE_KEEPALIVE  // allows stack accumulation
    );
    Push_Frame(D_OUT, composer);  // writes TRUE if modified, FALSE if not

    // Allow the subframe to pick up on the initial parameterization
    // of the COMPOSE (even though it has no varlist of its own).
    //
    Init_Frame(FRM_SPARE(composer), Context_For_Frame_May_Manage(frame_));
    INIT_F_EXECUTOR(composer, &Composer_Executor);
    D_STATE_BYTE = ST_COMPOSE_COMPOSING;
    return R_CONTINUATION;
  }

  composer_finished: {
    REBFRM *composer = FS_TOP;
    assert(composer->prior == frame_);
    Drop_Frame_Unbalanced(composer);  // allow stack accumulation

    // This is the signal that stack levels use to say nothing under them
    // needed compose, so you can just use a copy (if you want).  COMPOSE
    // always copies at least the outermost array, though.
    //
    assert(IS_LOGIC(D_OUT));  // true: changed, false: unchanged

    // The stack values contain N NEWLINE_BEFORE flags, and we need N + 1
    // flags.  Borrow the one for the tail directly from the input REBARR.
    //
    REBFLGS flags = NODE_FLAG_MANAGED | ARRAY_MASK_HAS_FILE_LINE;
    if (GET_ARRAY_FLAG(VAL_ARRAY(ARG(value)), NEWLINE_AT_TAIL))
        flags |= ARRAY_FLAG_NEWLINE_AT_TAIL;

    REBARR *popped = Pop_Stack_Values_Core(D_FRAME->baseline.dsp, flags);
    if (ANY_PATH(ARG(value)))
        return Init_Any_Path(D_OUT, VAL_TYPE(ARG(value)), popped);

    return Init_Any_Array(D_OUT, VAL_TYPE(ARG(value)), popped);
  }
}


enum FLATTEN_LEVEL {
    FLATTEN_NOT,
    FLATTEN_ONCE,
    FLATTEN_DEEP
};


static void Flatten_Core(
    RELVAL *head,
    REBSPC *specifier,
    enum FLATTEN_LEVEL level
) {
    RELVAL *item = head;
    for (; NOT_END(item); ++item) {
        if (IS_BLOCK(item) and level != FLATTEN_NOT) {
            REBSPC *derived = Derive_Specifier(specifier, item);
            Flatten_Core(
                VAL_ARRAY_AT(item),
                derived,
                level == FLATTEN_ONCE ? FLATTEN_NOT : FLATTEN_DEEP
            );
        }
        else
            Derelativize(DS_PUSH(), item, specifier);
    }
}


//
//  flatten: native [
//
//  {Flattens a block of blocks.}
//
//      return: [block!]
//          {The flattened result block}
//      block [block!]
//          {The nested source block}
//      /deep
//  ]
//
REBNATIVE(flatten)
{
    INCLUDE_PARAMS_OF_FLATTEN;

    REBDSP dsp_orig = DSP;

    Flatten_Core(
        VAL_ARRAY_AT(ARG(block)),
        VAL_SPECIFIER(ARG(block)),
        REF(deep) ? FLATTEN_DEEP : FLATTEN_ONCE
    );

    return Init_Block(D_OUT, Pop_Stack_Values(dsp_orig));
}
