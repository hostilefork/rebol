//
//  File: %u-parse.c
//  Summary: "parse dialect interpreter"
//  Section: utility
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
// As a major operational difference from R3-Alpha, each recursion in Revolt's
// PARSE runs using a "Revolt Stack Frame"--similar to how the DO evaluator
// works.  So `[print "abc"]` and `[thru "abc"]` are both seen as "code" and
// iterated using the same mechanic.  (The rules are also locked from
// modification during the course of the PARSE, as code is in Revolt.)
//
// This leverages common services like reporting the start of the last
// "expression" that caused an error.  So merely calling `fail()` will use
// the call stack to properly indicate the start of the parse rule that caused
// a problem.  But most importantly, debuggers can break in and see the
// state at every step in the parse rule recursions.
//
// The function users see on the stack for each recursion is a native called
// SUBPARSE.  Although it is shaped similarly to typical DO code, there are
// differences.  The subparse advances the "current evaluation position" in
// the frame as it operates, so it is a variadic function...with the rules as
// the variadic parameter.  Calling it directly looks a bit unusual:
//
//     >> flags: 0
//     >> subparse "aabb" flags some "a" some "b"
//     == 4
//
// But as far as a debugging tool is concerned, the "where" of each frame
// in the call stack is what you would expect.
//
// !!! The PARSE code in R3-Alpha had gone through significant churn, and
// had a number of cautionary remarks and calls for review.  During Revolt
// development, several edge cases emerged about interactions with the
// garbage collector or throw mechanics...regarding responsibility for
// temporary values or other issues.  The code has become more clear in many
// ways, though it is also more complex due to the frame mechanics...and is
// under ongoing cleanup as time permits.
//

#include "sys-core.h"


// !!! R3-Alpha would frequently conflate indexes and flags, which could be
// confusing in the evaluator and led to many THROWN values being overlooked.
// To deal with this, a REBIXO datatype (Index-OR-a-flag) was introduced.  It
// helped transition the system to its current mechanism where there is no
// THROWN type indicator--rather a _Throws() boolean-return convention that
// chains through the stack.  PARSE is left as the only user of the datatype,
// and should also be converted to the cleaner convention.
//
#define REBIXO REBLEN
#define THROWN_FLAG ((REBLEN)(-1))
#define END_FLAG ((REBLEN)(-2))


// States for the Parse_Executor()'s STATE_BYTE().  The executor has to
// execute a C `return` with a R_CONTINUATION if it ever wants to run an
// evaluation, in order to be "stackless".  These states tell it where to
// `goto` in order to pick up where it left off when the "Trampoline" calls
// it back.  (see %c-trampoline.c)
//
enum {
    ST_PARSE_ENTRY = 0,
    ST_PARSE_PRE_RULE,
    ST_PARSE_GROUP_WAS_EVALUATED
};


//
// These macros are used to address into the frame directly to get the
// current parse rule, current input series, current parse position in that
// input series, etc.  Because the bits inside the frame arguments are
// modified as the parse runs, that means users can see the effects at
// a breakpoint.
//
// (Note: when arguments to natives are viewed under the debugger, the
// debug frames are read only.  So it's not possible for the user to change
// the ANY_SERIES! of the current parse position sitting in slot 0 into
// a DECIMAL! and crash the parse, for instance.  They are able to change
// usermode authored function arguments only.)
//

#define P_RULE              (f->feed->value + 0)  // rvalue
#define P_RULE_SPECIFIER    (f->feed->specifier + 0)  // rvalue

#define P_RETURN_VALUE      (f->rootvar + 1)

#define P_INPUT_VALUE       (f->rootvar + 2)
#define P_TYPE              VAL_TYPE(P_INPUT_VALUE)
#define P_INPUT             VAL_SERIES(P_INPUT_VALUE)
#define P_INPUT_SPECIFIER   VAL_SPECIFIER(P_INPUT_VALUE)
#define P_POS               VAL_INDEX(P_INPUT_VALUE)

#define P_FIND_FLAGS_VALUE  (f->rootvar + 3)
#define P_FIND_FLAGS        VAL_INT64(P_FIND_FLAGS_VALUE)
#define P_HAS_CASE          (did (P_FIND_FLAGS & AM_FIND_CASE))

#define P_COLLECTION_VALUE  (f->rootvar + 4)
#define P_COLLECTION \
    (IS_BLANK(P_COLLECTION_VALUE) ? nullptr : VAL_ARRAY(P_COLLECTION_VALUE))

#define P_NUM_QUOTES_VALUE  (f->rootvar + 5)
#define P_NUM_QUOTES        VAL_INT32(P_NUM_QUOTES_VALUE)

#define P_MINCOUNT_VALUE    (f->rootvar + 6)
#define P_MINCOUNT          VAL_INT32(P_MINCOUNT_VALUE)

#define P_MAXCOUNT_VALUE    (f->rootvar + 7)
#define P_MAXCOUNT          VAL_INT32(P_MAXCOUNT_VALUE)

#define P_COUNT_VALUE       (f->rootvar + 8)
#define P_COUNT             VAL_INT32(P_COUNT_VALUE)

#define P_START_VALUE       (f->rootvar + 9)
#define P_START             cast(REBLEN, VAL_INT32(P_START_VALUE))

#define P_BEGIN_VALUE       (f->rootvar + 10)
#define P_BEGIN             cast(REBLEN, VAL_INT32(P_BEGIN_VALUE))


#define P_OUT (f->out)

#define P_CELL FRM_SPARE(f)

// !!! R3-Alpha's PARSE code long predated frames, and was retrofitted to use
// them as an experiment in Revolt.  If it followed the rules of frames, then
// what is seen in a lookback is only good for *one* unit of time and may be
// invalid after that.  It takes several observations and goes back expecting
// a word to be in the same condition, so it can't use opt_lookback yet.
//
// (The evaluator pushes SET-WORD!s and SET-PATH!s to the stack in order to
// be able to reuse the frame and avoid a recursion.  This would have to do
// that as well.)
//
#define FETCH_NEXT_RULE_KEEP_LAST(opt_lookback,f) \
    *opt_lookback = P_RULE; \
    Fetch_Next_Forget_Lookback(f)

#define FETCH_NEXT_RULE(f) \
    Fetch_Next_Forget_Lookback(f)

// It's fundamental to PARSE to recognize `|` and skip ahead to it to the end.
// The debug build has enough checks on things like VAL_WORD_SPELLING() that
// it adds up when you already tested someting IS_WORD().  This reaches a
// bit lower level to try and still have protections but speed up some--and
// since there's no inlining in the debug build, FETCH_TO_BAR_OR_END=>macro
//
inline static bool IS_BAR(const RELVAL *v)
    { return IS_WORD(v) and VAL_NODE(v) == NOD(PG_Bar_Canon); }

#define FETCH_TO_BAR_OR_END(f) \
    while (NOT_END(P_RULE) and not ( \
        KIND_BYTE_UNCHECKED(P_RULE) == REB_WORD \
        and VAL_NODE(P_RULE) == NOD(PG_Bar_Canon) \
    )){ \
        FETCH_NEXT_RULE(f); \
    }


// See the notes on `flags` in the main parse loop for how these work.
//
// !!! Review if all the parse state flags can be merged into the frame
// flags...there may be few enough of them that they can, as they do not
// compete with EVAL_FLAG_XXX for the most part.  Some may also become
// not necessary with new methods of implementation.
//
enum parse_flags {
    PF_SET = 1 << 0,
    PF_COPY = 1 << 1,
    PF_NOT = 1 << 2,
    PF_NOT2 = 1 << 3,
    PF_THEN = 1 << 4,
    PF_AHEAD = 1 << 5,
    PF_REMOVE = 1 << 6,
    PF_INSERT = 1 << 7,
    PF_CHANGE = 1 << 8,
    PF_ANY_OR_SOME = 1 << 9,
    PF_ONE_RULE = 1 << 10  // signal to only run one step of the parse
};


// In %words.r, the parse words are lined up in order so they can be quickly
// filtered, skipping the need for a switch statement if something is not
// a parse command.
//
// !!! This and other efficiency tricks from R3-Alpha should be reviewed to
// see if they're really the best option.
//
inline static REBSYM VAL_CMD(const RELVAL *v) {
    REBSYM sym = VAL_WORD_SYM(v);
    if (sym >= SYM_SET and sym <= SYM_END)
        return sym;
    return SYM_0;
}


void Pack_Subparse(
    REBVAL *out,
    RELVAL *input,
    REBSPC *input_specifier,
    REBFRM *f,
    REBARR *opt_collection,
    REBFLGS flags
){
    assert(ANY_SERIES_KIND(CELL_KIND(VAL_UNESCAPED(input))));

    Push_Frame(out, f);  // checks for C stack overflow
    Push_Action(f, NAT_ACTION(subparse), UNBOUND);
    Begin_Prefix_Action(f, Canon(SYM_SUBPARSE));
    Drop_Action(f);  // keep frame around.
    INIT_F_EXECUTOR(f, &Parse_Executor);

/*   f->param = END_NODE; // informs infix lookahead
    f->arg = m_cast(REBVAL*, END_NODE);
    f->special = END_NODE; */

    Init_Nulled(Prep_Stack_Cell(P_RETURN_VALUE));  // not used

    Derelativize(Prep_Stack_Cell(P_INPUT_VALUE), input, input_specifier);

    // We always want "case-sensitivity" on binary bytes, vs. treating as
    // case-insensitive bytes for ASCII characters.
    //
    Init_Integer(Prep_Stack_Cell(P_FIND_FLAGS_VALUE), flags);

    // If there's an array for collecting into, there has to be some way of
    // passing it between frames.  We need to track the state to roll back
    // to on failure, so use the index of the array to mark the tail at the
    // time it was saved into this frame.
    //
    if (opt_collection)
        Init_Any_Array_At(
            Prep_Stack_Cell(P_COLLECTION_VALUE),
            REB_BLOCK,
            opt_collection,
            ARR_LEN(opt_collection)  // keep track of the `collect_tail`
        );
    else
        Init_Blank(Prep_Stack_Cell(P_COLLECTION_VALUE));

    // These are <local>, which means they're not things you can pass in to
    // subparse.  They are calculated from the parameters on the first entry.
    //
    Init_Nulled(Prep_Stack_Cell(P_NUM_QUOTES_VALUE));
    Init_Nulled(Prep_Stack_Cell(P_MINCOUNT_VALUE));
    Init_Nulled(Prep_Stack_Cell(P_MAXCOUNT_VALUE));
    Init_Nulled(Prep_Stack_Cell(P_COUNT_VALUE));
    Init_Nulled(Prep_Stack_Cell(P_START_VALUE));
    Init_Nulled(Prep_Stack_Cell(P_BEGIN_VALUE));

    assert(
        f->rootvar + ACT_NUM_PARAMS(NAT_ACTION(subparse)) == P_BEGIN_VALUE
    );
}


bool Unpack_Subparse_Throws(
    bool *interrupted_out,
    REBVAL *out,
    REBFRM *f,
    const REBVAL *r
){
    REBARR *opt_collection;
    REBLEN collect_tail;
    if (IS_BLANK(P_COLLECTION_VALUE)) {
        collect_tail = 0;
        opt_collection = nullptr;
    }
    else {
        collect_tail = VAL_INDEX(P_COLLECTION_VALUE);
        opt_collection = VAL_ARRAY(P_COLLECTION_VALUE);
    }

    /* Drop_Action(f); */  // now done automatically if dispatched
    Drop_Frame(f);

    if ((r == R_THROWN or IS_NULLED(out)) and opt_collection)
        TERM_ARRAY_LEN(opt_collection, collect_tail);  // roll back on abort

    if (r == R_THROWN) {
        //
        // ACCEPT and REJECT are special cases that can happen at nested parse
        // levels and bubble up through the throw mechanism to break a looping
        // construct.
        //
        // !!! R3-Alpha didn't react to these instructions in general, only in
        // the particular case where subparsing was called inside an iterated
        // construct.  Even then, it could only break through one level of
        // depth.  Most places would treat them the same as a normal match
        // or not found.  This returns the interrupted flag which is still
        // ignored by most callers, but makes that fact more apparent.
        //
        const REBVAL *label = VAL_THROWN_LABEL(out);
        if (IS_ACTION(label)) {
            if (VAL_ACTION(label) == NAT_ACTION(parse_reject)) {
                CATCH_THROWN(out, out);
                assert(IS_NULLED(out));
                *interrupted_out = true;
                return false;
            }

            if (VAL_ACTION(label) == NAT_ACTION(parse_accept)) {
                CATCH_THROWN(out, out);
                assert(IS_INTEGER(out));
                *interrupted_out = true;
                return false;
            }
        }

        return true;
    }

    assert(r == out);

    *interrupted_out = false;
    return false;
}


