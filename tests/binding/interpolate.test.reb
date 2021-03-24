; %interpolate.test.reb
;
; String interpolation is the concept of variables being substituted into text
; strings as a language feature...where the relationship between the variable
; and the string is managed for you:
;
; https://en.wikipedia.org/wiki/String_interpolation

; Basic demonstration: the GROUP! frame specifier (into which LET X is injected)
; is used to derelativize the "X $(x)" into the argument slot of INTERPOLATE.
; It uses this information to resolve the variables.
(
    let x: 10
    "X 10" = interpolate "X $(x)"
)

; In this case, the derelativization happens when the variable is assigned.
; The STR reference fetches a fully resolved variable.
(
    let x: 10
    let str: "X $(x)"
    "X 10" = interpolate str
)

; When a function runs, the frame is used as the specifier.  But it also
; inherits from the specifier *of the body*.  This means anything the body
; would know about, the interpolation will see...as well as arguments to
; the function in the frame.
(
    let x: 10
    let foo: func [y] [interpolate "X $(x) Y $(y)"]
    "X 10 Y 20" = foo 20
)

; Here we will return an entire block, that preserves its binding...both to
; INTERPOLATE itself and to the parameter it has captured.
(
    let body-maker: func [x] [return [interpolate "X $(x)"]]
    "X 20" = do body-maker 20
)

; We wouldn't expect definitions outside the function to override the argument
(
    let x: 10
    let body-maker: func [x] [return [interpolate "X $(x)"]]
    "X 20" = do body-maker 20
)

; In this case, the body is generated by a function which returns already
; bound code as a BLOCK!.  The definition of the X in that bound code is to
; the argument from the invocation of BODY-MAKER, which was 20 at the time
; of definition.  However, the binding composition puts the second function
; in a position of overriding the meaning of X.
(
    let body-maker: func [x] [return [interpolate "X $(x)"]]
    let foo: func [x] (body-maker 20)
    "X 10" = foo 10
)

(
    let body-maker: func [x] [return [interpolate "X $(x)"]]
    let foo: func [x] [do (body-maker 20)]
    "X 10" = foo 10
)

; Here we take a twist where we unbind the returned block from the body-maker.
; This means it doesn't know what INTERPOLATE is, and it has no context
; assigned to the string.  The knowledge of both are then picked up from the
; specifier of the function.

(
    let body-maker: func [x] [return nonbound [interpolate "X $(x)"]]
    let foo: func [x] [do (body-maker 20)]
    "X 10" = foo 10
)