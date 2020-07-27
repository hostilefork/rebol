; "GENERATE" usermode generator test
;
; Generate was a usermode implementation of a generator by @giuliolunati.
; It predated the stackless YIELDER and GENERATOR, and lacked the benefit
; of their generality.
;
; It also had features that are not included out of the box for YIELDER:
;
; * There was a /RESET that let you provide a block of code to run in the
;   local environment of the generator, so you could update variables.
;
; * A `count` field was implicitly incremented from 1 and reset by /RESET.
;
; The YIELDER attempts to be fully generic, and has no prescribed parameters.
; It's possible to make wrapping functions that do these features and more,
; and by being more general it likely combines better for most tasks.
;
; However, since GENERATE was written and had some tests...it's good to keep
; around as test code!  Implementations of the same functionality are in the
; %generator.test.reb file--done a different way.


(
    generate: func [ "Make a generator."
        init [block!] "Init code"
        condition [block! blank!] "While condition"
        iteration [block!] "Step code"
    ][
        let words: make block! 2
        for-each x reduce [init condition iteration] [
            if not block? x [continue]
            let w: collect-words/deep/set x
            if not empty? intersect w [count result] [ fail [
                "count: and result: set-words aren't allowed in" mold x
            ]]
            append words w
        ]
        let spec: compose [/reset [block!] <static> ((unique words)) count]
        let body: compose/deep [
            if reset [count: reset return]
            if block? count [
                let result: bind count 'count
                count: 1
                return do result
            ]
            count: me + 1
            let result: (to group! iteration)
            ((either empty? condition
                [[ return result ]]
                [compose [ return if (to group! condition) [result] ]]
            ))
        ]
        let f: function spec body
        f/reset init
        return :f
    ]
    true
)

( { GENERATE }
    { Start with 1 then double while x < 100 }
    {  => 1 2 4 8 16 32 64  }
    for-each x sequence: generate [x: 1] [x < 100] [x: 2 * x] [t: x]
    t = 64
)
( { GENERATE/RESET }
    { restart sequence from 5}
    { => 5, 10, 20, 40, 80 }
    sequence/reset [x: 5]
    for-each x :sequence [t: x]
    t = 80
)( { GENERATE, use COUNT }
    { Start with 1, step 2, 3 terms }
    { => 1, 3, 6, 10 }
    for-each x sequence: generate [i: count] [count <= 4] [i: i + count] [t: x]
    t = 10
)
( { GENERATE, no stop }
    { Fibonacci numbers, forever }
    for-each x generate
        [a: b: 1]
        _
        [c: a + b a: b b: c]
    [
        t: x
        if x >= 10 [break] { <- manual break }
    ]
    t = 13
)
( { GENERATE, 20 prime numbers }
    for-each x generate [primes: mutable [2] n: 2] [count <= 20] [
        forever [n: n + 1 nop: true for-each p primes [
            if (n mod p = 0) [break]
            if (p * p > n) [nop: false break]
        ] if not nop [break]]
        append primes n
        n
    ] [ t: x ]
    t = 71
)


;== THE ABOVE EXAMPLES DONE WITH GENERATOR / YIELDER ==

[
    (
        sequence: func [/reset [integer!] <static> x (1) g (null)] [
            if reset [x: reset | g: null | return]
            reeval g: default [
                generator [
                    while [x < 100] [
                        yield x
                        x: 2 * x
                     ]
                ]
            ]
        ]
        true
    )

    ; Start with 1 then double while x < 100
    ; => 1 2 4 8 16 32 64
    (
        for-each x :sequence [t: x]
        t = 64
    )

    ; Restart sequence from 5
    ; => 5, 10, 20, 40, 80
    (
        sequence/reset 5
        for-each x :sequence [t: x]
        t = 80
    )
]

; Start with 1, step 2, 3 terms
; => 1, 3, 6, 10
(
    sequence: yielder [<local> i count] [
        count: 1
        i: count
        while [count <= 4] [
            yield i
            count: count + 1
            i: i + count
        ]
    ]

    for-each x :sequence [t: x]
    t = 10
)

; Generate Fibonacci numbers infinitely
(
    sequence: yielder [<local> a b c] [
        a: b: 1
        cycle [
            yield c: a + b
            a: b
            b: c
        ]
    ]

    for-each x :sequence [
        t: x
        if x >= 10 [break]  ; manual break
    ]
    t = 13
)

; 20 prime numbers
(
    sequence: generator [
        primes: [2]  ; starts with one prime...
        n: 2
        count-up count 19 [  ; ...add 19 more to get 20
            cycle [
                n: n + 1
                is-prime: false
                for-each p primes [
                    if (n mod p = 0) [break]
                    if (p * p > n) [is-prime: true break]
                ]
                if is-prime [stop]  ; stop the cycle and yield
            ]
            append primes n
            yield n
        ]
    ]

    for-each x :sequence [t: x]
    t = 71
)