// Subparse_Throws() is a helper that sets up a call frame and invokes the
// SUBPARSE native--which represents one level of PARSE recursion.
//
// !!! It is the intent of Ren-C that calling functions be light and fast
// enough through Do_Va() and other mechanisms that a custom frame constructor
// like this one would not be needed.  Data should be gathered on how true
// it's possible to make that.
//
// !!! Calling subparse creates another recursion.  This recursion means
// that there are new arguments and a new frame spare cell.  Callers do not
// evaluate directly into their output slot at this time (except the top
// level parse), because most of them are framed to return other values.
//
static bool Subparse_Throws(
    bool *interrupted_out,
    REBVAL *out,
    RELVAL *input,
    REBSPC *input_specifier,
    REBFRM *f,
    REBARR *opt_collection,
    REBFLGS flags
){
    Pack_Subparse(
        out,
        input, input_specifier,
        f,
        opt_collection,
        flags
    );

    // !!! When all calls are converted to stackless, this will never do a
    // C-style recursion.
    //
    bool threw = Eval_Throws(f);

    const REBVAL *r = threw ? R_THROWN : out;

    return Unpack_Subparse_Throws(
        interrupted_out,
        out,
        f,
        r
    );
}


// Very generic errors.  Used to be parameterized with the parse rule in
// question, but now the `where` at the time of failure will indicate the
// location in the parse dialect that's the problem.

inline static REBCTX *Error_Parse_Rule(void) {
    return Error_Parse_Rule_Raw();
}

inline static REBCTX *Error_Parse_End(void) {
    return Error_Parse_End_Raw();
}

inline static REBCTX *Error_Parse_Command(REBFRM *f) {
    DECLARE_LOCAL (command);
    Derelativize(command, P_RULE, P_RULE_SPECIFIER);
    return Error_Parse_Command_Raw(command);
}

inline static REBCTX *Error_Parse_Variable(REBFRM *f) {
    DECLARE_LOCAL (variable);
    Derelativize(variable, P_RULE, P_RULE_SPECIFIER);
    return Error_Parse_Variable_Raw(variable);
}


static void Print_Parse_Index(REBFRM *f) {
    DECLARE_LOCAL (input);
    Init_Any_Series_At_Core(
        input,
        P_TYPE,
        P_INPUT,
        P_POS,
        IS_SER_ARRAY(P_INPUT)
            ? P_INPUT_SPECIFIER
            : SPECIFIED
    );

    // Either the rules or the data could be positioned at the end.  The
    // data might even be past the end.
    //
    // !!! Or does PARSE adjust to ensure it never is past the end, e.g.
    // when seeking a position given in a variable or modifying?
    //
    if (IS_END(P_RULE)) {
        if (P_POS >= SER_LEN(P_INPUT))
            rebElide("print {[]: ** END **}", rebEND);
        else
            rebElide("print [{[]:} mold", input, "]", rebEND);
    }
    else {
        DECLARE_LOCAL (rule);
        Derelativize(rule, P_RULE, P_RULE_SPECIFIER);

        if (P_POS >= SER_LEN(P_INPUT))
            rebElide("print [mold", rule, "{** END **}]", rebEND);
        else {
            rebElide("print ["
                "mold", rule, "{:} mold", input,
            "]", rebEND);
        }
    }
}


//
//  Get_Parse_Value: C
//
// Gets the value of a word (when not a command) or path.  Returns all other
// values as-is.
//
// !!! Because path evaluation does not necessarily wind up pointing to a
// variable that exists in memory, a derived value may be created.  R3-Alpha
// would push these on the stack without any corresponding drops, leading
// to leaks and overflows.  This requires you to pass in a cell of storage
// which will be good for as long as the returned pointer is used.  It may
// not be used--e.g. with a WORD! fetch.
//
static const RELVAL *Get_Parse_Value(
    REBVAL *cell,
    const RELVAL *rule,
    REBSPC *specifier
){
    if (IS_WORD(rule)) {
        if (VAL_CMD(rule))  // includes IS_BAR()...also a "command"
            return rule;

        Get_Word_May_Fail(cell, rule, specifier);
        return cell;
    }

    if (IS_PATH(rule)) {
        //
        // !!! REVIEW: how should GET-PATH! be handled?
        //
        // Should PATH!s be evaluating GROUP!s?  This does, but would need
        // to route potential thrown values up to do it properly.

        if (Get_Path_Throws_Core(cell, rule, specifier))
            fail (Error_No_Catch_For_Throw(cell));

        if (IS_NULLED(cell))
            fail (Error_No_Value_Core(rule, specifier));

        return cell;
    }

    return rule;
}


//
//  Process_Group_For_Parse: C
//
// Historically a single group in PARSE ran code, discarding the value (with
// a few exceptions when appearing in an argument position to a rule).  Revolt
// adds another behavior for GET-GROUP!, e.g. :(...).  This makes them act
// like a COMPOSE/ONLY that runs each time they are visited.
//
REB_R Process_Group_For_Parse(
    REBFRM *f,
    REBVAL *cell,
    const RELVAL *group  // may be same as `cell`
){
    // `cell` may equal `group`, read its type before Do() overwrites `cell`
    bool inject = IS_GET_GROUP(group);  // plain groups always discard

    assert(IS_GROUP(group) or IS_GET_GROUP(group));
    REBSPC *derived = Derive_Specifier(P_RULE_SPECIFIER, group);

    if (Do_Any_Array_At_Throws(cell, group, derived))
        return R_THROWN;

    // !!! The input is not locked from modification by agents other than the
    // PARSE's own REMOVE/etc.  This is a sketchy idea, but as long as it's
    // allowed, each time arbitrary user code runs, rules have to be adjusted
    //
    if (P_POS > SER_LEN(P_INPUT))
        P_POS = SER_LEN(P_INPUT);

    if (not inject or IS_NULLED(cell))  // even GET-GROUP! discards nulls
        return R_INVISIBLE;

    return cell;
}


//
//  Parse_Group_Executor: C
//
// Historically a single group in PARSE ran code, discarding the value (with
// a few exceptions when appearing in an argument position to a rule).  Ren-C
// adds another behavior for GET-GROUP!, e.g. :(...).  This makes them act
// like a COMPOSE/ONLY that runs each time they are visited.
//
REB_R Parse_Group_Executor(REBFRM *f)
{
    // We don't want to over-aggressively Derelativize() values we don't need
    // to.  So we don't pass in the GROUP! or GET-GROUP! value, just a flag.
    //
    bool inject = VAL_LOGIC(FRM_SPARE(f));  // true if it was a GET-GROUP!

    // !!! The input is not locked from modification by agents other than the
    // PARSE's own REMOVE/etc.  This is a sketchy idea, but as long as it's
    // allowed, each time arbitrary user code runs, rules have to be adjusted
    //
    if (P_POS > SER_LEN(P_INPUT))
        P_POS = SER_LEN(P_INPUT);

    if (NOT_END(f->out))
        if (not inject or IS_NULLED(f->out))  // even GET-GROUP! discards null
            SET_END(f->out);

    INIT_F_EXECUTOR(f, &Parse_Executor);  // return to prior parsing
    assert(STATE_BYTE(f) == ST_PARSE_GROUP_WAS_EVALUATED);
    assert(IS_END(f->out) or not IS_NULLED(f->out));
    return f->out;
}


//
//  Parse_One_Rule: C
//
// Used for parsing ANY-SERIES! to match the next rule in the ruleset.  If it
// matches, return the index just past it.
//
// This function is also called by To_Thru, consequently it may need to
// process elements other than the current one in the frame.  Hence it
// is parameterized by an arbitrary `pos` instead of assuming the P_POS
// that is held by the frame.
//
// The return result is either an int position, END_FLAG, or THROWN_FLAG
// Only in the case of THROWN_FLAG will f->out (aka P_OUT) be affected.
// Otherwise, it should exit the routine as an END marker (as it started);
//
static REB_R Parse_One_Rule(
    REBFRM *f,
    REBLEN pos,
    const RELVAL *rule
){
    assert(IS_END(P_OUT));

    if (IS_GROUP(rule) or IS_GET_GROUP(rule)) {
        rule = Process_Group_For_Parse(f, P_CELL, rule);
        if (rule == R_THROWN) {
            Move_Value(P_OUT, P_CELL);
            return R_THROWN;
        }
        if (rule == R_INVISIBLE) { // !!! Should this be legal?
            assert(pos <= SER_LEN(P_INPUT)); // !!! Process_Group ensures
            return Init_Integer(P_OUT, pos);
        }
        // was a GET-GROUP! :(...), use result as rule
    }

    if (Trace_Level) {
        Trace_Value("match", rule);
        Trace_Parse_Input(P_INPUT_VALUE);
    }

    if (P_POS == SER_LEN(P_INPUT)) { // at end of input
        if (IS_BLANK(rule) or IS_LOGIC(rule) or IS_BLOCK(rule)) {
            //
            // Only these types can *potentially* handle an END input.
            // For instance, `parse [] [[[_ _ _]]]` should be able to match,
            // but we have to process the block to know for sure.
        }
        else
            return R_UNHANDLED; // Other cases below can assert if item is END
    }

    switch (KIND_BYTE(rule)) { // handle rules w/same behavior for all P_INPUT

      case REB_BLANK:  // blank rules "match" but don't affect parse position
        return Init_Integer(P_OUT, pos);

      case REB_LOGIC:
        if (VAL_LOGIC(rule))
            return Init_Integer(P_OUT, pos);  // true matches always
        return R_UNHANDLED;  // false matches never

      case REB_INTEGER:
        fail ("Non-rule-count INTEGER! in PARSE must be literal, use QUOTE");

      case REB_BLOCK: {
        //
        // Process a subrule.  The subrule will run in its own frame, so it
        // will not change P_POS directly (it will have its own P_INPUT_VALUE)
        // Hence the return value regarding whether a match occurred or not
        // has to be based on the result that comes back in P_OUT.

        REBLEN pos_before = P_POS;
        P_POS = pos; // modify input position

        DECLARE_ARRAY_FEED (subfeed,
            VAL_ARRAY(rule),
            VAL_INDEX(rule),
            P_RULE_SPECIFIER
        );

        DECLARE_FRAME (
            subframe,
            subfeed,
            EVAL_MASK_DEFAULT | EVAL_FLAG_ALLOCATED_FEED
        );

        DECLARE_LOCAL (subresult);
        bool interrupted;
        if (Subparse_Throws(
            &interrupted,
            SET_END(subresult),
            P_INPUT_VALUE, // affected by P_POS assignment above
            SPECIFIED,
            subframe,
            P_COLLECTION,
            P_FIND_FLAGS & ~PF_ONE_RULE
        )){
            Move_Value(P_OUT, subresult);
            return R_THROWN;
        }

        UNUSED(interrupted); // !!! ignore "interrupted" (ACCEPT or REJECT?)

        P_POS = pos_before; // restore input position

        if (IS_NULLED(subresult))
            return R_UNHANDLED;

        REBINT index = VAL_INT32(subresult);
        assert(index >= 0);
        return Init_Integer(P_OUT, index); }

      default:;
        // Other cases handled distinctly between blocks/strings/binaries...
    }

    if (IS_SER_ARRAY(P_INPUT)) {
        REBARR *arr = ARR(P_INPUT);
        RELVAL *item = ARR_AT(arr, pos);

        switch (VAL_TYPE(rule)) {
          case REB_QUOTED:
            Derelativize(P_CELL, rule, P_RULE_SPECIFIER);
            rule = Unquotify(P_CELL, 1);
            break; // fall through to direct match

          case REB_DATATYPE:
            if (VAL_TYPE(item) == VAL_TYPE_KIND(rule))
                return Init_Integer(P_OUT, pos + 1); // specific type match
            return R_UNHANDLED;

          case REB_TYPESET:
            if (TYPE_CHECK(rule, VAL_TYPE(item)))
                return Init_Integer(P_OUT, pos + 1); // type was in typeset
            return R_UNHANDLED;

          case REB_WORD:
            if (VAL_WORD_SYM(rule) == SYM_LIT_WORD_X) { // hack for lit-word!
                if (IS_QUOTED_WORD(item))
                    return Init_Integer(P_OUT, pos + 1);
                return R_UNHANDLED;
            }
            if (VAL_WORD_SYM(rule) == SYM_LIT_PATH_X) { // hack for lit-path!
                if (IS_QUOTED_PATH(item))
                    return Init_Integer(P_OUT, pos + 1);
                return R_UNHANDLED;
            }
            if (VAL_WORD_SYM(rule) == SYM_REFINEMENT_X) { // another hack...
                if (IS_REFINEMENT(item))
                    return Init_Integer(P_OUT, pos + 1);
                return R_UNHANDLED;
            }
            fail (Error_Parse_Rule());

          default:
            break;
        }

        // !!! R3-Alpha said "Match with some other value"... is this a good
        // default?!
        //
        if (Cmp_Value(item, rule, P_HAS_CASE) == 0)
            return Init_Integer(P_OUT, pos + 1);

        return R_UNHANDLED;
    }
    else {
        assert(ANY_STRING_KIND(P_TYPE) or P_TYPE == REB_BINARY);

        // We try to allow some conveniences when parsing strings based on
        // how items render, e.g.:
        //
        //     >> did parse "ab<c>10" ['ab <c> '10]
        //     == #[true]
        //
        // It can be less visually noisy than:
        //
        //     >> did parse "ab<c>10" ["ab" {<c>} "10"]
        //     == #[true]
        //
        // !!! The concept is based somewhat on what was legal in FIND for
        // Rebol2, and leverages quoting.  It's being experimented with.
        //
        const REBCEL *rule_cell = VAL_UNESCAPED(rule);
        enum Reb_Kind rule_cell_kind = CELL_KIND(rule_cell);
        if (
            (ANY_WORD_KIND(rule_cell_kind) and VAL_NUM_QUOTES(rule) == 1)
            or (ANY_STRING_KIND(rule_cell_kind) and VAL_NUM_QUOTES(rule) <= 1)
            or (rule_cell_kind == REB_BINARY and VAL_NUM_QUOTES(rule) == 0)
            or (rule_cell_kind == REB_INTEGER and VAL_NUM_QUOTES(rule) == 1)
        ){
            REBLEN len;
            REBLEN index = Find_In_Any_Sequence(
                &len,
                P_INPUT_VALUE,
                rule_cell,
                P_FIND_FLAGS | AM_FIND_MATCH
            );
            if (index == NOT_FOUND)
                return R_UNHANDLED;
            return Init_Integer(P_OUT, index + len);
        }
        else switch (VAL_TYPE(rule)) {
          case REB_CHAR:
            if (P_TYPE == REB_BINARY) {
                //
                // See if current binary position matches UTF-8 encoded char
                //
                if (P_POS + VAL_CHAR_ENCODED_SIZE(rule) > BIN_LEN(P_INPUT))
                    return R_UNHANDLED;

                const REBYTE *ep = VAL_CHAR_ENCODED(rule);
                assert(*ep != 0);
                const REBYTE *bp = BIN_AT(P_INPUT, P_POS);
                do {
                    if (*ep++ != *bp++)
                        return R_UNHANDLED;
                } while (*ep);

                return Init_Integer(
                    P_OUT,
                    P_POS + VAL_CHAR_ENCODED_SIZE(rule)
                );
            }

            // Otherwise it's a string and may have case sensitive behavior.
            //
            // !!! Could this unify with above method for binary, somehow?

            if (P_HAS_CASE) {
                if (VAL_CHAR(rule) != GET_CHAR_AT(STR(P_INPUT), P_POS))
                    return R_UNHANDLED;
            }
            else {
                if (
                    UP_CASE(VAL_CHAR(rule))
                    != UP_CASE(GET_CHAR_AT(STR(P_INPUT), P_POS))
                ){
                    return R_UNHANDLED;
                }
            }
            return Init_Integer(P_OUT, P_POS + 1);

          case REB_BITSET: {
            //
            // Check current char/byte against character set, advance matches
            //
            bool uncased;
            REBUNI uni;
            if (P_TYPE == REB_BINARY) {
                uni = *BIN_AT(P_INPUT, P_POS);
                uncased = false;
            }
            else {
                uni = GET_CHAR_AT(STR(P_INPUT), P_POS);
                uncased = not P_HAS_CASE;
            }

            if (Check_Bit(VAL_BITSET(rule), uni, uncased))
                return Init_Integer(P_OUT, P_POS + 1);

            return R_UNHANDLED; }

          case REB_TYPESET:
          case REB_DATATYPE: {
            REBSTR *filename = Canon(SYM___ANONYMOUS__);

            REBLIN start_line = 1;

            REBSIZ size;
            const REBYTE *bp = VAL_BYTES_AT(&size, P_INPUT_VALUE);

            SCAN_LEVEL level;
            SCAN_STATE ss;
            Init_Scan_Level(&level, &ss, filename, start_line, bp, size);
            level.opts |= SCAN_FLAG_NEXT;  // _ONLY?

            REBDSP dsp_orig = DSP;
            if (Scan_To_Stack_Relaxed_Failed(&level)) {
                DS_DROP();
                return R_UNHANDLED;
            }

            if (DSP == dsp_orig)
                return R_UNHANDLED;  // nothing was scanned

            assert(DSP == dsp_orig + 1);  // only adds one value to stack

            enum Reb_Kind kind = VAL_TYPE(DS_TOP);
            if (IS_DATATYPE(rule)) {
                if (kind != VAL_TYPE_KIND(rule)) {
                    DS_DROP();
                    return R_UNHANDLED;
                }
            }
            else {
                if (not TYPE_CHECK(rule, kind)) {
                    DS_DROP();
                    return R_UNHANDLED;
                }
            }

            // !!! We need the caller to know both the updated position in
            // the text string -and- be able to get the value.  It's already
            // on the data stack, so use that as the method to pass it back,
            // but put the position after the match in P_OUT.

            if (IS_BINARY(P_INPUT_VALUE))
                Init_Integer(P_OUT, P_POS + (ss.end - bp));
            else
                Init_Integer(
                    P_OUT,
                    P_POS + Num_Codepoints_For_Bytes(bp, ss.end)
                );

            return R_IMMEDIATE; }  // produced value in DS_TOP

          default:
            fail (Error_Parse_Rule());
        }
    }
}


