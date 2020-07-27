//
//  File: %n-control.c
//  Summary: "native functions for control flow"
//  Section: natives
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2020 Revolt Open Source Contributors
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
// Control constructs follow these rules:
//
// * If they do not run any branches, the construct returns NULL...which is
//   not an ANY-VALUE! and can't be put in a block or assigned to a variable
//   (via SET-WORD! or SET-PATH!).  This is systemically the sign of a "soft
//   failure", and can signal constructs like ELSE, ALSO, TRY, etc.
//
// * If a branch *does* run--and that branch evaluation produces a NULL--then
//   conditionals designed to be used with branching (like IF or CASE) will
//   return a VOID! result.  Voids are neither true nor false, and since they
//   are values (unlike NULL), they distinguish the signal of a branch that
//   evaluated to NULL from no branch at all.
//
// * Zero-arity function values used as branches will be executed, and
//   single-arity functions used as branches will also be executed--but passed
//   the value of the triggering condition.  See Do_Branch_Throws().
//
// * There is added checking that a literal block is not used as a condition,
//   to catch common mistakes like `if [x = 10] [...]`.
//

#include "sys-core.h"


//
//  if: native [
//
//  {When TO LOGIC! CONDITION is true, execute branch}
//
//      return: "null if branch not run, otherwise branch result"
//          [<opt> any-value!]
//      condition [<opt> any-value!]
//      'branch "If arity-1 ACTION!, receives the evaluated condition"
//          [block! action! quoted! sym-word! sym-path! sym-group! blank!]
//  ]
//
REBNATIVE(if)
{
    INCLUDE_PARAMS_OF_IF;

    enum {
        ST_IF_INITIAL_ENTRY = 0,
        ST_IF_EVALUATING_BRANCH
    };

    switch (D_STATE_BYTE) {
      case ST_IF_INITIAL_ENTRY: goto initial_entry;
      case ST_IF_EVALUATING_BRANCH: goto branch_was_evaluated;
      default: assert(false);
    }

  initial_entry: {
    if (IS_CONDITIONAL_FALSE(ARG(condition)))  // void or literal blocks fail
        return nullptr;  // branch not run, null signals ELSE and THEN

    D_STATE_BYTE = ST_IF_EVALUATING_BRANCH;
    CONTINUE_WITH (ARG(branch), ARG(condition));  // pass condition if action
  }

  branch_was_evaluated: {
    return Voidify_If_Nulled(D_OUT);  // null reserved for branch not run
  }
}


//
//  either: native [
//
//  {Choose a branch to execute, based on TO-LOGIC of the CONDITION value}
//
//      return: [<opt> any-value!]
//          "Returns null if either branch returns null (unlike IF...ELSE)"
//      condition [<opt> any-value!]
//      'true-branch "If arity-1 ACTION!, receives the evaluated condition"
//          [block! action! quoted! sym-word! sym-path! sym-group! blank!]
//      'false-branch
//          [block! action! quoted! sym-word! sym-path! sym-group! blank!]
//  ]
//
REBNATIVE(either)
{
    INCLUDE_PARAMS_OF_EITHER;

    DELEGATE_WITH (
        IS_CONDITIONAL_TRUE(ARG(condition))  // fails on void, literal blocks
            ? ARG(true_branch)
            : ARG(false_branch),
        ARG(condition)  // pass condition if branch being run is an action
    );
}


