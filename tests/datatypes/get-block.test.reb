; GET-BLOCK! tests

(get-block! = type of first [:[a b c]])
(get-path! = type of first [:[a b c]/d])

; !!! Current thinking is that GET-BLOCK! will be inert in the evaluator, and
; used to subvert voidification of branches.
(
    a: 10 b: 20
    :[a b] = :[a b]
)