//
//  To_Thru_Block_Rule: C
//
// The TO and THRU keywords in PARSE do not necessarily match the direct next
// item, but scan ahead in the series.  This scan may be successful or not,
// and how much the match consumes can vary depending on how much THRU
// content was expressed in the rule.
//
// !!! This routine from R3-Alpha is fairly circuitous.  As with the rest of
// the code, it gets clarified in small steps.
//
static REBIXO To_Thru_Block_Rule(
    REBFRM *f,
    const RELVAL *rule_block,
    bool is_thru
) {
    DECLARE_LOCAL (cell); // holds evaluated rules (use frame cell instead?)

    // Note: This enumeration goes through <= SER_LEN(P_INPUT), because the
    // block rule might be something like `to [{a} | end]`.  e.g. being
    // positioned on the end cell or null terminator of a string may match.
    //
    REBLEN pos = P_POS;
    for (; pos <= SER_LEN(P_INPUT); ++pos) {  // see note
        const RELVAL *blk = VAL_ARRAY_HEAD(rule_block);
        for (; NOT_END(blk); blk++) {
            if (IS_BAR(blk))
                fail (Error_Parse_Rule()); // !!! Shouldn't `TO [|]` succeed?

            const RELVAL *rule;
            if (not (IS_GROUP(blk) or IS_GET_GROUP(blk)))
                rule = blk;
            else {
                rule = Process_Group_For_Parse(f, cell, blk);
                if (rule == R_THROWN) {
                    Move_Value(P_OUT, cell);
                    return THROWN_FLAG;
                }
                if (rule == R_INVISIBLE)
                    continue;
            }

            if (IS_WORD(rule)) {
                REBSYM cmd = VAL_CMD(rule);

                if (cmd != SYM_0) {
                    if (cmd == SYM_END) {
                        if (pos >= SER_LEN(P_INPUT))
                            return SER_LEN(P_INPUT);
                        goto next_alternate_rule;
                    }
                    else if (
                        cmd == SYM_LIT or cmd == SYM_LITERAL
                        or cmd == SYM_QUOTE // temporarily same for bootstrap
                    ){
                        rule = ++blk; // next rule is the literal value
                        if (IS_END(rule))
                            fail (Error_Parse_Rule());
                    }
                    else
                        fail (Error_Parse_Rule());
                }
                else {
                    Get_Word_May_Fail(cell, rule, P_RULE_SPECIFIER);
                    rule = cell;
                }
            }
            else if (IS_PATH(rule))
                rule = Get_Parse_Value(cell, rule, P_RULE_SPECIFIER);

            // Try to match it:
            if (ANY_ARRAY_OR_PATH_KIND(P_TYPE)) {
                if (ANY_ARRAY(rule))
                    fail (Error_Parse_Rule());

                REB_R r = Parse_One_Rule(f, pos, rule);
                if (r == R_THROWN)
                    return THROWN_FLAG;

                if (r == R_UNHANDLED) {
                    // fall through, keep looking
                    SET_END(P_OUT);
                }
                else {  // P_OUT is pos we matched past, so back up if only TO
                    assert(r == P_OUT);
                    pos = VAL_INT32(P_OUT);
                    SET_END(P_OUT);
                    if (is_thru)
                        return pos;  // don't back up
                    return pos - 1;  // back up
                }
            }
            else if (P_TYPE == REB_BINARY) {
                REBYTE ch1 = *BIN_AT(P_INPUT, pos);

                if (pos == SER_LEN(P_INPUT)) {
                    //
                    // If we weren't matching END, then the only other thing
                    // we'll match at the BINARY! end is an empty BINARY!.
                    // Not a NUL codepoint, because the internal BINARY!
                    // terminator is implementation detail.
                    //
                    assert(ch1 == '\0');  // internal BINARY! terminator
                    if (IS_BINARY(rule) and VAL_LEN_AT(rule) == 0)
                        return pos;
                }
                else if (IS_CHAR(rule)) {
                    if (VAL_CHAR(rule) > 0xff)
                        fail (Error_Parse_Rule());

                    if (ch1 == VAL_CHAR(rule)) {
                        if (is_thru)
                            return pos + 1;
                        return pos;
                    }
                }
                else if (IS_BINARY(rule)) {
                    REBLEN len = VAL_LEN_AT(rule);
                    if (0 == Compare_Bytes(
                        BIN_AT(P_INPUT, pos),
                        VAL_BIN_AT(rule),
                        len,
                        false
                    )) {
                        if (is_thru)
                            return pos + 1;
                        return pos;
                    }
                }
                else if (IS_INTEGER(rule)) {
                    if (VAL_INT64(rule) > 0xff)
                        fail (Error_Parse_Rule());

                    if (ch1 == VAL_INT32(rule)) {
                        if (is_thru)
                            return pos + 1;
                        return pos;
                    }
                }
                else
                    fail (Error_Parse_Rule());
            }
            else {
                assert(ANY_STRING_KIND(P_TYPE));

                REBUNI ch_unadjusted = GET_CHAR_AT(STR(P_INPUT), pos);
                if (ch_unadjusted == '\0') { // cannot be passed to UP_CASE()
                    assert(pos == SER_LEN(P_INPUT));

                    if (IS_TEXT(rule) and VAL_LEN_AT(rule) == 0)
                        return pos;  // empty string can match at end

                    goto next_alternate_rule;  // other match is END (above)
                }

                REBUNI ch;
                if (!P_HAS_CASE)
                    ch = UP_CASE(ch_unadjusted);
                else
                    ch = ch_unadjusted;

                if (IS_CHAR(rule)) {
                    REBUNI ch2 = VAL_CHAR(rule);
                    if (ch2 == 0)
                        goto next_alternate_rule;  // no 0 char in ANY-STRING!

                    if (!P_HAS_CASE)
                        ch2 = UP_CASE(ch2);
                    if (ch == ch2) {
                        if (is_thru)
                            return pos + 1;
                        return pos;
                    }
                }
                else if (IS_BITSET(rule)) {
                    if (Check_Bit(VAL_BITSET(rule), ch, not P_HAS_CASE)) {
                        if (is_thru)
                            return pos + 1;
                        return pos;
                    }
                }
                else if (IS_TAG(rule)) {
                    if (ch == '<') {
                        //
                        // !!! This code was adapted from Parse_to, and is
                        // inefficient in the sense that it forms the tag
                        //
                        REBSTR *formed = Copy_Form_Value(rule, 0);
                        REBLEN len = STR_LEN(formed);
                        const REBINT skip = 1;
                        REBLEN i = Find_Str_In_Str(
                            STR(P_INPUT),
                            pos,
                            SER_LEN(P_INPUT),
                            skip,
                            formed,
                            0,
                            len,
                            AM_FIND_MATCH | P_FIND_FLAGS
                        );
                        Free_Unmanaged_Series(SER(formed));
                        if (i != NOT_FOUND) {
                            if (is_thru)
                                return pos + len;
                            return pos;
                        }
                    }
                }
                else if (ANY_STRING(rule)) {
                    REBLEN len = VAL_LEN_AT(rule);
                    const REBINT skip = 1;
                    REBLEN i = Find_Str_In_Str(
                        STR(P_INPUT),
                        pos,
                        SER_LEN(P_INPUT),
                        skip,
                        VAL_STRING(rule),
                        VAL_INDEX(rule),
                        len,
                        AM_FIND_MATCH | P_FIND_FLAGS
                    );

                    if (i != NOT_FOUND) {
                        if (is_thru)
                            return i + len;
                        return i;
                    }
                }
                else if (IS_INTEGER(rule)) {
                    if (ch_unadjusted == cast(REBUNI, VAL_INT32(rule))) {
                        if (is_thru)
                            return pos + 1;
                        return pos;
                    }
                }
                else
                    fail (Error_Parse_Rule());
            }

          next_alternate_rule:  // alternates are BAR! separated `[a | b | c]`

            do {
                ++blk;
                if (IS_END(blk))
                    goto next_input_position;
            } while (not IS_BAR(blk));
        }

      next_input_position:; // not matched yet, so keep trying to go THRU or TO
    }
    return END_FLAG;
}