inline static bool Single_Test_Throws(
    REBVAL *out, // GC-safe output cell
    const RELVAL *test,
    REBSPC *test_specifier,
    const RELVAL *arg,
    REBSPC *arg_specifier,
    REBLEN sum_quotes
){
    // Note the user could write `rule!: [integer! rule!]`, and then try to
    // `match rule! <infinite>`...have to worry about stack overflows here.
    //
    if (C_STACK_OVERFLOWING(&sum_quotes))
        Fail_Stack_Overflow();

    // We may need to add in the quotes of the dereference.  e.g.
    //
    //     >> quoted-word!: quote word!
    //     >> match ['quoted-word!] lit ''foo
    //     == ''foo
    //
    sum_quotes += VAL_NUM_QUOTES(test);

    const REBCEL *test_cell = VAL_UNESCAPED(test);
    const REBCEL *arg_cell = VAL_UNESCAPED(arg);

    enum Reb_Kind test_kind = CELL_KIND(test_cell);

    // If test is a WORD! or PATH! then GET it.  To help keep things clear,
    // require GET-WORD! or GET-PATH! for actions to convey they are not being
    // invoked inline, and disallow them on non-actions to help discern them
    // (maybe relax that later)
    //
    //    maybe [integer! :even?] 4  ; this is ok
    //    maybe [:integer! even?] 4  ; this is not
    //
    if (
        test_kind == REB_WORD
        or test_kind == REB_GET_WORD or test_kind == REB_GET_PATH
    ){
        const bool push_refinements = false;

        DECLARE_LOCAL (dequoted_test); // wouldn't need if Get took any escape
        Dequotify(Derelativize(dequoted_test, test, test_specifier));

        REBSTR *opt_label = NULL;
        REBDSP lowest_ordered_dsp = DSP;
        if (Get_If_Word_Or_Path_Throws( // !!! take any escape level?
            out,
            &opt_label,
            dequoted_test,
            SPECIFIED,
            push_refinements // !!! Look into pushing e.g. `match :foo?/bar x`
        )){
            return true;
        }

        assert(lowest_ordered_dsp == DSP); // would have made specialization
        UNUSED(lowest_ordered_dsp);

        if (IS_NULLED(out))
            fail (Error_No_Value_Raw(dequoted_test));

        if (IS_ACTION(out)) {
            if (IS_GET_WORD(dequoted_test) or IS_GET_PATH(dequoted_test)) {
                // ok
            } else
                fail ("ACTION! match rule must be GET-WORD!/GET-PATH!");
        }
        else {
            sum_quotes += VAL_NUM_QUOTES(out);
            Dequotify(out); // we want to use the dequoted version for test
        }

        test = out;
        test_cell = VAL_UNESCAPED(test);
        test_kind = CELL_KIND(test_cell);
        test_specifier = SPECIFIED;
    }

    switch (test_kind) {
      case REB_PATH: { // AND the tests together
        RELVAL *item = VAL_ARRAY_AT(test_cell);
        REBSPC *specifier = Derive_Specifier(test_specifier, test);

        for (; NOT_END(item); ++item) {
            if (IS_GET_WORD(item))
                fail ("GET-WORD! may be slated to be illegal in PATH!s");

            if (IS_QUOTED(item))
                fail ("QUOTED! items may be slated to be illegal in PATH!s");

            if (Single_Test_Throws(
                out,
                item,
                specifier,
                arg,
                arg_specifier,
                sum_quotes
            )){
                return true;
            }

            if (not VAL_LOGIC(out)) // any ANDing failing skips block
                return false; // false=no throw
        }
        assert(VAL_LOGIC(out)); // if all tests succeeded in block
        return false; } // return the LOGIC! truth, false=no throw

      case REB_BLOCK: { // OR the tests together
        RELVAL *item = VAL_ARRAY_AT(test_cell);
        REBSPC *specifier = Derive_Specifier(test_specifier, test);
        for (; NOT_END(item); ++item) {
            if (Single_Test_Throws(
                out,
                item,
                specifier,
                arg,
                arg_specifier,
                sum_quotes
            )){
                return true;
            }
            if (VAL_LOGIC(out)) // test succeeded
                return false; // return the LOGIC! truth, false=no throw
        }
        assert(not VAL_LOGIC(out));
        return false; }

      case REB_LOGIC: // test for "truthy" or "falsey"
        //
        // Note: testing a literal block for truth or falsehood could make
        // sense if the *test* varies (e.g. true or false from variable).
        //
        Init_Logic(
            out,
            VAL_LOGIC(test_cell) == IS_TRUTHY(arg) // vs IS_CONDITIONAL_TRUE()
                and VAL_NUM_QUOTES(test) == VAL_NUM_QUOTES(arg)
        );
        return false;

      case REB_ACTION: {
        DECLARE_LOCAL (arg_specified);
        Derelativize(arg_specified, arg, arg_specifier);
        Dequotify(arg_specified);  // e.g. '':refinement? wants unquoted
        PUSH_GC_GUARD(arg_specified);

        DECLARE_LOCAL (temp);  // test is in `out`
        bool threw = RunQ_Throws(
            temp,
            true,  // `fully` (ensure argument consumed)
            rebU1(KNOWN(test)),
            NULLIFY_NULLED(arg_specified),  // nulled cells to nullptr for API
            rebEND
        );

        DROP_GC_GUARD(arg_specified);
        if (threw) {
            Move_Value(out, temp);
            return true;
        }

        Init_Logic(out, IS_TRUTHY(temp));  // errors on VOID!
        return false; }

      case REB_DATATYPE:
        Init_Logic(
            out,
            VAL_TYPE_KIND(test_cell) == CELL_KIND(arg_cell)
                and VAL_NUM_QUOTES(arg) == sum_quotes
        );
        return false;

      case REB_TYPESET:
        Init_Logic(
            out,
            TYPE_CHECK(test_cell, CELL_KIND(arg_cell))
                and VAL_NUM_QUOTES(arg) == sum_quotes
        );
        return false;

     case REB_TAG: // just support <opt> for now
        Init_Logic(
            out,
            CELL_KIND(arg_cell) == REB_NULLED
            and 0 == Compare_String_Vals(test_cell, Root_Opt_Tag, true)
            and VAL_NUM_QUOTES(test) == VAL_NUM_QUOTES(arg)
        );
        return false;

      case REB_INTEGER: // interpret as length
        Init_Logic(
            out,
            ANY_SERIES_KIND(CELL_KIND(arg_cell))
                and VAL_LEN_AT(arg_cell) == VAL_UINT32(test_cell)
                and VAL_NUM_QUOTES(test) == VAL_NUM_QUOTES(arg)
        );
        return false;

      case REB_SYM_WORD: {
        //
        // !!! Currently a hack for the absence of higher-level typecheck
        // functions.  e.g. there's no way to make a typecheck that says
        // "PATH! with a BLANK! at the head".  You can only say PATH.  So
        // for now, @REFINEMENT is that test.  Generalizations of typechecks
        // should improve the state of this.
        //
        if (VAL_WORD_SYM(test_cell) == SYM_REFINEMENT_X) {
            Init_Logic(
                out,
                REB_PATH == CELL_KIND(arg_cell)
                    and IS_BLANK(ARR_AT(VAL_ARRAY(arg_cell), 0))
            );
            return false;
        }
        break; }

      default:
        break;
    }

    fail (Error_Invalid_Type(test_kind));
}


//
//  Match_Core_Throws: C
//
// MATCH is based on the idea of running a group of tests represented by
// single items.  e.g. `match 2 block` would check to see if the block was
// length 2, and `match :even? num` would pass back the value if it were even.
//
// A block can pull together these single tests.  They are OR'd by default,
// but if you use PATH! inside them then those are AND'ed.  Hence:
//
//     match [block!/2 integer!/[:even?]] value
//
// ...that would either match a block of length 2 or an even integer.
//
// In the quoted era, the concept is that match ['integer!] x would match '2.
//
// !!! Future directions may allow `match :(> 2) value` to auto-specialize a
// function to reduce it down to single arity so it can be called.
//
// !!! The choice of paths for the AND-ing rules is a bit edgy considering
// how wily paths are, but it makes sense (paths are minimum length 2, and
// no need for an AND group of length 1)...and allows for you to define a
// rule and then reuse it by reference from a word and know if it's an AND
// rule or an OR'd rule.
//
bool Match_Core_Throws(
    REBVAL *out, // GC-safe output cell
    const RELVAL *test,
    REBSPC *test_specifier,
    const RELVAL *arg,
    REBSPC *arg_specifier
){
    if (Single_Test_Throws(
        out,
        test,
        test_specifier,
        arg,
        arg_specifier,
        0 // number of quotes to add in, start at zero
    )){
        return true;
    }

    assert(IS_LOGIC(out));
    return false;
}


//
//  else: enfix native [
//
//  {If input is not null, return that value, otherwise evaluate the branch}
//
//      return: "Input value if not null, or branch result (possibly null)"
//          [<opt> any-value!]
//      optional "<deferred argument> Run branch if this is null"
//          [<opt> any-value!]
//      'branch [block! action! quoted! blank!]
//  ]
//
REBNATIVE(else)  // see `tweak :else #defer on` in %base-defs.r
{
    INCLUDE_PARAMS_OF_ELSE;  // faster than EITHER-TEST specialized w/`VALUE?`

    if (not IS_NULLED(ARG(optional)))  // Note: VOID!s are crucially non-NULL
        RETURN (ARG(optional));

    // We don't "voidify" the result, allows chaining: `else [...] then [...]`
    //
    DELEGATE (ARG(branch));
}


