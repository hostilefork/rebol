; DELIMIT is specialized to implement SPACED and UNSPACED

; !!! The routines experienced fairly heavy usage, so comprehensive tests
; were not initially written for them.

(null? delimit #" " [])
("1 2" = delimit #" " [1 2])

(null? delimit "unused" [])
("1" = delimit "unused" [1])
("12" = delimit "" [1 2])

("1^/^/2" = delimit #"^/" ["1^/" "2"])

; Empty text is distinct from BLANK/null
(" A" = delimit ":" [_ "A" null])
(":A:" = delimit ":" ["" "A" ""])

("a:bc:d" = delimit ":" ["a" "bc" "d"])

("a bc d" = spaced ["a" unspaced ["b" "c"] "d"])

(null = spaced [if false ["a"] if false ["b"]])
("" = spaced [if false ["a"]  if true [""] if false ["b"]])