//
//  To_Thru_Non_Block_Rule: C
//
// There's a high-level split between block and non-block rule processing,
// as blocks are the common case.
//
static REBIXO To_Thru_Non_Block_Rule(
    REBFRM *f,
    const RELVAL *rule,
    bool is_thru
){
    REBYTE kind = KIND_BYTE(rule);
    assert(kind != REB_BLOCK);

    if (kind == REB_BLANK)
        return P_POS; // make it a no-op

    if (kind == REB_LOGIC) // no-op if true, match failure if false
        return VAL_LOGIC(rule) ? P_POS : END_FLAG;

    if (kind == REB_WORD and VAL_WORD_SYM(rule) == SYM_END) {
        //
        // `TO/THRU END` JUMPS TO END INPUT SERIES (ANY SERIES TYPE)
        //
        return SER_LEN(P_INPUT);
    }

    if (IS_SER_ARRAY(P_INPUT)) {
        //
        // FOR ARRAY INPUT WITH NON-BLOCK RULES, USE Find_In_Array()
        //
        // !!! This adjusts it to search for non-literal words, but are there
        // other considerations for how non-block rules act with array input?
        //
        REBFLGS flags = P_HAS_CASE ? AM_FIND_CASE : 0;
        DECLARE_LOCAL (temp);
        if (IS_QUOTED(rule)) { // make `'[foo bar]` match `[foo bar]`
            Derelativize(temp, rule, P_RULE_SPECIFIER);
            rule = Unquotify(temp, 1);
            flags |= AM_FIND_ONLY; // !!! Is this implied?
        }

        REBLEN i = Find_In_Array(
            ARR(P_INPUT),
            P_POS,
            SER_LEN(P_INPUT),
            rule,
            1,
            flags,
            1
        );

        if (i == NOT_FOUND)
            return END_FLAG;

        if (is_thru)
            return i + 1;

        return i;
    }

    //=//// PARSE INPUT IS A STRING OR BINARY, USE A FIND ROUTINE /////////=//

    REBLEN len;  // e.g. if a TAG!, match length includes < and >
    REBLEN i = Find_In_Any_Sequence(
        &len,
        P_INPUT_VALUE,
        rule,
        P_FIND_FLAGS
    );

    if (i == NOT_FOUND)
        return END_FLAG;

    if (is_thru)
        return i + len;

    return i;
}


//
//  Do_Eval_Rule: C
//
// Perform an EVALAUTE on the *input* as a code block, and match the following
// rule against the evaluative result.
//
//     parse [1 + 2] [do [lit 3]] => true
//
// The rule may be in a block or inline.
//
//     parse [reverse copy "abc"] [do "cba"]
//     parse [reverse copy "abc"] [do ["cba"]]
//
// !!! Since this only does one step, it no longer corresponds to DO as a
// name, and corresponds to EVALUATE.
//
// !!! Due to failures in the mechanics of "Parse_One_Rule", a block must
// be used on rules that are more than one item in length.
//
// This feature was added to make it easier to do dialect processing where the
// dialect had code inline.  It can be a little hard to get one's head around,
// because it says `do [...]` and yet the `...` is a parse rule and not the
// code to be executed.  But this is somewhat in the spirit of operations
// like COPY which are not operating on their arguments, but implicitly taking
// the series itself as an argument.
//
// !!! The way this feature was expressed in R3-Alpha isolates it from
// participating in iteration or as the target of an outer rule, e.g.
//
//     parse [1 + 2] [set var do [lit 3]]  ; var gets 1, not 3
//
// Other problems arise since the caller doesn't know about the trickiness
// of this evaluation, e.g. this won't work either:
//
//     parse [1 + 2] [thru do integer!]
//
static REBIXO Do_Eval_Rule(REBFRM *f)
{
    if (not IS_SER_ARRAY(P_INPUT)) // can't be an ANY-STRING!
        fail (Error_Parse_Rule());

    if (IS_END(P_RULE))
        fail (Error_Parse_End());

    // The DO'ing of the input series will generate a single REBVAL.  But
    // for a parse to run on some input, that input has to be in a series...
    // so the single item is put into a block holder.  If the item was already
    // a block, then the user will have to use INTO to parse into it.
    //
    // Note: Implicitly handling a block evaluative result as an array would
    // make it impossible to tell whether the evaluation produced [1] or 1.
    //
    REBARR *holder;

    REBLEN index;
    if (P_POS >= SER_LEN(P_INPUT)) {
        //
        // We could short circuit and notice if the rule was END or not, but
        // that leaves out other potential matches like `[(print "Hi") end]`
        // as a rule.  Keep it generalized and pass an empty block in as
        // the series to process.
        //
        holder = EMPTY_ARRAY; // read-only
        index = 0xDECAFBAD;  // shouldn't be used, avoid compiler warning
        SET_END(P_CELL);
    }
    else {
        // Evaluate next expression from the *input* series (not the rules)
        //
        if (Eval_Step_In_Any_Array_At_Throws(
            P_CELL,
            &index,
            P_INPUT_VALUE,
            P_INPUT_SPECIFIER,
            EVAL_MASK_DEFAULT
        )){
            Move_Value(P_OUT, P_CELL);  // BREAK/RETURN/QUIT/THROW...
            return THROWN_FLAG;
        }

        // !!! This copies a single value into a block to use as data, because
        // parse input is matched as a series.  Can this be avoided?
        //
        holder = Alloc_Singular(SERIES_FLAGS_NONE);
        Move_Value(ARR_SINGLE(holder), P_CELL);
        Deep_Freeze_Array(holder); // don't allow modification of temporary
    }

    // We want to reuse the same frame we're in, because if you say
    // something like `parse [1 + 2] [do [lit 3]]`, the `[lit 3]` rule
    // should be consumed.  We also want to be able to use a nested rule
    // inline, such as `do skip` not only allow `do [skip]`.
    //
    // So the rules should be processed normally, it's just that for the
    // duration of the next rule the *input* is the temporary evaluative
    // result.
    //
    DECLARE_LOCAL (saved_input);
    Move_Value(saved_input, P_INPUT_VALUE); // series and P_POS position
    PUSH_GC_GUARD(saved_input);
    Init_Block(P_INPUT_VALUE, holder);

    // !!! There is not a generic form of SUBPARSE/NEXT, but there should be.
    // The particular factoring of the one-rule form of parsing makes us
    // redo work like fetching words/paths, which should not be needed.
    //
    DECLARE_LOCAL (cell);
    const RELVAL *rule = Get_Parse_Value(cell, P_RULE, P_RULE_SPECIFIER);

    // !!! The actual mechanic here does not permit you to say `do thru x`
    // or other multi-argument things.  A lot of R3-Alpha's PARSE design was
    // rather ad-hoc and hard to adapt.  The one rule parsing does not
    // advance the position, but it should.
    //
    REB_R r = Parse_One_Rule(f, P_POS, rule);
    assert(r != R_IMMEDIATE);  // parse "1" [integer!], only for string input
    FETCH_NEXT_RULE(f);

    // Restore the input series to what it was before parsing the temporary
    // (this restores P_POS, since it's just an alias for the input's index)
    //
    Move_Value(P_INPUT_VALUE, saved_input);
    DROP_GC_GUARD(saved_input);

    if (r == R_THROWN)
        return THROWN_FLAG;

    if (r == R_UNHANDLED) {
        SET_END(P_OUT); // preserve invariant
        return P_POS;  // as failure, hand back original, no advancement
    }

    REBLEN n = VAL_INT32(P_OUT);
    SET_END(P_OUT);  // preserve invariant
    if (n == ARR_LEN(holder)) {
        //
        // Eval result reaching end means success, so return index advanced
        // past the evaluation.
        //
        // !!! Though Eval_Step_In_Any_Array_At_Throws() uses an END cell to
        // communicate reaching the end, these parse routines always return
        // an array index.
        //
        return IS_END(P_CELL) ? SER_LEN(P_INPUT) : index;
    }

    return P_POS; // as failure, hand back original position--no advancement
}


// This handles marking positions, either as plain `pos:` the SET-WORD! rule,
// or the newer `mark pos` rule.  Handles WORD! and PATH!.
//
static void Handle_Mark_Rule(
    REBFRM *f,
    const RELVAL *rule,
    REBSPC *specifier
){
    //
    // !!! Experiment: Put the quote level of the original series back on when
    // setting positions (then remove)
    //
    //     parse lit '''{abc} ["a" mark x:]` => '''{bc}

    Quotify(P_INPUT_VALUE, P_NUM_QUOTES);

    REBYTE k = KIND_BYTE(rule);  // REB_0_END ok
    if (k == REB_WORD or k == REB_SET_WORD) {
        Move_Value(
            Sink_Word_May_Fail(rule, specifier),
            P_INPUT_VALUE
        );
    }
    else if (k == REB_PATH or k == REB_SET_PATH) {
        if (Set_Path_Throws_Core(
            P_OUT, rule, specifier, P_INPUT_VALUE
        )){
            fail (Error_No_Catch_For_Throw(P_OUT));
        }
    }
    else
        fail (Error_Parse_Variable(f));

    Dequotify(P_INPUT_VALUE);  // go back to 0 quote level
}


static REB_R Handle_Seek_Rule_Dont_Update_Begin(
    REBFRM *f,
    const RELVAL *rule,
    REBSPC *specifier
){
    REBYTE k = KIND_BYTE(rule);  // REB_0_END ok
    if (k == REB_WORD or k == REB_GET_WORD) {
        rule = Lookup_Word_May_Fail(rule, specifier);
        k = KIND_BYTE(rule);
    }
    else if (k == REB_PATH) {
        if (Get_Path_Throws_Core(P_CELL, rule, specifier))
            fail (Error_No_Catch_For_Throw(P_CELL));
        rule = P_CELL;
        k = KIND_BYTE(rule);
    }

    REBINT index;
    if (k == REB_INTEGER) {
        index = VAL_INT32(rule);
        if (index < 1)
            fail ("Cannot SEEK a negative integer position");
        --index;  // Rebol is 1-based, C is 0 based...
    }
    else if (ANY_SERIES_KIND(k)) {
        if (VAL_SERIES(rule) != P_INPUT)
            fail ("Switching PARSE series is not allowed");
        index = VAL_INDEX(rule);
    }
    else {  // #1263
        DECLARE_LOCAL (specific);
        Derelativize(specific, rule, P_RULE_SPECIFIER);
        fail (Error_Parse_Series_Raw(specific));
    }

    if (cast(REBLEN, index) > SER_LEN(P_INPUT))
        P_POS = SER_LEN(P_INPUT);
    else
        P_POS = index;

    return R_INVISIBLE;
}

// !!! Note callers will `continue` without any post-"match" processing, so
// the only way `begin` will get set for the next rule is if they set it,
// else commands like INSERT that follow will insert at the old location.
//
// https://github.com/rebol/rebol-issues/issues/2269
//
// Without known resolution on #2269, it isn't clear if there is legitimate
// meaning to seeking a parse in mid rule or not.  So only reset the begin
// position if the seek appears to be a "separate rule" in its own right.
//
#define HANDLE_SEEK_RULE_UPDATE_BEGIN(f,rule,specifier) \
    Handle_Seek_Rule_Dont_Update_Begin((f), (rule), (specifier)); \
    if (flags == 0) \
        Init_Integer(P_BEGIN_VALUE, P_POS);