//
//  then: enfix native [
//
//  {If input is null, return null, otherwise evaluate the branch}
//
//      return: "null if input is null, or branch result (voided if null)"
//          [<opt> any-value!]
//      optional "<deferred argument> Run branch if this is not null"
//          [<opt> any-value!]
//      'branch "If arity-1 ACTION!, receives value that triggered branch"
//          [block! action! quoted! blank!]
//  ]
//
REBNATIVE(then)  // see `tweak :then #defer on` in %base-defs.r
{
    INCLUDE_PARAMS_OF_THEN;  // faster than EITHER-TEST specialized w/`NULL?`

    if (IS_NULLED(ARG(optional)))  // Note: VOID!s are crucially non-NULL
        return nullptr;  // left didn't run, so signal THEN didn't run either

    // We don't "voidify" the result, allows chaining: `then [...] then [...]`
    //
    DELEGATE_WITH (ARG(branch), ARG(optional));
}


//
//  also: enfix native [
//
//  {For non-null input, evaluate and discard branch (like a pass-thru THEN)}
//
//      return: "The same value as input, regardless of if branch runs"
//          [<opt> any-value!]
//      optional "<deferred argument> Run branch if this is not null"
//          [<opt> any-value!]
//      'branch "If arity-1 ACTION!, receives value that triggered branch"
//          [blank! block! action! quoted!]
//  ]
//
REBNATIVE(also)  // see `tweak :also #defer on` in %base-defs.r
{
    INCLUDE_PARAMS_OF_ALSO;  // `then func [x] [(...) :x]` => `also [...]`

    enum {
        ST_ALSO_INITIAL_ENTRY = 0,
        ST_ALSO_EVALUATING_BRANCH
    };

    switch (D_STATE_BYTE) {
      case ST_ALSO_INITIAL_ENTRY: goto initial_entry;
      case ST_ALSO_EVALUATING_BRANCH: goto branch_was_evaluated;
      default: assert(false);
    }

  initial_entry: {
    if (IS_NULLED(ARG(optional)))  // Note: VOID!s are crucially non-NULL
        return nullptr;  // telegraph original input, but don't run

    D_STATE_BYTE = ST_ALSO_EVALUATING_BRANCH;
    CONTINUE_WITH (ARG(branch), ARG(optional));
  }

  branch_was_evaluated: {
    RETURN (ARG(optional));  // ran, but pass thru the original input
  }
}


//
//  either-match: native [
//
//  {Check value using tests (match types, TRUE or FALSE, or filter action)}
//
//      return: "Input if it matched, otherwise branch result"
//          [<opt> any-value!]
//      'test "Typeset membership, LOGIC! to test for truth, filter function"
//          [
//              word!  ; GET to find actual test
//              action! get-word! get-path!  ; arity-1 filter function
//              path!  ; AND'd tests
//              block!  ; OR'd tests
//              datatype! typeset!  ; literals accepted
//              logic!  ; tests TO-LOGIC compatibility
//              tag!  ; just <opt> for now
//              integer!  ; matches length of series
//              quoted!  ; same test, but make quote level part of the test
//          ]
//       value [<opt> any-value!]
//      'branch "Branch to run on non-matches, passed VALUE if ACTION!"
//          [block! action! quoted!]
//  ]
//
REBNATIVE(either_match)
{
    INCLUDE_PARAMS_OF_EITHER_MATCH;

    if (Match_Core_Throws(D_OUT, ARG(test), SPECIFIED, ARG(value), SPECIFIED))
        return R_THROWN;

    if (VAL_LOGIC(D_OUT))
        RETURN (ARG(value));

    DELEGATE_WITH (ARG(branch), ARG(value));
}


//
//  match: native [
//
//  {Check value using tests (match types, TRUE or FALSE, or filter action)}
//
//      return: "Input if it matched, otherwise null (void if falsey match)"
//          [<opt> any-value!]
//      'test "Typeset membership, LOGIC! to test for truth, filter function"
//          [
//              word!  ; GET to find actual test
//              action! get-word! get-path!  ; arity-1 filter function
//              path!  ; AND'd tests
//              block!  ; OR'd tests
//              datatype! typeset!  ; literals accepted
//              logic!  ; tests TO-LOGIC compatibility
//              tag!  ; just <opt> for now
//              integer!  ; matches length of series
//              quoted!  ; same test, but make quote level part of the test
//          ]
//      :args [<opt> any-value! <...>]
//  ]
//
REBNATIVE(match)
//
// MATCH implements a special frame making behavior, to accomplish:
//
//     >> match parse "aaa" [some "a"]
//     == "AAA"
//
// To do this, it builds a frame for the function, steals its argument, and
// returns it.  Hence it has to be variadic.  EITHER-MATCH provides a more
// easily reusable variant of the MATCH logic (e.g. specialized by ENSURE)
//
{
    INCLUDE_PARAMS_OF_MATCH;

    // !!! The vararg's frame is not really a parent, but try to stay
    // consistent with the naming in subframe code copy/pasted for now...
    //
    REBFRM *parent;
    if (not Is_Frame_Style_Varargs_May_Fail(&parent, ARG(args)))
        fail (
            "Currently MATCH on a VARARGS! only works with a varargs"
            " which is tied to an existing, running frame--not one that is"
            " being simulated from a BLOCK! (e.g. MAKE VARARGS! [...])"
        );

    assert(Is_Action_Frame(parent));

    REBVAL *test = ARG(test);

    switch (KIND_BYTE(test)) {
      case REB_WORD:
      case REB_PATH: {
        if (NOT_CELL_FLAG(test, UNEVALUATED)) // soft quote eval'd
            goto either_match; // allow `MATCH ('NULL?) ...`

        // REBFRM whose built FRAME! context we will steal (capture DSP now)
        //
        DECLARE_FRAME (
            f, parent->feed, EVAL_MASK_DEFAULT | EVAL_FLAG_ROOT_FRAME
        );
        Push_Frame(D_OUT, f);

        REBSTR *opt_label = NULL;
        if (Get_If_Word_Or_Path_Throws(
            D_OUT,
            &opt_label,
            test,
            SPECIFIED,
            true  // push_refinements, DECLARE_FRAME() captured original DSP
        )){
            Drop_Frame(f);
            return R_THROWN;
        }

        Move_Value(test, D_OUT);

        if (not IS_ACTION(test)) {
            Drop_Frame(f);
            if (
                IS_WORD(test) or IS_GET_WORD(test) or IS_SET_WORD(test)
                or ANY_PATH(test)  // ^-- we allow ISSUE!
            ){
                fail (PAR(test)); // disallow `X: 'Y | MATCH X ...`
            }
            goto either_match; // will typecheck the result
        }

        // It was a non-soft quote eval'd word, the kind we want to give the
        // "magical" functionality to.
        //
        // We run the testing function in place in a way that appears "normal"
        // but actually captures its first argument.  That will be MATCH's
        // return value if the filter function returns a truthy result.

        REBVAL *first_arg;
        if (Make_Invocation_Frame_Throws(f, &first_arg, test)) {
            Drop_Frame(f);
            return R_THROWN;
        }

        if (not first_arg)
            fail ("MATCH with a function pattern must take at least 1 arg");

        Move_Value(D_OUT, first_arg); // steal first argument before call

        DECLARE_LOCAL (temp);
        f->out = SET_END(temp);

        f->rootvar = CTX_ARCHETYPE(CTX(f->varlist));
        f->param = ACT_PARAMS_HEAD(VAL_ACTION(test));
        f->arg = FRM_ARGS_HEAD(f);
        f->special = f->arg;  // signal only type check data

        f->flags.bits = EVAL_MASK_DEFAULT
            | EVAL_FLAG_FULLY_SPECIALIZED
            | EVAL_FLAG_ROOT_FRAME;
      #if !defined(NDEBUG)  // completely overwriting original frame's flags
        f->initial_flags = f->flags.bits;  // ...would trigger an assert
      #endif

        Begin_Prefix_Action(f, opt_label);

        bool threw = Eval_Throws(f);

        Drop_Frame(f);

        if (threw)
            return R_THROWN;

        if (IS_VOID(temp))
            fail (Error_Void_Conditional_Raw());

        // We still have the first argument from the filter call in D_OUT.

        // MATCH *wants* to pass through the argument on a match, but
        // won't do so if the argument was falsey, as that is misleading.
        // Instead it passes a VOID! back (test with `value?` or `null?`)

        if (IS_TRUTHY(temp)) {
            if (IS_FALSEY(D_OUT))
                return Init_Void(D_OUT);
            return D_OUT;
        }

        return nullptr; }

      default:
        break;
    }

  either_match:;

    // For the "non-magic" cases that are handled by plain EITHER-TEST, call
    // through with the transformed test.  Just take one normal arg via
    // variadic.

    if (Do_Vararg_Op_Maybe_End_Throws_Core(
        D_OUT,
        VARARG_OP_TAKE,
        ARG(args),
        REB_P_NORMAL
    )){
        return R_THROWN;
    }

    if (IS_END(D_OUT))
        fail ("Frame hack is written to need argument!");

    DECLARE_LOCAL (temp);
    if (Match_Core_Throws(temp, test, SPECIFIED, D_OUT, SPECIFIED))
        return R_THROWN;

    if (VAL_LOGIC(temp)) {
        if (IS_FALSEY(D_OUT)) // see above for why false match not passed thru
            return Init_Void(D_OUT);
        return D_OUT;
    }

    return nullptr;
}


