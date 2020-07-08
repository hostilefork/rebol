; MULTIPLE RETURNS TESTS
;
; Multiple returns in Revolt are done in a way very much in like historical
; Rebol, where WORD! or PATH! gets passed in to a routine which is the
; variable that is then set by the code.
;
; The difference is that it offers the ability to specifically label such
; refinements as <output>, and if this is done then it will participate
; in the evaluator with the SET-BLOCK! construct, which will do an ordered
; injection of parameters from the left-hand side.  It takes care of
; pre-composing any PATH!s with GROUP!s in them, as well as voiding the
; variables.
;
; This results in functions that can be used in the traditional way or
; that can take advantage of the shorthand.

[
    (test: func [x /y [<output>] /z [<output>]] [
        if not null? y [
            assert [void? get/any y]
            set y <y-result>
        ]
        if not null? z [
            assert [void? get/any z]
            set z <z-result>
        ]

        return 304
    ]
    true)

    (304 = test 1020)

    (did all [
        304 = [a]: test 1020
        a = 304
    ])

    (did all [
        304 = [b c]: test 1020
        b = 304
        c = <y-result>
    ])

    (did all [
        304 = [d e f]: test 1020
        d = 304
        e = <y-result>
        f = <z-result>
    ])

    (did all [
        304 = [g _ h]: test 1020
        g = 304
        h = <z-result>
    ])
]