//
//  subparse: native [
//
//  {Internal support function for PARSE (acts as variadic to consume rules)}
//
//      return: [<opt> integer!]
//      input [any-series! any-array! quoted!]
//      find-flags [integer!]
//      collection "Array into which any KEEP values are collected"
//          [blank! any-series!]
//      <local> num-quotes mincount maxcount count start begin
//  ]
//
REBNATIVE(subparse)
//
// Rules are matched until one of these things happens:
//
// * A rule fails, and is not then picked up by a later "optional" rule.
// This returns NULL.
//
// * You run out of rules to apply without any failures or errors, and the
// position in the input series is returned.  This may be at the end of
// the input data or not--it's up to the caller to decide if that's relevant.
// This will return D_OUT with out containing an integer index.
//
// !!! The return of an integer index is based on the R3-Alpha convention,
// but needs to be rethought in light of the ability to switch series.  It
// does not seem that all callers of Subparse's predecessor were prepared for
// the semantics of switching the series.
//
// * A `fail()`, in which case the function won't return--it will longjmp
// up to the most recently pushed handler.  This can happen due to an invalid
// rule pattern, or if there's an error in code that is run in parentheses.
//
// * A throw-style result caused by DO code run in parentheses (e.g. a
// THROW, RETURN, BREAK, CONTINUE).  This returns a thrown value.
//
// * A special throw to indicate a return out of the PARSE itself, triggered
// by the RETURN instruction.  This also returns a thrown value, but will
// be caught by PARSE before returning.
{
    UNUSED(frame_);
    assert(!"SUBPARSE temporarily out of commission.");
    return nullptr;
}