//
//  matches: enfix native [
//
//  {Check value using tests (match types, TRUE or FALSE, or filter action)}
//
//      return: "Input if it matched, otherwise null (void if falsey match)"
//          [<opt> any-value!]
//       value [<opt> any-value!]
//      'test "Typeset membership, LOGIC! to test for truth, filter function"
//          [
//              word!  ; GET to find actual test
//              action! get-word! get-path!  ; arity-1 filter function
//              path!  ; AND'd tests
//              block!  ; OR'd tests
//              datatype! typeset!  ; literals accepted
//              logic!  ; tests TO-LOGIC compatibility
//              tag!  ; just <opt> for now
//              integer!  ; matches length of series
//              quoted!  ; same test, but make quote level part of the test
//          ]
//  ]
//
REBNATIVE(matches)
{
    INCLUDE_PARAMS_OF_MATCHES;

    if (Match_Core_Throws(D_OUT, ARG(test), SPECIFIED, ARG(value), SPECIFIED))
        return R_THROWN;

    assert(IS_LOGIC(D_OUT));
    return D_OUT;
}


//
//  non: native [
//
//  {Make sure a value does NOT match a type constraint (see also: ENSURE)}
//
//      return: "Input value if it passes the type test"
//          [<opt> any-value!]
//      test "The test to apply (limited to DATATYPE! and NULL at this time)"
//          [<opt> datatype!]
//      value "Value to test (will either be returned as result or error)"
//          [<opt> any-value!]
// ]
//
REBNATIVE(non)
//
// !!! This is a partial implementation of NON implemented for R3C, just good
// enough for `non void!` and `non null` cases to give validation options to
// those wanting a less permissive SET (now that it has no /ANY refinement).
{
    INCLUDE_PARAMS_OF_NON;

    REBVAL *test = ARG(test);
    REBVAL *value = ARG(value);

    if (IS_NULLED(test)) {  // not a datatype, needs special case
        if (IS_NULLED(value))
            fail ("NON expected value to not be NULL, but it was");
    }
    else if (VAL_TYPE_KIND(test) == REB_VOID) {  // specialize common case
        if (IS_VOID(value))
            fail ("NON expected value to not be VOID!, but it was");
    }
    else if (not TYPE_CHECK(value, VAL_TYPE_KIND(test))) {
        fail ("NON expected value to not match a type, but it did match");
    }

    RETURN (value);
}


//
//  all: native [
//
//  {Short-circuiting variant of AND, using a block of expressions as input}
//
//      return: "Product of last evaluation if all truthy, else null"
//          [<opt> any-value!]
//      block "Block of expressions"
//          [block!]
//  ]
//
REBNATIVE(all)
{
    INCLUDE_PARAMS_OF_ALL;

    enum {
        ST_ALL_INITIAL_ENTRY = 0,
        ST_ALL_EVAL_STEP
    };

    switch (D_STATE_BYTE) {
      case ST_ALL_INITIAL_ENTRY: goto initial_entry;
      case ST_ALL_EVAL_STEP: goto eval_step_finished;
      default: assert(false);
    }

  initial_entry: {
    REBFRM *f = Push_Continuation_At(D_OUT, ARG(block));
    assert(f == FS_TOP);  // recovered via FS_TOP on eval_step_finished
    UNUSED(f);
    Init_Nulled(D_OUT);  // so `all []` sees stale falsey value, returns null
    D_STATE_BYTE = ST_ALL_EVAL_STEP;
    return R_CONTINUATION;
  }

  eval_step_finished: {
    REBFRM *f = FS_TOP;
    assert(f->prior == D_FRAME);

    if (IS_FALSEY(D_OUT)) {  // any false/blank/null will trigger failure
        Drop_Frame(f);
        return nullptr;
    }

    if (IS_END(f->feed->value)) {
        Drop_Frame(f);
        return D_OUT;  // ALL success when the last D_OUT assignment is truthy
        // ^-- Note: might be "stale", as in `all [true elide 1 + 2]`
    }

    INIT_F_EXECUTOR(f, &New_Expression_Executor);
    assert(D_STATE_BYTE == ST_ALL_EVAL_STEP);
    return R_CONTINUATION;
  }
}


