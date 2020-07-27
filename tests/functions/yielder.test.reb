; YIELDER is the basis of GENERATOR, which allows a parameterized call to
; each invocation of the routine.  GENERATOR simply has an empty spec for
; making it convenient to call (like DOES)


; Interoperability with multiple return values
(
    y: yielder [/other [<output>]] [
        if other [set other <foo>]
        yield "FOO!"
        if other [set other <bar>]
        yield "BAR!"
    ]
    did all [
        "FOO!" = [a b]: y
        a = "FOO!"
        b = <foo>

        "BAR!" = y

        null? y
        null? y
    ]
)

; Non-Channel based parallel to:
; https://www.geeksforgeeks.org/coroutine-in-python/
; Something of a mismatch; see channel-based version in %channel.test.reb
; for an improved method.  (Still good as a yielder test.)
(
    stuff: []
    log: func [x] [append/only stuff x]

    producer: func [sentence [text!] next-coroutine [action!]] [ 
        let tokens: split sentence space 
        for-each token tokens [ 
            next-coroutine token  ; Python says `next_coroutine.send(token)`
        ] 
        next-coroutine null  ; Python says `next_coroutine.close()`
    ]
  
    pattern-filter: func [next-coroutine [action!] /pattern [text!]] [
        pattern: default ["ing"]
        log <start-filtering>

        return yielder [token [<opt> text!]] [
            while [token] [  ; Python does a blocking `token = (yield)`
                if find token pattern [
                    next-coroutine token
                ]
                yield void 
            ]
            next-coroutine null
            log <done-filtering>
        ]
    ]

    emit-token: func [] [
        log <start-emitting>

        return yielder [token [<opt> text!]] [
            while [token] [  ; Python does a blocking `token = (yield)`
                log token
                yield void
            ]
            log <done-emitting>
        ] 
    ]

    et: emit-token
    pf: pattern-filter :et
  
    sentence: "Bob is running behind a fast moving car"
    producer sentence :pf

    stuff = [
        <start-emitting>
        <start-filtering>
        "running"
        "moving"
        <done-emitting>
        <done-filtering>
    ]
)