//
//  Parse_Executor: C
//
REB_R Parse_Executor(REBFRM *frame_) {
    INCLUDE_PARAMS_OF_SUBPARSE;
    CLEAR_SERIES_INFO(frame_->varlist, HOLD);  // remove bit

    UNUSED(ARG(input));  // used via P_INPUT
    UNUSED(ARG(find_flags));  // used via P_FIND_FLAGS
    UNUSED(ARG(num_quotes));  // used via P_NUM_QUOTES_VALUE

    REBFRM *f = frame_; // nice alias of implicit native parameter

    // Every time we hit an alternate rule match (with |), we have to reset
    // any of the collected values.  Remember the tail when we started.
    //
    // !!! Could use the VAL_INDEX() of ARG(collect) for this
    //
    // !!! How this interplays with throws that might be caught before the
    // COLLECT's stack level is not clear (mostly because ACCEPT and REJECT
    // were not clear; many cases dropped them on the floor in R3-Alpha, and
    // no real resolution exists...see the UNUSED(interrupted) cases.)
    //
    REBLEN collection_tail = P_COLLECTION ? VAL_INDEX(P_COLLECTION_VALUE) : 0;
    UNUSED(ARG(collection)); // implicitly accessed as P_COLLECTION

  #if !defined(NDEBUG)
    //
    // These parse state variables live in chunk-stack REBVARs, which can be
    // annoying to find to inspect in the debugger.  This makes pointers into
    // the value payloads so they can be seen more easily.
    //
    const REBLEN *pos_debug = &P_POS;
    (void)pos_debug; // UNUSED() forces corruption in C++11 debug builds
  #endif

    DECLARE_LOCAL (save);

    // start w/rule in block, may eval/fetch
    const RELVAL *rule;
    TRASH_POINTER_IF_DEBUG(rule);  // all entry points must set

    // For now assume we do not carry the set or copy word across an eval.
    // !!! This is likely not true.  :-/
    //
    REBFLGS flags = 0;
    const RELVAL *set_or_copy_word = NULL;

    if (D_STATE_BYTE == ST_PARSE_ENTRY) {
        //
        // If input is quoted, e.g. `parse lit ''''[...] [rules]`, we dequote
        // it while processing the ARG().  This is because we are trying to
        // update and maintain the value as we work in a way that can be shown
        // in the debug stack frame.  Calling VAL_UNESCAPED() constantly would
        // be slower, and gives back a const value which may be shared with
        // other quoted instances, so we can't update VAL_INDEX() directly.
        //
        // But we save the number of quotes in a local variable.  This way we
        // can put the quotes back on whenever doing a COPY etc.
        //
        Init_Integer(P_NUM_QUOTES_VALUE, VAL_NUM_QUOTES(P_INPUT_VALUE));
        Dequotify(P_INPUT_VALUE);

        // Mincount and maxcount may need to carry a value across calls.
        // They are reset at the main continuation.
        //
        Init_Integer(P_MINCOUNT_VALUE, 1);
        Init_Integer(P_MAXCOUNT_VALUE, 1);
        Init_Unreadable_Blank(P_COUNT_VALUE);  // make sure it's applicable

        // Capture the `start` of the subparse where it was at the beginning.
        // (The P_POS is done using feeds, so the error reporting and debug
        // will report the position in the rules.)  `begin` is restarted on
        // each continuation to the pre_rule (but not gotos).
        //
        Init_Integer(P_START_VALUE, P_POS);
        Init_Integer(P_BEGIN_VALUE, P_POS);

        if (NOT_END(P_OUT)) {  // invariant provided by evaluator
            assert(IS_NULLED(P_OUT));  // !!! Except screwed up ATM
            SET_END(D_OUT);
        }

        // falls through to pre_rule plus new rule boilerplate
    }
    else {
        switch (D_STATE_BYTE) {
          case ST_PARSE_PRE_RULE:
            assert(IS_NULLED(P_OUT));  // temporary due to BLANK! continuation
            SET_END(P_OUT);
            break;  // sets `rule`
          case ST_PARSE_GROUP_WAS_EVALUATED:
            goto group_was_evaluated;  // sets the `rule` or `goto pre_rule;`
          default: assert(false);
        }
    }

    // Make sure index position is not past END
    //
    if (VAL_INDEX(P_INPUT_VALUE) > VAL_LEN_HEAD(P_INPUT_VALUE))
        VAL_INDEX(P_INPUT_VALUE) = VAL_LEN_HEAD(P_INPUT_VALUE);

    Init_Integer(P_BEGIN_VALUE, P_POS);
    assert(P_MINCOUNT == 1);
    assert(P_MAXCOUNT == 1);

    // The loop iterates across each REBVAL's worth of "rule" in the rule
    // block.  Some of these rules just set `flags` and `goto pre_rule`, so
    // the flags will apply to the next rule item.  If the flag is PF_SET
    // or PF_COPY, then the `set_or_copy_word` pointers will be assigned
    // at the same time as the active target of the COPY or SET.
    //
    // !!! This flagging process--established by R3-Alpha--is efficient
    // but somewhat haphazard.  It may work for `while ["a" | "b"]` to
    // "set the PF_WHILE" flag when it sees the `while` and then iterate
    // a rule it would have otherwise processed just once.  But there are
    // a lot of edge cases like `while |` where this method isn't set up
    // to notice a "grammar error".  It could use review.

  pre_rule:

    /* Print_Parse_Index(f); */
    UPDATE_EXPRESSION_START(f);

    //==////////////////////////////////////////////////////////////////==//
    //
    // PRE-RULE PROCESSING SECTION
    //
    //==////////////////////////////////////////////////////////////////==//

    // For non-iterated rules, including setup for iterated rules.
    // The input index is not advanced here, but may be changed by
    // a GET-WORD variable.

    rule = P_RULE;  // starting situation (may get evaluated and repointed)

    //=//// FIRST THINGS FIRST: CHECK FOR END /////////////////////////////=//

    if (IS_END(rule))
        goto return_position;  // used to `do_signals` (now trampoline does)

    //=//// HANDLE BAR! (BEFORE GROUP!) ///////////////////////////////////=//
    //
    // BAR!s cannot be abstracted.  If they could be, then you'd have to
    // run all GET-GROUP! `:(...)` to find them in alternates lists.
    //
    // Note: First test, so `[| ...anything...]` is a "no-op" match

    if (IS_BAR(rule))  // reached BAR! without a match failure, good!
        goto return_position;  // indicate match @ current pos

    //=//// (GROUP!) AND :(GET-GROUP!) PROCESSING /////////////////////////=//
    //
    // Code below may jump here to re-process groups, consider:
    //
    //    rule: lit (print "Hi")
    //    parse "a" [:('rule) "a"]
    //
    // First it processes the group to get RULE, then it looks that
    // up and gets another group.  In theory this could continue
    // indefinitely, but for now a GET-GROUP! can't return another.

    if (IS_GROUP(rule) or IS_GET_GROUP(rule)) {
      process_group: blockscope {
        DECLARE_FEED_AT_CORE (subfeed, rule, P_RULE_SPECIFIER);
        DECLARE_FRAME (subframe, subfeed,
            EVAL_MASK_DEFAULT | EVAL_FLAG_TO_END | EVAL_FLAG_ALLOCATED_FEED);
        INIT_F_EXECUTOR(f, &Parse_Group_Executor);
        Init_Logic(FRM_SPARE(f), IS_GET_GROUP(rule));
        INIT_F_EXECUTOR(subframe, &New_Expression_Executor);
        assert(IS_END(P_OUT));
        Push_Frame(P_OUT, subframe);
        D_STATE_BYTE = ST_PARSE_GROUP_WAS_EVALUATED;
        return R_CONTINUATION;
      }

      group_was_evaluated: blockscope {
        if (IS_END(P_OUT)) {  // was a (...), or null-bearing :(...)
            FETCH_NEXT_RULE(f);  // ignore result and go on to next rule
            goto pre_rule;
        }

        // was a GET-GROUP!, e.g. :(...), fall through so its result will
        // act as a rule in its own right.
        //
        assert(not IS_NULLED(P_OUT));
        Move_Value(save, P_OUT);
        rule = save;
        SET_END(P_OUT);
      }
    }

    assert(not IS_POINTER_TRASH_DEBUG(rule));  // should be assigned by here

    //=//// ANY-WORD!/ANY-PATH! PROCESSING ////////////////////////////////=//

    // Some iterated rules have a parameter.  `3 into [some "a"]` will
    // actually run the INTO `rule` 3 times with the `subrule` of
    // `[some "a"]`.  Because it is iterated it is only captured the first
    // time through, nullptr indicates it's not been captured yet.
    //
    const RELVAL *subrule = nullptr;

    if (ANY_PLAIN_GET_SET_WORD(rule)) { // word!, set-word!, or get-word!

        REBSYM cmd = VAL_CMD(rule);
        if (cmd != SYM_0) {
            if (not IS_WORD(rule)) // Command but not WORD! (COPY:, :THRU)
                fail (Error_Parse_Command(f));

            if (cmd > SYM_BREAK)  // R3-Alpha claimed "optimization"
                goto skip_pre_rule;  // but jump tables are fast, review

            switch (cmd) {
              case SYM_WHILE:
                assert(P_MINCOUNT == 1 and P_MAXCOUNT == 1);  // true on entry
                Init_Integer(P_MINCOUNT_VALUE, 0);
                Init_Integer(P_MAXCOUNT_VALUE, INT32_MAX);
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              case SYM_ANY:
                assert(P_MINCOUNT == 1 and P_MAXCOUNT == 1);  // true on entry
                Init_Integer(P_MINCOUNT_VALUE, 0);
                goto sym_some;

              case SYM_SOME:
                assert(P_MINCOUNT == 1 and P_MAXCOUNT == 1);  // true on entry
              sym_some:;
                flags |= PF_ANY_OR_SOME;
                Init_Integer(P_MAXCOUNT_VALUE, INT32_MAX);
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              case SYM_OPT:
                Init_Integer(P_MINCOUNT_VALUE, 0);
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              case SYM_COPY:
                flags |= PF_COPY;
                goto set_or_copy_pre_rule;

              case SYM_SET:
                flags |= PF_SET;
                goto set_or_copy_pre_rule;

              set_or_copy_pre_rule:;

                FETCH_NEXT_RULE(f);

                if (not (IS_WORD(P_RULE) or IS_SET_WORD(P_RULE)))
                    fail (Error_Parse_Variable(f));

                if (VAL_CMD(P_RULE))  // set set [...]
                    fail (Error_Parse_Command(f));

                FETCH_NEXT_RULE_KEEP_LAST(&set_or_copy_word, f);
                goto pre_rule;

              case SYM_COLLECT: {
                FETCH_NEXT_RULE(f);
                if (not (IS_WORD(P_RULE) or IS_SET_WORD(P_RULE)))
                    fail (Error_Parse_Variable(f));

                FETCH_NEXT_RULE_KEEP_LAST(&set_or_copy_word, f);

                REBARR *collection = Make_Array_Core(
                    10,  // !!! how big?
                    NODE_FLAG_MANAGED
                );
                PUSH_GC_GUARD(collection);

                DECLARE_FRAME (subframe, f->feed, EVAL_MASK_DEFAULT);

                bool interrupted;
                assert(IS_END(P_OUT));  // invariant until finished
                bool threw = Subparse_Throws(
                    &interrupted,
                    P_OUT,
                    P_INPUT_VALUE, // affected by P_POS assignment above
                    SPECIFIED,
                    subframe,
                    collection,
                    P_FIND_FLAGS | PF_ONE_RULE
                );

                DROP_GC_GUARD(collection);
                UNUSED(interrupted);  // !!! ignore ACCEPT/REJECT (?)

                if (threw)
                    return R_THROWN;

                if (IS_NULLED(P_OUT)) {  // match of rule failed
                    SET_END(P_OUT);  // restore invariant
                    goto next_alternate;  // backtrack collect, seek |
                }
                P_POS = VAL_INT32(P_OUT);
                SET_END(P_OUT);  // restore invariant

                Init_Block(
                    Sink_Word_May_Fail(
                        set_or_copy_word,
                        P_RULE_SPECIFIER
                    ),
                    collection
                );
                goto pre_rule; }

              case SYM_KEEP: {
                if (not P_COLLECTION)
                    fail ("Used PARSE KEEP with no COLLECT in effect");

                FETCH_NEXT_RULE(f);  // e.g. skip the KEEP word!

                // !!! We follow the R3-Alpha principle of not using
                // PATH! dispatch here, so it's `keep only` instead of
                // `keep/only`.  But is that any good?  Review.
                //
                bool only;
                if (IS_WORD(P_RULE) and VAL_WORD_SYM(P_RULE) == SYM_ONLY) {
                    only = true;
                    FETCH_NEXT_RULE(f);
                }
                else
                    only = false;

                REBLEN pos_before = P_POS;

                rule = Get_Parse_Value(save, P_RULE, P_RULE_SPECIFIER);

                if (IS_GET_BLOCK(rule)) {
                    //
                    // !!! Experimental use of GET-BLOCK! to mean ordinary
                    // evaluation of material that is not matched as
                    // a PARSE rule.
                    //
                    assert(IS_END(P_OUT));  // should be true until finish
                    if (Do_Any_Array_At_Throws(
                        P_OUT,
                        rule,
                        P_RULE_SPECIFIER
                    )){
                        return R_THROWN;
                    }

                    if (IS_END(P_OUT) or IS_NULLED(P_OUT)) {
                        // Nothing to add
                    }
                    else if (only) {
                        Move_Value(
                            Alloc_Tail_Array(P_COLLECTION),
                            P_OUT
                        );
                    }
                    else
                        rebElide(
                            "append", P_COLLECTION_VALUE, rebQ1(P_OUT),
                        rebEND);

                    SET_END(P_OUT);  // since we didn't throw, put it back

                    // Don't touch P_POS, we didn't consume anything from
                    // the input series but just fabricated DO material.

                    FETCH_NEXT_RULE(f);
                }
                else {  // Ordinary rule (may be block, may not be)

                    DECLARE_FRAME (subframe, f->feed, EVAL_MASK_DEFAULT);

                    bool interrupted;
                    assert(IS_END(P_OUT));  // invariant until finished
                    bool threw = Subparse_Throws(
                        &interrupted,
                        P_OUT,
                        P_INPUT_VALUE,
                        SPECIFIED,
                        subframe,
                        P_COLLECTION,
                        P_FIND_FLAGS | PF_ONE_RULE
                    );

                    UNUSED(interrupted);  // !!! ignore ACCEPT/REJECT (?)

                    if (threw)
                        return R_THROWN;

                    if (IS_NULLED(P_OUT)) {  // match of rule failed
                        SET_END(P_OUT);  // restore invariant
                        goto next_alternate;  // backtrack collect, seek |
                    }
                    REBLEN pos_after = VAL_INT32(P_OUT);
                    SET_END(P_OUT);  // restore invariant

                    assert(pos_after >= pos_before);  // 0 or more matches

                    REBARR *target;
                    if (pos_after == pos_before and not only) {
                        target = nullptr;
                    }
                    else if (ANY_STRING(P_INPUT_VALUE)) {
                        target = nullptr;
                        Init_Any_String(
                            Alloc_Tail_Array(P_COLLECTION),
                            P_TYPE,
                            Copy_String_At_Limit(
                                P_INPUT_VALUE,
                                pos_after - pos_before
                            )
                        );
                    }
                    else if (not IS_SER_ARRAY(P_INPUT)) {  // BINARY! (?)
                        target = nullptr;  // not an array, one item
                        Init_Any_Series(
                            Alloc_Tail_Array(P_COLLECTION),
                            P_TYPE,
                            Copy_Sequence_At_Len(
                                P_INPUT,
                                pos_before,
                                pos_after - pos_before
                            )
                        );
                    }
                    else if (only) {  // taken to mean "add as one block"
                        target = Make_Array_Core(
                            pos_after - pos_before,
                            NODE_FLAG_MANAGED
                        );
                        Init_Block(Alloc_Tail_Array(P_COLLECTION), target);
                    }
                    else
                        target = P_COLLECTION;

                    if (target) {
                        REBLEN n;
                        for (n = pos_before; n < pos_after; ++n)
                            Derelativize(
                                Alloc_Tail_Array(target),
                                ARR_AT(ARR(P_INPUT), n),
                                P_INPUT_SPECIFIER
                            );
                    }

                    P_POS = pos_after;  // continue from end of kept data
                }
                goto pre_rule; }

              case SYM_NOT:
                flags |= PF_NOT;
                flags ^= PF_NOT2;
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              case SYM_AND:
              case SYM_AHEAD:
                flags |= PF_AHEAD;
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              case SYM_THEN:
                flags |= PF_THEN;
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              case SYM_REMOVE:
                flags |= PF_REMOVE;
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              case SYM_INSERT:
                flags |= PF_INSERT;
                FETCH_NEXT_RULE(f);
                goto post_match_processing;

              case SYM_CHANGE:
                flags |= PF_CHANGE;
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              // IF is deprecated in favor of `:(<logic!>)`.  But it is
              // currently used for bootstrap.  Remove once the bootstrap
              // executable is updated to have GET-GROUP!s.  Substitution:
              //
              //    (go-on?: either condition [[accept]][[reject]])
              //    go-on?
              //
              // !!! Note: PARSE/REDBOL may be a modality it needs to
              // support, and Red added IF.  It might be necessary to keep
              // it (though Rebol2 did not have IF in PARSE...)
              //
              case SYM_IF: {
                FETCH_NEXT_RULE(f);
                if (IS_END(P_RULE))
                    fail (Error_Parse_End());

                if (not IS_GROUP(P_RULE))
                    fail (Error_Parse_Rule());

                DECLARE_LOCAL (condition);
                if (Do_Any_Array_At_Throws(  // note: might GC
                    condition,
                    P_RULE,
                    P_RULE_SPECIFIER
                )) {
                    Move_Value(P_OUT, condition);
                    return R_THROWN;
                }

                FETCH_NEXT_RULE(f);

                if (IS_TRUTHY(condition))
                    goto pre_rule;

                P_POS = NOT_FOUND;
                goto post_match_processing; }

              case SYM_ACCEPT:
              case SYM_BREAK: {
                //
                // This has to be throw-style, because it's not enough
                // to just say the current rule succeeded...it climbs
                // up and affects an enclosing parse loop.
                //
                DECLARE_LOCAL (thrown_arg);
                Init_Integer(thrown_arg, P_POS);
                thrown_arg->extra.trash = thrown_arg;  // see notes

                return Init_Thrown_With_Label(
                    P_OUT,
                    thrown_arg,
                    NAT_VALUE(parse_accept)
                ); }

              case SYM_REJECT: {
                //
                // Similarly, this is a break/continue style "throw"
                //
                return Init_Thrown_With_Label(
                    P_OUT,
                    NULLED_CELL,
                    NAT_VALUE(parse_reject)
                ); }

              case SYM_FAIL:  // deprecated... use LOGIC! false instead
                P_POS = NOT_FOUND;
                FETCH_NEXT_RULE(f);
                goto post_match_processing;

              case SYM_LIMIT:
                fail (Error_Not_Done_Raw());

              case SYM__Q_Q:
                Print_Parse_Index(f);
                FETCH_NEXT_RULE(f);
                goto pre_rule;

              case SYM_RETURN:
                fail ("RETURN removed from PARSE, use (THROW ...)");

              case SYM_MARK: {
                FETCH_NEXT_RULE(f);  // skip the MARK word
                // !!! what about `mark @(first [x])` ?
                Handle_Mark_Rule(f, P_RULE, P_RULE_SPECIFIER);
                FETCH_NEXT_RULE(f);  // e.g. skip the `x` in `mark x`
                goto pre_rule; }

              case SYM_SEEK: {
                FETCH_NEXT_RULE(f);  // skip the SEEK word
                // !!! what about `seek @(first x)` ?
                HANDLE_SEEK_RULE_UPDATE_BEGIN(f, P_RULE, P_RULE_SPECIFIER);
                FETCH_NEXT_RULE(f);  // e.g. skip the `x` in `seek x`
                goto pre_rule; }

              default:  // the list above should be exhaustive
                assert(false);
            }

          skip_pre_rule:;

            // Any other WORD! with VAL_CMD() is a parse keyword, but is
            // a "match command", so proceed...
        }
        else {
            // It's not a PARSE command, get or set it

            // word: - set a variable to the series at current index
            if (IS_SET_WORD(rule)) {
                //
                // !!! Review meaning of marking the parse in a slot that
                // is a target of a rule, e.g. `thru pos: xxx`
                //
                // https://github.com/rebol/rebol-issues/issues/2269
                //
                // if (flags != 0) fail (Error_Parse_Rule());

                Handle_Mark_Rule(f, rule, P_RULE_SPECIFIER);
                FETCH_NEXT_RULE(f);
                goto pre_rule;
            }

            // :word - change the index for the series to a new position
            if (IS_GET_WORD(rule)) {
                HANDLE_SEEK_RULE_UPDATE_BEGIN(f, rule, P_RULE_SPECIFIER);
                FETCH_NEXT_RULE(f);
                goto pre_rule;
            }

            assert(IS_WORD(rule));  // word - some other variable

            if (rule != save) {
                Get_Word_May_Fail(save, rule, P_RULE_SPECIFIER);
                rule = save;
            }
            if (IS_NULLED(rule))
                fail (Error_No_Value_Core(P_RULE, P_RULE_SPECIFIER));
        }
    }
    else if (ANY_PATH(rule)) {
        if (IS_PATH(rule)) {
            if (Get_Path_Throws_Core(save, rule, P_RULE_SPECIFIER)) {
                Move_Value(P_OUT, save);
                return R_THROWN;
            }
            rule = save;
        }
        else if (IS_SET_PATH(rule)) {
            Handle_Mark_Rule(f, rule, P_RULE_SPECIFIER);
            FETCH_NEXT_RULE(f);
            goto pre_rule;
        }
        else if (IS_GET_PATH(rule)) {
            HANDLE_SEEK_RULE_UPDATE_BEGIN(f, rule, P_RULE_SPECIFIER);
            FETCH_NEXT_RULE(f);
            goto pre_rule;
        }
    }
    else if (IS_SET_GROUP(rule)) {
        //
        // Don't run the group yet, just hold onto it...will run and set
        // the contents (or pass found value to function as parameter)
        // only if a match happens.
        //
        FETCH_NEXT_RULE_KEEP_LAST(&set_or_copy_word, f);
        flags |= PF_SET;
        goto pre_rule;
    }

    assert(not IS_NULLED(rule));

    if (IS_BAR(rule))
        fail ("BAR! must be source level (else PARSE can't skip it)");

    switch (VAL_TYPE(rule)) {
      case REB_GROUP:
        goto process_group; // GROUP! can make WORD! that fetches GROUP!

      case REB_BLANK: // no-op
        FETCH_NEXT_RULE(f);
        goto pre_rule;

      case REB_LOGIC: // true is a no-op, false causes match failure
        if (VAL_LOGIC(rule)) {
            FETCH_NEXT_RULE(f);
            goto pre_rule;
        }
        FETCH_NEXT_RULE(f);
        P_POS = NOT_FOUND;
        goto post_match_processing;

      case REB_INTEGER: // Specify count or range count, 1 or 2 integers
        Init_Integer(P_MINCOUNT_VALUE, Int32s(rule, 0));
        Init_Integer(P_MAXCOUNT_VALUE, Int32s(rule, 0));

        FETCH_NEXT_RULE(f);
        if (IS_END(P_RULE))
            fail (Error_Parse_End());

        rule = Get_Parse_Value(save, P_RULE, P_RULE_SPECIFIER);

        if (IS_INTEGER(rule)) {
            Init_Integer(P_MAXCOUNT_VALUE, Int32s(rule, 0));

            FETCH_NEXT_RULE(f);
            if (IS_END(P_RULE))
                fail (Error_Parse_End());

            rule = Get_Parse_Value(save, P_RULE, P_RULE_SPECIFIER);
        }

        if (IS_INTEGER(rule)) {
            //
            // `parse [1 1] [1 3 1]` must be `parse [1 1] [1 3 lit 1]`
            //
            fail ("For matching, INTEGER!s must be literal with QUOTE");
        }
        break;

      default:;
        // Fall through to next section
    }

    //==////////////////////////////////////////////////////////////////==//
    //
    // ITERATED RULE PROCESSING SECTION
    //
    //==////////////////////////////////////////////////////////////////==//

    // Repeats the same rule N times or until the rule fails.
    // The index is advanced and stored in a temp variable i until
    // the entire rule has been satisfied.

    FETCH_NEXT_RULE(f);

    Init_Integer(P_BEGIN_VALUE, P_POS);  // input at match section beginning

    assert(IS_UNREADABLE_DEBUG(P_COUNT_VALUE));
    Init_Integer(P_COUNT_VALUE, 0);
    while (P_COUNT < P_MAXCOUNT) {
        assert(
            not IS_BAR(rule)
            and not IS_BLANK(rule)
            and not IS_LOGIC(rule)
            and not IS_INTEGER(rule)
            and not IS_GROUP(rule)
        ); // these should all have been handled before iterated section

        REBIXO i; // temp index point

        if (IS_WORD(rule)) {
            REBSYM cmd = VAL_CMD(rule);

            switch (cmd) {
              case SYM_SKIP:
                i = (P_POS < SER_LEN(P_INPUT))
                    ? P_POS + 1
                    : END_FLAG;
                break;

              case SYM_END:
                i = (P_POS < SER_LEN(P_INPUT))
                    ? END_FLAG
                    : SER_LEN(P_INPUT);
                break;

              case SYM_TO:
              case SYM_THRU: {
                if (IS_END(P_RULE))
                    fail (Error_Parse_End());

                if (!subrule) { // capture only on iteration #1
                    subrule = Get_Parse_Value(
                        save, P_RULE, P_RULE_SPECIFIER
                    );
                    FETCH_NEXT_RULE(f);
                }

                bool is_thru = (cmd == SYM_THRU);

                if (IS_BLOCK(subrule))
                    i = To_Thru_Block_Rule(f, subrule, is_thru);
                else
                    i = To_Thru_Non_Block_Rule(f, subrule, is_thru);
                break; }

              case SYM_QUOTE: // temporarily behaving like LIT for bootstrap
              case SYM_LITERAL:
              case SYM_LIT: {
                if (not IS_SER_ARRAY(P_INPUT))
                    fail (Error_Parse_Rule()); // see #2253

                if (IS_END(P_RULE))
                    fail (Error_Parse_End());

                if (not subrule) // capture only on iteration #1
                    FETCH_NEXT_RULE_KEEP_LAST(&subrule, f);

                RELVAL *cmp = ARR_AT(ARR(P_INPUT), P_POS);

                if (IS_END(cmp))
                    i = END_FLAG;
                else if (0 == Cmp_Value(cmp, subrule, P_HAS_CASE))
                    i = P_POS + 1;
                else
                    i = END_FLAG;
                break;
            }

            // !!! This is a hack to try and get some semblance of
            // compatibility in a world where 'X and 'X/Y/Z don't have
            // unique datatype "kinds", but are both QUOTED! (versions of
            // WORD! and PATH! respectively).  By making a LIT-WORD! and
            // LIT-PATH! parse rule keyword, situations can be worked
            // around, but MATCH should be used in the general case.
            //
              case SYM_LIT_WORD_X: // lit-word!
              case SYM_LIT_PATH_X: // lit-path!
              case SYM_REFINEMENT_X: {  // refinement!
                REB_R r = Parse_One_Rule(f, P_POS, rule);
                assert(r != R_IMMEDIATE);
                if (r == R_THROWN)
                    return R_THROWN;

                if (r == R_UNHANDLED)
                    i = END_FLAG;
                else {
                    assert(r == P_OUT);
                    i = VAL_INT32(P_OUT);
                }
                SET_END(P_OUT);  // preserve invariant
                break; }

            // Because there are no LIT-XXX! datatypes, a special rule
            // must be used if you want to match quoted types.  MATCH is
            // brought in to do this duty, bringing along with it the
            // features of the native.
            //
              case SYM_MATCH: {
                if (not IS_SER_ARRAY(P_INPUT))
                    fail (Error_Parse_Rule()); // see #2253

                if (IS_END(P_RULE))
                    fail (Error_Parse_End());

                if (not subrule) // capture only on iteration #1
                    FETCH_NEXT_RULE_KEEP_LAST(&subrule, f);

                RELVAL *cmp = ARR_AT(ARR(P_INPUT), P_POS);

                if (IS_END(cmp))
                    i = END_FLAG;
                else {
                    DECLARE_LOCAL (temp);
                    if (Match_Core_Throws(
                        temp,
                        subrule, P_RULE_SPECIFIER,
                        cmp, P_INPUT_SPECIFIER
                    )){
                        Move_Value(P_OUT, temp);
                        return R_THROWN;
                    }

                    if (VAL_LOGIC(temp))
                        i = P_POS + 1;
                    else
                        i = END_FLAG;
                }
                break; }

              case SYM_INTO: {
                if (IS_END(P_RULE))
                    fail (Error_Parse_End());

                if (!subrule) {
                    subrule = Get_Parse_Value(
                        save, P_RULE, P_RULE_SPECIFIER
                    );
                    FETCH_NEXT_RULE(f);
                }

                if (not IS_BLOCK(subrule))
                    fail (Error_Parse_Rule());

                // parse ["aa"] [into ["a" "a"]] ; is legal
                // parse "aa" [into ["a" "a"]] ; is not...already "into"
                //
                if (not IS_SER_ARRAY(P_INPUT))
                    fail (Error_Parse_Rule());

                RELVAL *into = ARR_AT(ARR(P_INPUT), P_POS);
                if (IS_END(into)) {
                    i = END_FLAG;  // `parse [] [into [...]]`, rejects
                    break;
                }
                else if (ANY_PATH_KIND(CELL_KIND(VAL_UNESCAPED(into)))) {
                    //
                    // Can't PARSE an ANY-PATH! because it has no position
                    // But would be inconvenient if INTO did not support.
                    // Transform implicitly into a BLOCK! form.
                    //
                    // !!! Review faster way of sharing the AS transform.
                    //
                    Derelativize(P_CELL, into, P_INPUT_SPECIFIER);
                    into = rebValueQ("as block!", P_CELL, rebEND);
                }
                else if (
                    not ANY_SERIES_KIND(CELL_KIND(VAL_UNESCAPED(into)))
                ){
                    i = END_FLAG;  // `parse [1] [into [...]`, rejects
                    break;
                }

                DECLARE_ARRAY_FEED (subrules_feed,
                    VAL_ARRAY(subrule),
                    VAL_INDEX(subrule),
                    P_RULE_SPECIFIER
                );

                DECLARE_FRAME (
                    subframe,
                    subrules_feed,
                    EVAL_MASK_DEFAULT | EVAL_FLAG_ALLOCATED_FEED
                );

                bool interrupted;
                if (Subparse_Throws(
                    &interrupted,
                    SET_END(P_OUT),
                    into,
                    P_INPUT_SPECIFIER,  // harmless if specified API value
                    subframe,
                    P_COLLECTION,
                    P_FIND_FLAGS
                )){
                    return R_THROWN;
                }

                // !!! ignore interrupted? (e.g. ACCEPT or REJECT ran)

                if (IS_NULLED(P_OUT)) {
                    i = END_FLAG;
                }
                else {
                    if (VAL_UINT32(P_OUT) != VAL_LEN_HEAD(into))
                        i = END_FLAG;
                    else
                        i = P_POS + 1;
                }

                if (Is_Api_Value(into))
                    rebRelease(KNOWN(into));  // !!! rethink to use P_CELL

                SET_END(P_OUT);  // restore invariant
                break;
            }

            case SYM_DO: {
                if (subrule) {
                    //
                    // Not currently set up for iterating DO rules
                    // since the Do_Eval_Rule routine expects to be
                    // able to arbitrarily update P_NEXT_RULE
                    //
                    fail ("DO rules currently cannot be iterated");
                }

                subrule = VOID_VALUE; // cause an error if iterating

                i = Do_Eval_Rule(f); // changes P_RULE (should)

                if (i == THROWN_FLAG)
                    return R_THROWN;

                break;
            }

            default:
                fail (Error_Parse_Rule());
            }
        }
        else if (IS_BLOCK(rule)) {  // word fetched block, or inline block

            DECLARE_ARRAY_FEED (subrules_feed,
                VAL_ARRAY(rule),
                VAL_INDEX(rule),
                P_RULE_SPECIFIER
            );

            DECLARE_FRAME (
                subframe,
                subrules_feed,
                EVAL_MASK_DEFAULT | EVAL_FLAG_ALLOCATED_FEED
            );

            bool interrupted;
            if (Subparse_Throws(
                &interrupted,
                SET_END(P_CELL),
                P_INPUT_VALUE,
                SPECIFIED,
                subframe,
                P_COLLECTION,
                P_FIND_FLAGS & ~(PF_ONE_RULE)
            )) {
                Move_Value(P_OUT, P_CELL);
                return R_THROWN;
            }

            // Non-breaking out of loop instances of match or not.

            if (IS_NULLED(P_CELL))
                i = END_FLAG;
            else {
                assert(IS_INTEGER(P_CELL));
                i = VAL_INT32(P_CELL);
            }

            if (interrupted) { // ACCEPT or REJECT ran
                assert(i != THROWN_FLAG);
                if (i == END_FLAG)
                    P_POS = NOT_FOUND;
                else
                    P_POS = cast(REBLEN, i);
                break;
            }
        }
        else {
            // Parse according to datatype

            REB_R r = Parse_One_Rule(f, P_POS, rule);
            if (r == R_THROWN)
                return R_THROWN;

            if (r == R_UNHANDLED)
                i = END_FLAG;
            else {
                assert(r == P_OUT or r == R_IMMEDIATE);
                if (r == R_IMMEDIATE) {
                    assert(DSP == f->dsp_orig + 1);
                    if (not (flags & PF_SET))  // only SET handles
                        DS_DROP();
                }
                i = VAL_INT32(P_OUT);
            }
            SET_END(P_OUT);  // preserve invariant
        }

        assert(i != THROWN_FLAG);

        // i: indicates new index or failure of the *match*, but
        // that does not mean failure of the *rule*, because optional
        // matches can still succeed when the last match failed.
        //
        if (i == END_FLAG) {  // this match failed
            if (P_COUNT < P_MINCOUNT) {
                P_POS = NOT_FOUND;  // number of matches was not enough
            }
            else {
                // just keep index as is.
            }
            break;
        }

        Init_Integer(P_COUNT_VALUE, P_COUNT + 1);  // may overflow to negative
        if (P_COUNT < 0)
            Init_Integer(P_COUNT_VALUE, INT32_MAX);  // the forever case

        P_POS = cast(REBLEN, i);

        if (i == SER_LEN(P_INPUT) and (flags & PF_ANY_OR_SOME)) {
            //
            // ANY and SOME auto terminate on e.g. `some [... | end]`.
            // But WHILE is conceptually a synonym for a self-recursive
            // rule and does not consider it a termination.  See:
            //
            // https://github.com/rebol/rebol-issues/issues/1268
            //
            break;
        }
    }

    if (P_POS > SER_LEN(P_INPUT))
        P_POS = NOT_FOUND;

//==////////////////////////////////////////////////////////////////==//
//
// "POST-MATCH PROCESSING"
//
//==////////////////////////////////////////////////////////////////==//

    // The comment here says "post match processing", but it may be a
    // failure signal.  Or it may have been a success and there could be
    // a NOT to apply.  Note that failure here doesn't mean returning
    // from SUBPARSE, as there still may be alternate rules to apply
    // with bar e.g. `[a | b | c]`.

  post_match_processing:;

    if (flags) {
        if (flags & PF_NOT) {
            if ((flags & PF_NOT2) and P_POS != NOT_FOUND)
                P_POS = NOT_FOUND;
            else
                P_POS = P_BEGIN;
        }

        if (P_POS == NOT_FOUND) {
            if (flags & PF_THEN) {
                FETCH_TO_BAR_OR_END(f);
                if (NOT_END(P_RULE))
                    FETCH_NEXT_RULE(f);
            }
        }
        else {
            // Set count to how much input was advanced
            //
            if (P_BEGIN > P_POS)
                Init_Integer(P_COUNT_VALUE, 0);
            else
                Init_Integer(P_COUNT_VALUE, P_POS - P_BEGIN);

            if (flags & PF_COPY) {
                REBVAL *sink = Sink_Word_May_Fail(
                    set_or_copy_word,
                    P_RULE_SPECIFIER
                );
                if (ANY_ARRAY(P_INPUT_VALUE)) {
                    //
                    // Act like R3-Alpha in preserving GROUP! vs. BLOCK!
                    // distinction (which Rebol2 did not).  But don't keep
                    // SET-XXX! or GET-XXX! (like how quoting is not kept)
                    //
                    Init_Any_Array(
                        sink,
                        ANY_GROUP_KIND(P_TYPE) ? REB_GROUP : REB_BLOCK,
                        Copy_Array_At_Max_Shallow(
                            ARR(P_INPUT),
                            P_BEGIN,
                            P_INPUT_SPECIFIER,
                            P_COUNT
                        )
                    );
                }
                else if (IS_BINARY(P_INPUT_VALUE)) {
                    Init_Binary(  // R3-Alpha behavior (e.g. not AS TEXT!)
                        sink,
                        Copy_Sequence_At_Len(P_INPUT, P_BEGIN, P_COUNT)
                    );
                }
                else {
                    assert(ANY_STRING(P_INPUT_VALUE));

                    DECLARE_LOCAL (begin_val);
                    Init_Any_Series_At(begin_val, P_TYPE, P_INPUT, P_BEGIN);

                    // Rebol2 behavior of always "netural" TEXT!.  Avoids
                    // creation of things like URL!-typed fragments that
                    // have no scheme:// at their head, or getting <bc>
                    // out of <abcd> as if `<b` or `c>` had been found.
                    //
                    Init_Text(
                        sink,
                        Copy_String_At_Limit(begin_val, P_COUNT)
                    );
                }

                // !!! As we are losing the datatype here, it doesn't make
                // sense to carry forward the quoting on the input.  It
                // is collecting items in a neutral container.  It is less
                // obvious what marking a position should do.
            }
            else if ((flags & PF_SET) and (P_COUNT != 0)) { // 0-leave alone
                //
                // We waited to eval the SET-GROUP! until we knew we had
                // something we wanted to set.  Do so, and then go through
                // a normal setting procedure.
                //
                if (IS_SET_GROUP(set_or_copy_word)) {
                    if (Do_Any_Array_At_Throws(
                        P_CELL,
                        set_or_copy_word,
                        P_RULE_SPECIFIER
                    )){
                        Move_Value(P_OUT, P_CELL);
                        return R_THROWN;
                    }

                    // !!! What SET-GROUP! can do in PARSE is more
                    // ambitious than just an indirection for naming
                    // variables or paths...but for starters it does
                    // that just to show where more work could be done.

                    if (not (IS_WORD(P_CELL) or IS_SET_WORD(P_CELL)))
                        fail (Error_Parse_Variable_Raw(P_CELL));

                    set_or_copy_word = P_CELL;
                }

                if (IS_SER_ARRAY(P_INPUT)) {
                    Derelativize(
                        Sink_Word_May_Fail(
                            set_or_copy_word, P_RULE_SPECIFIER
                        ),
                        ARR_AT(ARR(P_INPUT), P_BEGIN),
                        P_INPUT_SPECIFIER
                    );
                }
                else {
                    REBVAL *var = Sink_Word_May_Fail(
                        set_or_copy_word, P_RULE_SPECIFIER
                    );

                    // A Git merge of UTF-8 everywhere put this here,
                    // with no corresponding use of "captured".  It's not
                    // clear what happened--leaving it here to investigate
                    // if a pertinent bug has a smoking gun here.

                    /*
                    DECLARE_LOCAL (begin_val);
                    Init_Any_Series_At(begin_val, P_TYPE, P_INPUT, begin);
                    Init_Any_Series(
                        captured,
                        P_TYPE,
                        Copy_String_At_Limit(begin_val, count)
                    );
                    */

                    if (DSP > f->dsp_orig) {
                        Move_Value(var, DS_TOP);
                        DS_DROP();
                        if (DSP != f->dsp_orig)
                            fail ("SET for datatype only allows 1 value");
                    }
                    else if (P_TYPE == REB_BINARY)
                        Init_Integer(var, *BIN_AT(P_INPUT, P_BEGIN));
                    else
                        Init_Char_Unchecked(
                            var,
                            GET_CHAR_AT(STR(P_INPUT), P_BEGIN)
                        );
                }
            }

            if (flags & PF_REMOVE) {
                FAIL_IF_READ_ONLY(P_INPUT_VALUE);
                if (P_COUNT != 0)
                    Remove_Series_Len(P_INPUT, P_BEGIN, P_COUNT);
                P_POS = P_BEGIN;
            }

            if (flags & (PF_INSERT | PF_CHANGE)) {
                FAIL_IF_READ_ONLY(P_INPUT_VALUE);
                Init_Integer(P_COUNT_VALUE, (flags & PF_INSERT) ? 0 : P_COUNT);
                bool only = false;

                if (IS_END(P_RULE))
                    fail (Error_Parse_End());

                if (IS_WORD(P_RULE)) { // check for ONLY flag
                    REBSYM cmd = VAL_CMD(P_RULE);
                    switch (cmd) {
                      case SYM_ONLY:
                        only = true;
                        FETCH_NEXT_RULE(f);
                        if (IS_END(P_RULE))
                            fail (Error_Parse_End());
                        break;

                      case SYM_0: // not a "parse command" word, keep going
                        break;

                      default: // other commands invalid after INSERT/CHANGE
                        fail (Error_Parse_Rule());
                    }
                }

                // new value...comment said "CHECK FOR QUOTE!!"
                rule = Get_Parse_Value(save, P_RULE, P_RULE_SPECIFIER);
                FETCH_NEXT_RULE(f);

                // If a GROUP!, then execute it first.  See #1279
                //
                DECLARE_LOCAL (evaluated);
                if (IS_GROUP(rule)) {
                    REBSPC *derived = Derive_Specifier(
                        P_RULE_SPECIFIER,
                        rule
                    );
                    if (Do_Any_Array_At_Throws(
                        evaluated,
                        rule,
                        derived
                    )){
                        Move_Value(P_OUT, evaluated);
                        return R_THROWN;
                    }

                    rule = evaluated;
                }

                if (IS_SER_ARRAY(P_INPUT)) {
                    DECLARE_LOCAL (specified);
                    Derelativize(specified, rule, P_RULE_SPECIFIER);

                    REBLEN mod_flags = (flags & PF_INSERT) ? 0 : AM_PART;
                    if (
                        not only and
                        Splices_Into_Type_Without_Only(P_TYPE, specified)
                    ){
                        mod_flags |= AM_SPLICE;
                    }
                    P_POS = Modify_Array(
                        (flags & PF_CHANGE)
                            ? Canon(SYM_CHANGE)
                            : Canon(SYM_INSERT),
                        ARR(P_INPUT),
                        P_BEGIN,
                        specified,
                        mod_flags,
                        P_COUNT,
                        1
                    );

                    if (IS_QUOTED(rule))
                        Unquotify(ARR_AT(ARR(P_INPUT), P_POS - 1), 1);
                }
                else {
                    DECLARE_LOCAL (specified);
                    Derelativize(specified, rule, P_RULE_SPECIFIER);

                    P_POS = P_BEGIN;

                    REBLEN mod_flags = (flags & PF_INSERT) ? 0 : AM_PART;

                    P_POS = Modify_String_Or_Binary(  // checks read-only
                        P_INPUT_VALUE,
                        (flags & PF_CHANGE)
                            ? Canon(SYM_CHANGE)
                            : Canon(SYM_INSERT),
                        specified,
                        mod_flags,
                        P_COUNT,
                        1
                    );
                }
            }

            if (flags & PF_AHEAD)
                P_POS = P_BEGIN;
        }

        flags = 0;
        set_or_copy_word = NULL;
    }

    if (P_POS == NOT_FOUND) {

      next_alternate:;

        // If this is just one step, e.g.:
        //
        //     collect x keep some "a" | keep some "b"
        //
        // COLLECT asked for one step, and the first keep asked for one
        // step.  So that second KEEP applies only to some outer collect.
        //
        if (P_FIND_FLAGS & PF_ONE_RULE)
            goto return_null;

        if (P_COLLECTION)
            TERM_ARRAY_LEN(P_COLLECTION, collection_tail);

        FETCH_TO_BAR_OR_END(f);
        if (IS_END(P_RULE))  // no alternate rule
            goto return_null;

        // Jump to the alternate rule and reset input
        //
        FETCH_NEXT_RULE(f);
        P_POS = P_START;
        Init_Integer(P_BEGIN_VALUE, P_START);
    }

    if (P_FIND_FLAGS & PF_ONE_RULE)  // don't loop
        goto return_position;

    // !!! `begin` is a local variable, reset on re-entry but the question is
    // if it needs to hold state across steps or evaluations.
    //
    /* begin = P_POS; */
    assert(flags == 0);  // local variable, wasn't reset here (should be 0)

    // !!! These apply to things like `parse "aa" [2 ["a" (print "hi")]]`.
    // Hence they may need to be carried across continuations (though perhaps
    // not if that's always a subparse).  If this rule needed to be re-entered
    // then they can't just be reset at the entry.  asserts that they are 1
    // at the outset are added.
    //
    Init_Integer(P_MINCOUNT_VALUE, 1);
    Init_Integer(P_MAXCOUNT_VALUE, 1);
    Init_Unreadable_Blank(P_COUNT_VALUE);

    assert(f->executor == &Parse_Executor);
    D_STATE_BYTE = ST_PARSE_PRE_RULE;
    CONTINUE (BLANK_VALUE);  // get called back with a NULL

  return_position:
    INIT_F_EXECUTOR(f, &Finished_Executor);
    return Init_Integer(D_OUT, P_POS); // !!! return switched input series??

  return_null:
    INIT_F_EXECUTOR(f, &Finished_Executor);
    return Init_Nulled(D_OUT);
}