//
//  any: native [
//
//  {Short-circuiting version of OR, using a block of expressions as input}
//
//      return: "First truthy evaluative result, or null if all falsey"
//          [<opt> any-value!]
//      block "Block of expressions"
//          [block!]
//  ]
//
REBNATIVE(any)
{
    INCLUDE_PARAMS_OF_ANY;

    enum {
        ST_ANY_INITIAL_ENTRY = 0,
        ST_ANY_EVAL_STEP
    };

    switch (D_STATE_BYTE) {
      case ST_ANY_INITIAL_ENTRY: goto initial_entry;
      case ST_ANY_EVAL_STEP: goto eval_step_finished;
      default: assert(false);
    }

  initial_entry: {
    REBFRM *f = Push_Continuation_At(D_OUT, ARG(block));
    assert(f == FS_TOP);  // recovered via FS_TOP on eval_step_finished
    UNUSED(f);
    Init_Nulled(D_OUT);  // preload output with falsey value
    D_STATE_BYTE = ST_ANY_EVAL_STEP;
    return R_CONTINUATION;
  }

  eval_step_finished: {
    REBFRM *f = FS_TOP;
    assert(f->prior == D_FRAME);

    if (IS_TRUTHY(D_OUT)) {  // successful ANY returns the value
        Drop_Frame(f);
        return D_OUT;
    }

    if (IS_END(f->feed->value)) {
        Drop_Frame(f);
        return nullptr;
    }

    INIT_F_EXECUTOR(f, &New_Expression_Executor);
    assert(D_STATE_BYTE == ST_ANY_EVAL_STEP);
    return R_CONTINUATION;
  }
}


//
//  none: native [
//
//  {Short circuiting version of NOR, using a block of expressions as input}
//
//      return: "true if all expressions are falsey, null if any are truthy"
//          [<opt> logic!]
//      block "Block of expressions."
//          [block!]
//  ]
//
REBNATIVE(none)
//
// !!! In order to reduce confusion and accidents in the near term, the
// %mezz-legacy.r renames this to NONE-OF and makes NONE report an error.
{
    INCLUDE_PARAMS_OF_NONE;

    enum {
        ST_NONE_INITIAL_ENTRY = 0,
        ST_NONE_EVAL_STEP
    };

    switch (D_STATE_BYTE) {
      case ST_NONE_INITIAL_ENTRY: goto initial_entry;
      case ST_NONE_EVAL_STEP: goto eval_step_finished;
      default: assert(false);
    }

  initial_entry: {
    REBFRM *f = Push_Continuation_At(D_OUT, ARG(block));
    assert(f == FS_TOP);  // recovered via FS_TOP on eval_step_finished
    UNUSED(f);
    Init_Nulled(D_OUT);  // preload output with falsey value
    D_STATE_BYTE = ST_NONE_EVAL_STEP;
    return R_CONTINUATION;
  }

  eval_step_finished: {
    REBFRM *f = FS_TOP;
    assert(f->prior == D_FRAME);

    if (IS_TRUTHY(D_OUT)) {  // any true results mean failure
        Drop_Frame(f);
        return nullptr;
    }

    if (IS_END(f->feed->value)) {
        Drop_Frame(f);
        return Init_True(D_OUT);  // !!! suggests LOGIC! on failure, bad?
    }

    INIT_F_EXECUTOR(f, &New_Expression_Executor);
    assert(D_STATE_BYTE == ST_NONE_EVAL_STEP);
    return R_CONTINUATION;
  }
}


