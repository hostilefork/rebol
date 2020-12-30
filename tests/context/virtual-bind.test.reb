; %virtual-bind.test.reb
;
; Virtual binding is a mechanism for creating a view of a block where its
; binding is seen differetly, without disrupting other views of that block.
; It is exposed via the IN and USE constructs, and is utilized by FOR-EACH
; and MAKE OBJECT!
;

; Basic example of virtual binding not disrupting the bindings of a block.
(
    obj1000: make object! [x: 1000]
    block: [x + 20]
    bind block obj1000

    obj284: make object! [x: 284]
    did all [
        1020 = do block
        304 = do in obj284 block
        1020 = do block
    ]
)

; One of the first trip-ups for virtual binding was Brett's PARSING-AT,
; which exposed an issue with virtual binding and PARSE (which was applying
; specifiers twice in cases of fetched words).  This is isolated from that
(
    make-rule: func [/make-rule] [  ; refinement helps recognize in C Probe()
        use [rule position][
            rule: compose/deep [
                [[position: "a"]]
            ]
            use [x] compose/deep [
                [(as group! rule) rule]
            ]
        ]
    ]
    did all [
        r: make-rule
        "a" = do first first first r  ; sanity check with plain DO
        parse "a" r  ; this was where the problem was
    ]
)

; Compounding specifiers is tricky, and many of the situations only arise
; when you return still-relative material (e.g. from nested blocks in a
; function body) that has only been derelativized at the topmost level.
; Using GROUP!s is a good way to catch this since they're easy to evaluate
; in a nested faction.
[
    (
        add1020: func [x] [use [y] [y: 1020, '(((x + y)))]]
        add1324: func [x] [
            use [z] compose/deep <*> [
                z: 304
                '(((z + (<*> add1020 x))))
            ]
        ]
        add2020: func [x] [
            use [zz] compose/deep <*> [
                zz: 696
                '(((zz + (<*> add1324 x))))
            ]
        ]

        true
    )

    (1324 = do add1020 304)
    (2020 = do add1324 696)
    (2021 = do add2020 1)
]

[
    (
        group: append '() use [x y] [x: 10, y: 20, '((x + y))]
        group = '(((x + y)))
    )

    ; Basic robustness
    ;
    (30 = do group)
    (30 = do compose [(group)])
    (30 = do compose [(group)])
    (30 = do compose/deep [do [(group)]])
    (30 = reeval does [do compose [(group)]])

    ; Unrelated USE should not interfere
    ;
    (30 = use [z] compose [(group)])
    (30 = use [z] compose/deep [do [(group)]])

    ; Related USE should override
    ;
    (110 = use [y] compose [y: 100, (group)])
    (110 = use [y] compose/deep [y: 100, do [(group)]])

    ; Chaining will affect any values that were visible at the time of the
    ; USE (think of it the way you would as if the BIND were run mutably).
    ; In the first case, the inner use sees the composed group's x and y,
    ; but the compose is run after the outer use, so the x override is unseen.
    ; Moving the compose so it happens before the use [x] runs will mean the
    ; x gets overridden as well.
    ;
    (110 = use [x] [x: 1000, use [y] compose [y: 100, (group)]])
    (1100 = use [x] compose/deep [x: 1000, use [y] [y: 100, do [(group)]]])
]