//
//  parse: native [
//
//  "Parse series according to grammar rules, return last match position"
//
//      return: "null if rules failed, else terminal position of match"
//          [<opt> any-series! quoted!]
//      input "Input series to parse"
//          [<blank> any-series! quoted!]
//      rules "Rules to parse by"
//          [<blank> block!]
//      /case "Uses case-sensitive comparison"
//  ]
//
REBNATIVE(parse)
//
// !!! We currently don't use <dequote> and <requote> so that the parse COPY
// can persist the type of the input.  This complicates things, but also it
// may not have been a great change in R3-Alpha in the first place:
//
// https://forum.rebol.info/t/1084
{
    INCLUDE_PARAMS_OF_PARSE;

    if (IS_BLANK(D_SPARE))
        goto subparse_finished;
    assert(IS_END(D_SPARE));

  blockscope {
    if (not ANY_SERIES_KIND(CELL_KIND(VAL_UNESCAPED(ARG(input)))))
        fail ("PARSE input must be an ANY-SERIES! (use AS BLOCK! for PATH!)");

    DECLARE_ARRAY_FEED (rules_feed,
        VAL_ARRAY(ARG(rules)),
        VAL_INDEX(ARG(rules)),
        VAL_SPECIFIER(ARG(rules))
    );

    DECLARE_FRAME (
        f,
        rules_feed,
        EVAL_MASK_DEFAULT | EVAL_FLAG_ALLOCATED_FEED
            | EVAL_FLAG_TRAMPOLINE_KEEPALIVE
    );

    Pack_Subparse(
        SET_END(D_OUT),
        ARG(input), SPECIFIED,
        f,
        nullptr,  // start out with no COLLECT in effect, so no P_COLLECTION
        REF(case) ? AM_FIND_CASE : 0
        //
        // We always want "case-sensitivity" on binary bytes, vs. treating
        // as case-insensitive bytes for ASCII characters.
    );

    Init_Blank(D_SPARE);  // signal next call that we won't be top of stack
//    assert(f->executor == &Action_Executor);  // was set by Begin_Action()

    D_STATE_BYTE = ST_PARSE_PRE_RULE;
    return R_CONTINUATION;
  }

  subparse_finished:
  blockscope {
    REBFRM *f = FS_TOP;
    assert(f->prior == frame_);  // !!! Review this guarantee generically

    const REBVAL *r = D_OUT;
    bool interrupted;
    bool threw = Unpack_Subparse_Throws(&interrupted, D_OUT, f, r);

    // Any PARSE-specific THROWs (where a PARSE directive jumped the
    // stack) should be handled here.  However, RETURN was eliminated,
    // in favor of a more clear return value protocol for PARSE

    if (threw)
        return R_THROWN;

    if (IS_NULLED(D_OUT))
        return nullptr;

    REBLEN progress = VAL_UINT32(D_OUT);
    assert(progress <= VAL_LEN_HEAD(ARG(input)));
    Move_Value(D_OUT, ARG(input));
    VAL_INDEX(D_OUT) = progress;
    return D_OUT;
  }
}


//
//  parse-accept: native [
//
//  "Accept the current parse rule (Internal Implementation Detail ATM)."
//
//  ]
//
REBNATIVE(parse_accept)
//
// !!! This was not created for user usage, but rather as a label for the
// internal throw used to indicate "accept".
{
    UNUSED(frame_);
    fail ("PARSE-ACCEPT is for internal PARSE use only");
}


//
//  parse-reject: native [
//
//  "Reject the current parse rule (Internal Implementation Detail ATM)."
//
//  ]
//
REBNATIVE(parse_reject)
//
// !!! This was not created for user usage, but rather as a label for the
// internal throw used to indicate "reject".
{
    UNUSED(frame_);
    fail ("PARSE-REJECT is for internal PARSE use only");
}
