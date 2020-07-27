; GENERATOR is the 0-arity convenience form of YIELDER.

; Basic disallowal of re-entry
(
    g: generator [g]
    'yielder-reentered = (trap [g])/id
)


; Errors that cross a yielder will always fail thereafter
(
    g: generator [yield 1 fail "Bad!" yield 2]

    did all [
        g = 1
        error? trap [g]
        (trap [g])/id = 'yielder-errored
        (trap [g])/id = 'yielder-errored
    ]
)

; Throws that cross a yielder are equivalent to exiting it normally
(
    data: copy []
    gmaker: func [] [
        g: generator [yield 1 yield 2 yield 3 return :g yield 4]
        cycle [
            append data g
        ]
    ]
    did all [
        action? result: gmaker
        null = result
        null = result
        data = [1 2 3]
    ]
)


; Trying to run YIELD while a generator is suspended is an error
(
    stolen-yield: void
    g: generator [
        stolen-yield: :yield
        yield 1
        yield 2
    ]

    did all [
        g = 1
        'frame-not-on-stack = (trap [stolen-yield 3])/id
    ]
)


; WHILE loop with generator
(
    g: generator [while [true] [yield 1]]
    sum: 0
    loop 1000 [sum: sum + g]
    sum = 1000
)
(
    [10 20 30] = collect [
        while generator [
            yield 1
            yield 2
            yield 3
        ] func [x] [
            keep x * 10
        ]
    ]
)
(
    x: 1
    g: generator [
        while [true] [
            x: 10 + yield x
        ]
    ]
    [1 11 21 31 41 51 61 71 81 91] = array/initial 10 :g
)


; ANY with generator
(
    g: generator [
        yield 1
        yield any [
            if 1 > 2 [<bad-news>]
            find "abc" "d"
        ]
        any [
            false
            yield 2
        ]
    ]

    did all [
        g = 1
        g = 2
        g = null
        g = null
    ]
)


; ENCLOSE* compatibility
(
    g: generator [
        yy: enclose* 'yield func [f] [
            f/value: me * 10
            return 1 + do f
        ]
        yy yy yy 10
    ]
    did all [
       g = 100
       g = 1010
       g = 10110
       g = null
       g = null
    ]
)

; ENCLOSE compatibility -- an ENCLOSEd ENCLOSE with added help derivation
; (more complex and harder to debug if something goes wrong than ENCLOSE*)
(
    g: generator [
        yy: enclose 'yield func [f] [
            f/value: me * 10
            return 1 + do f
        ]
        yy yy yy 10
    ]
    did all [
       g = 100
       g = 1010
       g = 10110
       g = null
       g = null
    ]
)


; Typically generators cannot return NULL, as that is reserved for when a
; generator has run out of values (by convention).  It is possible to work
; around this limitation by hooking the generator and the yield.
;
; !!! Over time, if this turns out to be a common desire then it may be
; worth offering it as a feature.
;
(
    n-generator: func [body [block!]] [
        let g: generator compose [
            yield: enclose 'yield func [f] [
                f/value: quote :f/value
                return unquote do f
            ]
            (as group! body)
        ]
        return func [] [
            unquote (g else [
                return null  ; when g finishes, returns a *non-quoted* null
            ])
        ]
    ]

    a: b: c: void

    g: n-generator [
        a: yield 1
        b: yield null
        c: yield 2
    ]

    did all [
        g = 1
        g = null
        g = 2
        g = null
        g = null

        a = 1
        b = null
        c = 2
    ]
)

; !!! This way of writing n-generator does not work.
; The reasoning seems tied up in how CHAIN is.  Research.
;
( comment [
    n-generator: func [body [block!]] [
        let g: generator compose [
            yield: enclose 'yield func [f] [
                f/value: quote :f/value
                return unquote do f
            ]
            (as group! body)
        ]
        return chain [:g | :unquote]
    ]

    a: b: c: void

    g: n-generator [
        a: yield 1
        b: yield null
        c: yield 2
    ]

    did all [
        g = 1
        g = null
        g = 2
        g = null
        g = null

        a = 1
        b = null
        c = 2
    ]
] true)  ; Skip this test for now


; COMPOSE test
(
    g: generator [
        yield compose/deep [
            So (yield "How") [(yield "About")] (yield "This") ?
        ]
    ]

    did all [
        g = "How"
        g = "About"
        g = "This"
        g = [So "How" ["About"] "This" ?]
        g = null
        g = null
    ]
)


; DELIMIT (SPACED) test
(
    g: generator [
        n: 1
        while [true] [
            yield spaced [yield "Step" yield n]
            n: n + 1
        ]
    ]

    did all [
        g = "Step"
        g = 1
        g = "Step 1"
        g = "Step"
        g = 2
        g = "Step 2"
    ]
)


; PARSE tests
(
    vowelizer: func [text [text!]] [
        let vowel: charset "aeiou"
        return generator [
            let ch: void
            parse text [any [
                [to vowel
                set ch skip (yield ch)]
                | skip
            ]]
        ]
    ]

    v: vowelizer "The Quick Brown Fox Jumped Over The Lazy Dogs"
    ([#"e" #"u" #"i" #"o" #"o" #"u" #"e" #"O" #"e" #"e" #"a" #"o" _]
        = reduce [v v v v v v v v v v v v try v])
)