//
//  case: native [
//
//  {Evaluates each condition, and when true, evaluates what follows it}
//
//      return: "Last matched case evaluation, or null if no cases matched"
//          [<opt> any-value!]
//      cases "Conditions followed by branches"
//          [block!]
//      /all "Do not stop after finding first logically true case"
//      /predicate "Case pre-processor, tested for logic (default: IDENTITY)"
//          [word! path! action!]
//  ]
//
REBNATIVE(case)
{
    INCLUDE_PARAMS_OF_CASE;

    REBFRM *f;
    REBVAL *predicate = ARG(predicate);  // will be canonized on first call
    REBVAL *last_branch_result = ARG(return);  // don't need return cell
    REBVAL *predicate_label = ARG(cases);  // can reuse, `f` frame holds cases

    enum {
        ST_CASE_INITIAL_ENTRY = 0,
        ST_CASE_EVALUATING_CONDITION,
        ST_CASE_SKIPPING_GROUP_BRANCH,  // (evaluated, but not then executed)
        ST_CASE_EVALUATING_BRANCH
    };

    if (D_STATE_BYTE == ST_CASE_INITIAL_ENTRY)
        goto initial_entry;

    f = FS_TOP;
    assert(f->prior == frame_);  // !!! review this guaranteee
    assert(f->executor == nullptr);

    switch (D_STATE_BYTE) {
      case ST_CASE_EVALUATING_CONDITION: goto condition_was_evaluated;
      case ST_CASE_SKIPPING_GROUP_BRANCH: goto evaluate_new_condition;
      case ST_CASE_EVALUATING_BRANCH: goto branch_was_evaluated;
      default: assert(false);
    }

  initial_entry: {
    DECLARE_FRAME_AT (
        f_cases,  // will be named "f" on later calls
        ARG(cases),  // slot gets reused for `predicate_label`
        EVAL_MASK_DEFAULT
            | EVAL_FLAG_ALLOCATED_FEED
            | EVAL_FLAG_TRAMPOLINE_KEEPALIVE
            | EVAL_FLAG_KEEP_STALE_BIT
    );
    f = f_cases;

    if (IS_NULLED(predicate)) {
      #if !defined(NDEBUG)
        if (SPORADICALLY(5)) {  // test optimization for parity with IDENTITY
            Move_Value(predicate, NAT_VALUE(identity));  // baseline predicate
            Init_Word(predicate_label, VAL_TYPESET_STRING(PAR(predicate)));
        }
      #endif
    }
    else {
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
            fail ("PREDICATE provided to CASE must look up to an ACTION!");

        Move_Value(predicate, D_OUT);
        if (opt_label)
            Init_Word(predicate_label, opt_label);
        else  // better to say "predicate doesn't allow..." than "_ doesn't"
            Init_Word(predicate_label, VAL_TYPESET_STRING(PAR(predicate)));
    }

    Init_Nulled(last_branch_result);  // default return result

    Push_Frame(D_OUT, f);
    goto evaluate_new_condition;
  }

  evaluate_new_condition: {
    Init_Nulled(D_OUT);  // Stale value if no condition found

    if (IS_ACTION(predicate)) {
        Push_Action(f, VAL_ACTION(predicate), VAL_BINDING(predicate));
        Begin_Prefix_Action(f, VAL_WORD_SPELLING(predicate_label));
    }
    else
        INIT_F_EXECUTOR(f, &New_Expression_Executor);

    D_STATE_BYTE = ST_CASE_EVALUATING_CONDITION;
    return R_CONTINUATION;
  }

  condition_was_evaluated: {
    if (GET_CELL_FLAG(D_OUT, OUT_MARKED_STALE)) {
        CLEAR_CELL_FLAG(D_OUT, OUT_MARKED_STALE);
        goto reached_end;
    }

    if (IS_END(F_VALUE(f)))
        goto reached_end;

    if (not IS_TRUTHY(D_OUT)) {
        if (
            IS_BLOCK(F_VALUE(f))
            or IS_ACTION(F_VALUE(f))
            or IS_QUOTED(F_VALUE(f))
            or IS_SYM_WORD(F_VALUE(f))
            or IS_SYM_PATH(F_VALUE(f))
            or IS_SYM_GROUP(F_VALUE(f))
        ){
            // Acceptable "branches" (e.g. for IF), just skipped on no match
            //
            Fetch_Next_Forget_Lookback(f);
        }
        else if (IS_GROUP(F_VALUE(f))) {
            //
            // IF evaluates branches that are GROUP! even if it does not
            // run them.  This implies CASE should too.
            //
            // !!! We want to evaluate this as if it were a soft quote.  This
            // means it should do whatever that does w.r.t. enfix, etc.
            // Review implications.
            //
            f->executor = &New_Expression_Executor;

            D_STATE_BYTE = ST_CASE_SKIPPING_GROUP_BRANCH;
            return R_CONTINUATION;
        }
        else {
            // Maintain symmetry with IF on non-taken branches:
            //
            // >> if false <some-tag>
            // ** Script Error: if does not allow tag! for its branch...
            //
            fail (Error_Bad_Value_Core(F_VALUE(f), F_SPECIFIER(f)));
        }

        goto evaluate_new_condition;
    }

    // !!! This should test for legal branch types and have a message like
    // "illegal branch", which indicates the right location.  Right now it
    // allows FRAME! and the error is likely unhelpful.

    // !!! The `f` frame is on top, holding state for the enumeration.  We are
    // going to push another frame which will need its own enumeration state.
    // When that is finished we will want the "Finished_Executor".  This
    // may be the correct interpretation of `nullptr` on dispatch, and instead
    // of crashing or erroring the frame that's just there would stay there.
    //
    INIT_F_EXECUTOR(f, &Finished_Executor);

    Push_Continuation_With_Core(
        D_OUT,
        f,
        0,  // don't delegate, don't catch throws
        F_VALUE(f),  // branch
        F_SPECIFIER(f),  // branch_specifier
        END_NODE  // with
    );
    D_STATE_BYTE = ST_CASE_EVALUATING_BRANCH;
    return R_CONTINUATION;
  }

  branch_was_evaluated: {
    if (GET_CELL_FLAG(D_OUT, OUT_MARKED_STALE)) {
        assert(!"Stale value seen, should not be possible here.");
        fail ("Stale value seen, should not be possible here.");
    }

    Voidify_If_Nulled(D_OUT); // null is reserved for no branch taken

    if (not REF(all)) {
        Drop_Frame(f);
        return D_OUT;
    }

    Move_Value(last_branch_result, D_OUT);
    Fetch_Next_Forget_Lookback(f);  // keep matching if /ALL
    goto evaluate_new_condition;
  }

  reached_end: {
    Drop_Frame(f);

    // Last evaluation will "fall out" if there is no branch:
    //
    //     case /not [1 < 2 [...] 3 < 4 [...] 10 + 20] = 30
    //
    if (not IS_NULLED(D_OUT))  // prioritize fallout result
        return D_OUT;

    assert(REF(all) or IS_NULLED(last_branch_result));
    RETURN (last_branch_result);  // else last branch "falls out", may be null
  }
}


