; Is PARSE working at all?

(did parse "abc" ["abc" end])

; Edge case of matching END with TO or THRU
;
("" = parse "" [to ["a" | end]])
("" = parse "" [thru ["a" | end]])
([] = parse [] [to ["a" | end]])
([] = parse [] [thru ["a" | end]])


[#206 (
    any-char: complement charset ""
    repeat n 512 [
        if n = 1 [continue]
        if "" != parse (append copy "" to char! n - 1) [set c any-char end] [
            fail "Parse didn't work"
        ]
        if c != to char! n - 1 [fail "Char didn't match"]
    ]
    true
)]


; Don't leak internal detail that BINARY! or ANY-STRING! are 0-terminated
[
    (NUL = to char! 0)

    (null = parse "" [to NUL])
    (null = parse "" [thru NUL])
    (null = parse "" [to [NUL]])
    (null = parse "" [thru [NUL]])

    (null = parse #{} [to NUL])
    (null = parse #{} [thru NUL])
    (null = parse #{} [to [NUL]])
    (null = parse #{} [thru [NUL]])
]


; Are null rules raising the right error?

(
    foo: null
    e: trap [parse "a" [foo]]
    e/id = 'no-value
)

; Blank and empty block case handling

(did parse [] [end])
(did parse [] [[[]] end])
(did parse [] [_ _ _ end])
(not parse [x] [end])
(not parse [x] [_ _ _ end])
(not parse [x] [[[]] end])
(did parse [] [[[_ _ _] end]])
(did parse [x] ['x _ end])
(did parse [x] [_ 'x end])
(did parse [x] [[] 'x [] end])

; SET-WORD! (store current input position)

(
    res: did parse ser: [x y] [pos: skip skip end]
    all [res | pos = ser]
)
(
    res: did parse ser: [x y] [skip pos: skip end]
    all [res | pos = next ser]
)
(
    res: did parse ser: [x y] [skip skip pos: end]
    all [res | pos = tail of ser]
)
[#2130 (
    res: did parse ser: [x] [set val pos: word! end]
    all [res | val = 'x | pos = ser]
)]
[#2130 (
    res: did parse ser: [x] [set val: pos: word! end]
    all [res | val = 'x | pos = ser]
)]
[#2130 (
    res: did parse ser: "foo" [copy val pos: skip end]
    all [not res | val = "f" | pos = ser]
)]
[#2130 (
    res: did parse ser: "foo" [copy val: pos: skip end]
    all [not res | val = "f" | pos = ser]
)]

; SEEK INTEGER! (replaces TO/THRU integer!

(did parse "abcd" [seek 3 "cd" end])
(did parse "abcd" [seek 5 end])
(did parse "abcd" [seek 128 end])

[#1965
    (did parse "abcd" [seek 3 skip "d" end])
    (did parse "abcd" [seek 4 skip end])
    (did parse "abcd" [seek 128 end])
    (did parse "abcd" ["ab" seek 1 "abcd" end])
    (did parse "abcd" ["ab" seek 1 skip "bcd" end])
]

; parse THRU tag!

[#682 (
    t: _
    parse "<tag>text</tag>" [thru <tag> copy t to </tag> end]
    t == "text"
)]

; THRU advances the input position correctly.

(
    i: 0
    parse "a." [
        any [thru "a" (i: i + 1 j: to-value if i > 1 [[end skip]]) j]
        end
    ]
    i == 1
)

[#1959
    (did parse "abcd" [thru "d" end])
]
[#1959
    (did parse "abcd" [to "d" skip end])
]

[#1959
    (did parse "<abcd>" [thru <abcd> end])
]
[#1959
    (did parse [a b c d] [thru 'd end])
]
[#1959
    (did parse [a b c d] [to 'd skip end])
]

; self-invoking rule

[#1672 (
    a: [a end]
    error? trap [parse [] a]
)]

; repetition

[#1280 (
    parse "" [(i: 0) 3 [["a" |] (i: i + 1)] end]
    i == 3
)]
[#1268 (
    i: 0
    <infinite?> = catch [
        parse "a" [any [(i: i + 1) (if i > 100 [throw <infinite?>])] end]
    ]
)]
[#1268 (
    i: 0
    parse "a" [while [(i: i + 1 j: to-value if i = 2 [[fail]]) j] end]
    i == 2
)]

; THEN rule

[#1267 (
    b: "abc"
    c: ["a" | "b"]
    a2: [any [b e: (d: [:e]) then fail | [c | (d: [fail]) fail]] d end]
    a4: [any [b then e: (d: [:e]) fail | [c | (d: [fail]) fail]] d end]
    equal? parse "aaaaabc" a2 parse "aaaaabc" a4
)]

; NOT rule

[#1246
    (did parse "1" [not not "1" "1" end])
]
[#1246
    (did parse "1" [not [not "1"] "1" end])
]
[#1246
    (not parse "" [not 0 "a" end])
]
[#1246
    (not parse "" [not [0 "a"] end])
]
[#1240
    (did parse "" [not "a" end])
]
[#1240
    (did parse "" [not skip end])
]
[#1240
    (did parse "" [not fail end])
]


; TO/THRU + bitset!/charset!

[#1457
    (did parse "a" compose [thru (charset "a") end])
]
[#1457
    (not parse "a" compose [thru (charset "a") skip end])
]
[#1457
    (did parse "ba" compose [to (charset "a") skip end])
]
[#1457
    (not parse "ba" compose [to (charset "a") "ba" end])
]
[#2141 (
    xset: charset "x"
    did parse "x" [thru [xset] end]
)]

; self-modifying rule, not legal in Revolt if it's during the parse

(error? trap [
    not parse "abcd" rule: ["ab" (remove back tail of rule) "cd" end]
])

[https://github.com/metaeducation/ren-c/issues/377 (
    o: make object! [a: 1]
    parse s: "a" [o/a: skip end]
    o/a = s
)]

; A couple of tests for the problematic DO operation

(did parse [1 + 2] [do [lit 3] end])
(did parse [1 + 2] [do integer! end])
(did parse [1 + 2] [do [integer!] end])
(not parse [1 + 2] [do [lit 100] end])
(did parse [reverse copy [a b c]] [do [into ['c 'b 'a]] end])
(not parse [reverse copy [a b c]] [do [into ['a 'b 'c]] end])

; AHEAD and AND are synonyms
;
(did parse ["aa"] [ahead text! into ["a" "a"] end])
(did parse ["aa"] [and text! into ["a" "a"] end])

; INTO is not legal if a string parse is already running
;
(error? trap [parse "aa" [into ["a" "a"]] end])


; Should return the same series type as input (Rebol2 did not do this)
; PATH! cannot be PARSE'd due to restrictions of the implementation
(
    a-value: first [a/b]
    parse as block! a-value [b-value: end]
    a-value = to path! b-value
)
(
    a-value: first [()]
    parse a-value [b-value: end]
    same? a-value b-value
)

; This test works in Rebol2 even if it starts `i: 0`, presumably a bug.
(
    i: 1
    parse "a" [any [(i: i + 1 j: if i = 2 [[end skip]]) j] end]
    i == 2
)

; Use experimental MATCH to get input on success, see #2165
; !!! This is a speculative feature, and is not confirmed for Beta/One

("abc" = match parse "abc" ["a" "b" "c" end])
(null? match parse "abc" ["a" "b" "d" end])


; GET-GROUP!
; These evaluate and inject their material into the PARSE, if it is not null.
; They act like a COMPOSE/ONLY that runs each time the GET-GROUP! is passed.

(did parse "aaabbb" [:([some "a"]) :([some "b"])])
(did parse "aaabbb" [:([some "a"]) :(if false [some "c"]) :([some "b"])])
(did parse "aaa" [:('some) "a" end])
(not parse "aaa" [:(1 + 1) "a" end])
(did parse "aaa" [:(1 + 2) "a" end])
(
    count: 0
    did parse ["a" "aa" "aaa"] [some [into [:(count: count + 1) "a"]] end]
)

; SET-GROUP!
; What these might do in PARSE could be more ambitious, but for starters they
; provide a level of indirection in SET.

(
    m: null
    word: 'm
    did all [
        parse [1020] [(word): integer!]
        word = 'm
        m = 1020
    ]
)

; LOGIC! BEHAVIOR
; A logic true acts as a no-op, while a logic false causes matches to fail

(did parse "ab" ["a" true "b" end])
(not parse "ab" ["a" false "b" end])
(did parse "ab" ["a" :(1 = 1) "b" end])
(not parse "ab" ["a" :(1 = 2) "b" end])


; QUOTED! BEHAVIOR
; Support for the new literal types

([[a b]] == parse [... [a b]] [to '[a b]])
(did parse [... [a b]] [thru '[a b] end])
(did parse [1 1 1] [some '1 end])

; Quote level is currently retained by the return value, but not by the
; captured content.
;
(did all [
    lit ''[] == parse lit ''[1 + 2] [copy x to end]
    x == [1 + 2]
])


; As alternatives to using SET-WORD! to set the parse position and GET-WORD!
; to get the parse position, Revolt has MARK and SEEK.  One ability this
; gives is to mark a variable without having it be a SET-WORD! and thus
; gathered by FUNCTION.  It also allows seeking to integer positions.
;
; Unlike R3-Alpha, changing the series being parsed is not allowed.
;
; !!! Feature does not currently allow marking a synthesized variable, or
; seeking a synthesized variable, e.g. `mark @(...)` or `seek @(...)`
(
    did all [
        parse "aabbcc" [some "a" mark x some "b" mark y: :x copy z to end]
        x = "bbcc"
        y = "cc"
        z = "bbcc"
    ]
)(
    pos: 5
    parse "123456789" [seek pos copy nums to end]
    nums = "56789"
)


; Multi-byte characters and strings present a lot of challenges.  There should
; be many more tests and philosophies written up of what the semantics are,
; especially when it comes to BINARY! and ANY-STRING! mixtures.  These tests
; are better than nothing...
(
    catchar: #"🐱"
    did parse #{F09F90B1} [catchar end]
)(
    cattext: "🐱"
    did parse #{F09F90B1} [cattext end]
)(
    catbin: #{F09F90B1}
    did parse "🐱" [catbin end]
)(
    catchar: #"🐱"
    did parse "🐱" [catchar end]
)

[
    (
        bincat: to-binary {C😺T}
        bincat = #{43F09F98BA54}
    )

    (did parse bincat [{C😺T} end])

    (did parse bincat [{c😺t} end])

    (not parse/case bincat [{c😺t} end])
]

(
    test: to-binary {The C😺T Test}
    did all [
        parse test [to {c😺t} copy x to space to end]
        x = #{43F09F98BA54}
        "C😺T" = to-text x
    ]
)


; With UTF-8 Everywhere, all strings are kept in UTF-8 format all the time.
; This makes it feasible to invoke the scanner during PARSE of a TEXT!.
; The feature was added only as a quick demo and needs significant design,
; testing, and development.  But it was added to show what kinds of things
; are possible now.

(did all [
    parse "abc [d e f] {Hello}" ["abc" set b block! set s text!]
    b = [d e f]
    s = {Hello}
])


(did all [
    parse text: "a ^/ " [
        any [newline remove [to end] | "a" [remove [to newline]] | skip]
    ]
    text = "a^/"
])


(did parse "a" [some [to end] end])

[https://github.com/metaeducation/ren-c/issues/1032 (
    s: {abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ}
    t: {----------------------------------------------------}
    for n 2 50 1 [
        sub: copy/part s n
        parse sub [any [
            remove skip
            insert "-"
        ]]
        if sub != copy/part t n [fail "Incorrect Replacement"]
    ]
    true
)]

[(
    countify: function [things data] [
        counts: make map! []
        rules: collect [
            for-each t things [
                counts/(t): 0
                keep t
                keep/only compose/deep '(counts/(t): me + 1)
                keep/line '|
            ]
            keep/only 'fail
        ]
        parse data (compose/deep [
            any [((rules))]
            end
        ]) then [
            collect [
                for-each [key value] counts [
                    keep key
                    keep value
                ]
            ]
        ] else [
            <outlier>
        ]
    ]
    true
)(
    ["a" 3 "b" 3 "c" 3] = countify ["a" "b" "c"] "aaabccbbc"
)(
    <outlier> = countify ["a" "b" "c"] "aaabccbbcd"
)]

[
    https://github.com/rebol/rebol-issues/issues/2393
    (not parse "aa" [some [#"a"] reject])
    (not parse "aabb" [some [#"a"] reject some [#"b"]])
    (not parse "aabb" [some [#"a" reject] to end])
]

; !!! R3-Alpha introduced a controversial "must make progress" rule, where
; something like an empty string does not make progress on a string parse
; so even if it doesn't fail, it fails the whole parse.  Red has all of
; these tests pass.  Revolt is questioning the progress rule, believing the
; benefit of infinite-loop-avoidance is not worth the sacrifice of logic.
[
    (not parse "ab" [to [""] "ab" end])
    (did parse "ab" [to ["a"] "ab" end])
    (did parse "ab" [to ["ab"] "ab" end])
    (not parse "ab" [thru [""] "ab" end])
    (did parse "ab" [thru ["a"] "b" end])
    (not parse "ab" [thru ["ab"] "" end])
]

; Ren-C made it possible to use quoted WORD!s in place of CHAR! or TEXT! to
; match in strings.  This gives a cleaner look, as you drop off 3 vertical
; tick marks from everything like ["ab"] to become just ['ab]
;
("c" = parse "abbbbbc" ['a some ['b]])
("" = parse "abbbbc" ['ab some ['bc | 'b]])
("def" = parse "abc10def" ['abc '10])

(
    byteset: make bitset! [0 16 32]
    did parse #{001020} [some byteset end]
)
