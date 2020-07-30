; Better than nothing channel tests
; Channels and GO are based on corresponding Go language constructs

(
    done: make-chan

    captured: void
    foo: func [x] [
       captured: x
       send-chan done true
    ]

    x: 20
    go foo x + 1000
    x: 304

   receive-chan done 

   did all [
       x = 304
       captured = 1020
   ]
)

(
    stuff: []
    log: func [x] [append/only stuff x]

    c: make-chan/capacity 1  ; can send 1 element w/o blocking
    done: make-chan  ; unbuffered

    worker: func [c done <local> data] [
        log <entering-print>
        while [data: receive-chan c] [
            log compose [receive (data)]
        ]
        log <exiting-print>
        send-chan done true
    ]

    send-chan c #before-the-go  ; one element sent before wait

    log <before-go-worker>
    go worker c done
    log <after-go-worker>

    send-chan c #after-the-go  ; 1-unit buffer, schedules now
    send-chan c 1
    send-chan c 2

    close-chan c

    log <checking-done>
    receive-chan done  ; wait until printer says its finished

    log <main-finished>

    stuff = [
        <before-go-worker>
        <after-go-worker>
        <entering-print>
        [receive #before-the-go]
        [receive #after-the-go]
        <checking-done>
        [receive 1]
        [receive 2]
        <exiting-print>
        <main-finished>
    ]
)

; Channel-based parallel to:
; https://www.geeksforgeeks.org/coroutine-in-python/
(
    stuff: []
    log: func [x] [append/only stuff x]

    producer: func [sentence [text!]] [ 
        let out: make-chan
        let tokens: split sentence space 

        go (func [] [
            for-next t tokens [
                send-chan out t/1
            ]
            close-chan out
        ])

        return out
    ]
  
    pattern-filter: func [in "channel" /pattern [text!]] [
        pattern: default ["ing"]
        log <start-filter>

        let out: make-chan

        go (func [<local> token] [
            while [token: receive-chan in] [
                if find token pattern [
                    send-chan out token
                ]
            ]
            close-chan out
            log <done-filtering>
        ])

        return out
    ]

    emit-token: func [in] [
        log <start-emitter>

        let done: make-chan
        go (func [<local> token] [
            while [token: receive-chan in] [
                log token
            ]
            close-chan done
            log <done-emitting>
        ])

        return done
    ]

  
    sentence: "Bob is running behind a fast moving car"

    unfiltered: producer sentence
    filtered: pattern-filter unfiltered
    done: emit-token filtered

    receive-chan done

    stuff = [
        <start-filter>
        <start-emitter>
        "running"
        "moving"
        <done-filtering>
        <done-emitting>
    ]
)