//
//  switch: native [
//
//  {Selects a choice and evaluates the block that follows it.}
//
//      return: "Last case evaluation, or null if no cases matched"
//          [<opt> any-value!]
//      value "Target value"
//          [<opt> any-value!]
//      :predicate "Binary switch-processing action (default is /EQUAL?)"
//          [refinement! action! <skip>]
//      cases "Block of cases (comparison lists followed by block branches)"
//          [block!]
//      /all "Evaluate all matches (not just first one)"
//  ]
//
REBNATIVE(switch)
{
    INCLUDE_PARAMS_OF_SWITCH;

    REBVAL *predicate = ARG(predicate);
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
            fail ("COMPARE provided to SWITCH must look up to an ACTION!");

        Move_Value(predicate, D_OUT);
    }

    DECLARE_FRAME_AT (f, ARG(cases), EVAL_MASK_DEFAULT | EVAL_FLAG_ROOT_FRAME);
    SHORTHAND (v, f->feed->value, NEVERNULL(const RELVAL*));
    SHORTHAND (specifier, f->feed->specifier, REBSPC*);

    Push_Frame(nullptr, f);
    REBVAL *last_branch_result = ARG(cases); // frame holds cases, can reuse
    Init_Nulled(last_branch_result);

    REBVAL *left = ARG(value);
    if (IS_BLOCK(left) and GET_CELL_FLAG(left, UNEVALUATED))
        fail (Error_Block_Switch_Raw(left));  // `switch [x] [...]` safeguard

    Init_Nulled(D_OUT);  // fallout result if no branches run

    while (NOT_END(*v)) {

        if (IS_BLOCK(*v) or IS_ACTION(*v)) {
            Fetch_Next_Forget_Lookback(f);
            Init_Nulled(D_OUT);  // reset fallout output to null
            continue;
        }

        // Feed the frame forward...evaluate one step to get second argument.
        //
        // NOTE: It may seem tempting to run COMPARE from the frame directly,
        // allowing it to take arity > 2.  Don't do this.  We have to get a
        // true/false answer *and* know what the right hand argument was, for
        // full switching coverage and for DEFAULT to work.
        //
        // !!! Advanced frame tricks *might* make this possible for N-ary
        // functions, the same way `match parse "aaa" [some "a"]` => "aaa"

        if (Eval_Step_Throws(SET_END(D_OUT), f))
            goto threw;

        if (IS_END(D_OUT)) {  // nothing left, or was just COMMENT/etc.
            assert(IS_END(*v));
            Drop_Frame(f);

            assert(REF(all) or IS_NULLED(last_branch_result));
            RETURN (last_branch_result);
        }

        if (IS_NULLED(predicate)) {
            //
            // It's okay that we are letting the comparison change `value`
            // here, because equality is supposed to be transitive.  So if it
            // changes 0.01 to 1% in order to compare it, anything 0.01 would
            // have compared equal to so will 1%.  (That's the idea, anyway,
            // required for `a = b` and `b = c` to properly imply `a = c`.)
            //
            // !!! This means fallout can be modified from its intent.  Rather
            // than copy here, this is a reminder to review the mechanism by
            // which equality is determined--and why it has to mutate.
            //
            // !!! A branch composed into the switch cases block may want to
            // see the un-mutated condition value.
            //
            if (not Compare_Modify_Values(left, D_OUT, 0))  // 0 => lax
                continue;
        }
        else {
            assert(IS_ACTION(predicate));  // entry code should guarantee

            // `switch x /greater? [10 [...]]` acts like `case [x > 10 [...]]
            // The ARG(value) passed in is the left/first argument to compare.
            //
            // !!! Using Run_Throws loses the labeling of the function we were
            // given (opt_label).  Consider how it might be passed through
            // for better stack traces and error messages.
            //
            // !!! We'd like to run this faster, so we aim to be able to
            // reuse this frame...hence D_SPARE should not be expected to
            // survive across this point.
            //
            DECLARE_LOCAL (temp);
            if (RunQ_Throws(
                temp,
                true,  // fully = true (e.g. both arguments must be taken)
                rebU1(predicate),
                left,  // first arg (left hand side if infix)
                D_OUT,  // second arg (right hand side if infix)
                rebEND
            )){
                goto threw;
            }
            if (IS_FALSEY(temp))
                continue;
        }

        // Skip ahead to try and find BLOCK!/ACTION! branch to take the match
        //
        while (true) {
            if (IS_END(*v))
                goto reached_end;

            if (IS_BLOCK(*v)) {  // *v is RELVAL, can't Do_Branch
                if (Do_Any_Array_At_Throws(D_OUT, *v, *specifier))
                    goto threw;
                break;
            }

            if (IS_ACTION(*v)) {  // must have been COMPOSE'd in cases
                DECLARE_LOCAL (temp);
                if (RunQ_Throws(
                    temp,
                    false,  // fully = false, e.g. arity-0 functions are ok
                    rebU1(KNOWN(*v)),  // actions don't need specifiers
                    D_OUT,
                    rebEND
                )){
                    Move_Value(D_OUT, temp);
                    goto threw;
                }
                Move_Value(D_OUT, temp);
                break;
            }

            Fetch_Next_Forget_Lookback(f);
        }

        Voidify_If_Nulled(D_OUT);  // null is reserved for no branch run

        if (not REF(all)) {
            Drop_Frame(f);
            return D_OUT;
        }

        Move_Value(last_branch_result, D_OUT);  // save in case no fallout
        Init_Nulled(D_OUT);  // switch back to using for fallout
        Fetch_Next_Forget_Lookback(f);  // keep matching if /ALL
    }

  reached_end:

    Drop_Frame(f);

    if (not IS_NULLED(D_OUT)) // prioritize fallout result
        return D_OUT;

    assert(REF(all) or IS_NULLED(last_branch_result));
    RETURN (last_branch_result);  // else last branch "falls out", may be null

  threw:;

    Drop_Frame(f);
    return R_THROWN;
}


//
//  default: enfix native/body [
//
//  {Set word or path to a default value if it is not set yet or blank.}
//
//      return: "Former value or branch result, can only be null if no target"
//          [<opt> any-value!]
//      :target "Word or path which might be set--no target always branches"
//          [<skip> set-word! set-path!]
//      'branch "If target not set already, this is evaluated and stored there"
//          [block! action! quoted!]
//      :look "Variadic lookahead used to make sure at end if no target"
//          [<...>]
//      /only "Consider target being BLANK! to be a value not to overwrite"
//  ][
//      if unset? 'target [  ; `case [... default [...]]`
//          if not tail? look [
//              fail ["DEFAULT usage with no left hand side must be at <end>"]
//          ]
//          return do :branch
//      ]
//      if set-path? target [target: compose target]
//      let gotten
//      either all [
//          value? gotten: get/hard target
//          only or [not blank? :gotten]
//      ][
//          :gotten  ; so that `x: y: default z` leads to `x = y`
//      ][
//          set/hard target do :branch
//      ]
//  ]
//
REBNATIVE(default)
{
    INCLUDE_PARAMS_OF_DEFAULT;

    REBVAL *target = ARG(target);

    enum {
        ST_DEFAULT_INITIAL_ENTRY = 0,
        ST_DEFAULT_EVALUATING_BRANCH
    };

    switch (D_STATE_BYTE) {
      case ST_DEFAULT_INITIAL_ENTRY: goto initial_entry;
      case ST_DEFAULT_EVALUATING_BRANCH: goto branch_was_evaluated;
      default: assert(false);
    }

  initial_entry: {
    if (IS_NULLED(target)) { // e.g. `case [... default [...]]`
        UNUSED(ARG(look));
        if (NOT_END(frame_->feed->value))  // !!! shortcut w/variadic for now
            fail ("DEFAULT usage with no left hand side must be at <end>");

        DELEGATE (ARG(branch));
    }

    if (IS_SET_WORD(target))
        Move_Value(D_OUT, Lookup_Word_May_Fail(target, SPECIFIED));
    else {
        assert(IS_SET_PATH(target));

        // We want to be able to default a path with groups in it, but don't
        // want to double-evaluate.  In a userspace DEFAULT we would do
        // COMPOSE on the PATH! and then use GET/HARD and SET/HARD.  To make
        // a faster native we just do a more optimal version of that.
        //
        bool has_groups = false;
        RELVAL *item = VAL_ARRAY_AT(target);
        for (; NOT_END(item); ++item) {
            if (IS_GROUP(item))
                has_groups = true;
        }
        if (has_groups) {
            REBARR *composed = Make_Array(VAL_LEN_AT(target));
            RELVAL *dest = ARR_HEAD(composed);
            item = VAL_ARRAY_AT(target);
            REBSPC *specifier = VAL_SPECIFIER(target);
            for (; NOT_END(item); ++item, ++dest) {
                if (not IS_GROUP(item))
                    Derelativize(dest, item, VAL_SPECIFIER(target));
                else {
                    // !!! This only does GROUP!s, but if there are GET-WORD!
                    // in the path the group evaluation could have side
                    // effects that change them as they go.  That's a weird
                    // edge case...not going to address it yet, as perhaps
                    // GET-WORD! in paths aren't good anyway.
                    //
                    if (Do_Any_Array_At_Throws(D_OUT, item, specifier))
                        return R_THROWN;
                    Move_Value(dest, D_OUT);
                }
            }
            TERM_ARRAY_LEN(composed, VAL_LEN_AT(target));
            Init_Any_Path(target, REB_SET_PATH, composed);
        }

        if (Eval_Path_Throws_Core(
            D_OUT,
            NULL, // not requesting symbol means refinements not allowed
            VAL_ARRAY(target),
            VAL_INDEX(target),
            VAL_SPECIFIER(target),
            NULL, // not requesting value to set means it's a get
            EVAL_MASK_DEFAULT
                | EVAL_FLAG_PATH_HARD_QUOTE  // pre-COMPOSE'd, literal GROUP!s
        )){
            panic (D_OUT); // shouldn't be possible... no executions!
        }
    }

    if (
        not IS_VOID(D_OUT)
        and not IS_NULLED(D_OUT)
        and (not IS_BLANK(D_OUT) or REF(only))
    ){
        return D_OUT;  // count it as "already set" !!! is this right?
    }

    D_STATE_BYTE = ST_DEFAULT_EVALUATING_BRANCH;
    CONTINUE (ARG(branch));
  }

  branch_was_evaluated: {
    if (IS_SET_WORD(target))
        Move_Value(Sink_Word_May_Fail(target, SPECIFIED), D_OUT);
    else {
        assert(IS_SET_PATH(target));
        DECLARE_LOCAL (dummy);
        if (Eval_Path_Throws_Core(
            dummy,
            NULL, // not requesting symbol means refinements not allowed
            VAL_ARRAY(target),
            VAL_INDEX(target),
            VAL_SPECIFIER(target),
            D_OUT,
            EVAL_MASK_DEFAULT
                | EVAL_FLAG_PATH_HARD_QUOTE  // precomposed, no double eval
        )){
            panic (dummy); // shouldn't be possible, no executions!
        }
    }
    return D_OUT;
  }
}


//
//  catch: native [
//
//  {Catches a throw from a block and returns its value.}
//
//      return: "Thrown value, or BLOCK! with value and name (if /NAME, /ANY)"
//          [<opt> any-value!]
//      block "Block to evaluate"
//          [block!]
//      /result "Evaluation result if not thrown"
//          [<output>]
//      /name "Catches a named throw (single name if not block)"
//          [block! word! action! object!]
//      /quit "Special catch for QUIT native"
//      /any "Catch all throws except QUIT (can be used with /QUIT)"
//  ]
//
REBNATIVE(catch)
//
// There's a refinement for catching quits, and CATCH/ANY will not alone catch
// it (you have to CATCH/ANY/QUIT).  Currently the label for quitting is the
// NATIVE! function value for QUIT.
{
    INCLUDE_PARAMS_OF_CATCH;

    enum {
        ST_CATCH_INITIAL_ENTRY = 0,
        ST_CATCH_EVALUATING
    };

    switch (D_STATE_BYTE) {
        case ST_CATCH_INITIAL_ENTRY: goto initial_entry;
        case ST_CATCH_EVALUATING: goto evaluation_finished;
        default: assert(false);
    }

  initial_entry: {
    //
    // /ANY would override /NAME, so point out the potential confusion
    //
    if (REF(any) and REF(name))
        fail (Error_Bad_Refines_Raw());

    D_STATE_BYTE = ST_CATCH_EVALUATING;
    CONTINUE_CATCHABLE (ARG(block));
  }

  evaluation_finished: {
    if (not Is_Throwing(D_FRAME)) {
        if (REF(result))
            rebElideQ(NAT_VALUE(set), REF(result), D_OUT, rebEND);

        return nullptr;  // no throw means just return null
    }

    const REBVAL *label = VAL_THROWN_LABEL(D_OUT);

    if (IS_ERROR(label))  // TRAP must be used to catch ERROR!-labeled FAIL
        return R_THROWN;

    if (REF(any) and not (
        IS_ACTION(label)
        and VAL_ACT_DISPATCHER(label) == &N_quit
    )){
        goto was_caught;
    }

    if (REF(quit) and (
        IS_ACTION(label)
        and VAL_ACT_DISPATCHER(label) == &N_quit
    )){
        goto was_caught;
    }

    if (REF(name)) {
        //
        // We use equal? by way of Compare_Modify_Values, and re-use the
        // refinement slots for the mutable space

        REBVAL *temp1 = ARG(quit);
        REBVAL *temp2 = ARG(any);

        if (IS_BLOCK(ARG(name))) {
            //
            // Test all the words in the block for a match to catch

            RELVAL *candidate = VAL_ARRAY_AT(ARG(name));
            for (; NOT_END(candidate); candidate++) {
                //
                // !!! Should we test a typeset for illegal name types?
                //
                if (IS_BLOCK(candidate))
                    fail (PAR(name));

                Derelativize(temp1, candidate, VAL_SPECIFIER(ARG(name)));
                Move_Value(temp2, label);

                // Return the THROW/NAME's arg if the names match
                // !!! 0 means equal?, but strict-equal? might be better
                //
                if (Compare_Modify_Values(temp1, temp2, 0))
                    goto was_caught;
            }
        }
        else {
            Move_Value(temp1, ARG(name));
            Move_Value(temp2, label);

            // Return the THROW/NAME's arg if the names match
            // !!! 0 means equal?, but strict-equal? might be better
            //
            if (Compare_Modify_Values(temp1, temp2, 0))
                goto was_caught;
        }
    }
    else {
        // Return THROW's arg only if it did not have a /NAME supplied
        //
        if (IS_NULLED(label) and (REF(any) or not REF(quit)))
            goto was_caught;
    }

    return R_THROWN;  // throw name is in D_OUT, value is held task local

  was_caught:

    if (REF(name) or REF(any)) {
        REBARR *a = Make_Array(2);

        Move_Value(ARR_AT(a, 0), label);  // throw name
        CATCH_THROWN(ARR_AT(a, 1), D_OUT);  // thrown value--may be null!
        if (IS_NULLED(ARR_AT(a, 1)))
            TERM_ARRAY_LEN(a, 1);  // trim out null value (illegal in block)
        else
            TERM_ARRAY_LEN(a, 2);
        return Init_Block(D_OUT, a);
    }

    CATCH_THROWN(D_OUT, D_OUT);  // thrown value
    return D_OUT;
  }
}


//
//  throw: native [
//
//  "Throws control back to a previous catch."
//
//      value "Value returned from catch"
//          [<opt> any-value!]
//      /name "Throws to a named catch"
//          [word! action! object!]
//  ]
//
REBNATIVE(throw)
//
// Choices are currently limited for what one can use as a "name" of a THROW.
// Note blocks as names would conflict with the `name_list` feature in CATCH.
//
// !!! Should it be /NAMED instead of /NAME?
{
    INCLUDE_PARAMS_OF_THROW;

    return Init_Thrown_With_Label(
        D_OUT,
        ARG(value),
        ARG(name)  // NULLED if unused
    );
}
