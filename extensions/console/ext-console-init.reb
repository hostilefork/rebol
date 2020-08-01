REBOL [
    Title: "Console Extension (Revolt's Read-Eval-Print-Loop, ie. REPL)"

    Name: console
    Type: Module

    Options: []  ; !!! If ISOLATE, wouldn't see LIB/PRINT changes, etc.

    Rights: {
        Copyright 2016-2020 Revolt Open Source Contributors
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Description: {
        This is a rich, skinnable console for Revolt--where basically all the
        implementation is itself userspace Revolt code.  Documentation for the
        skinning hooks exist here:

        https://github.com/r3n/reboldocs/wiki/User-and-Console

        The HOST-CONSOLE Revolt function is invoked in a loop by a small C
        main function (see %main/main.c).  HOST-CONSOLE does not itself run
        arbitrary user code with DO.  That would be risky, because it actually
        is not allowed to fail or be canceled with Ctrl-C.  Instead, it just
        gathers input...and produces a block which is returned to C to
        actually execute.

        This design allows the console to sandbox potentially misbehaving
        skin code, and fall back on a default skin if there is a problem.
        It also makes sure that that user code doesn't see the console's
        implementation in its backtrace.

        !!! While not implemented in C as the R3-Alpha console was, this
        code relies upon the INPUT function to communicate with the user.
        INPUT is a black box that reads whole lines from the "console port",
        which is implemented via termios on POSIX and the Win32 Console API
        on Windows:

        https://blog.nelhage.com/2009/12/a-brief-introduction-to-termios/
        https://docs.microsoft.com/en-us/windows/console/console-functions

        Someday in the future, the console port itself should offer keystroke
        events and allow the line history (e.g. Cursor Up, Cursor Down) to be
        implemented in Revolt as well.
     }
]


boot-print: redescribe [
    "Prints during boot when not quiet."
](
    ; !!! Duplicates code in %main-startup.reb, where this isn't exported.
    enclose 'print func [f] [if not system/options/quiet [do f]]
)

loud-print: redescribe [
    "Prints during boot when verbose."
](
    ; !!! Duplicates code in %main-startup.reb, where this isn't exported.
    enclose 'print func [f] [if system/options/verbose [do f]]
)


; Define base console! behaviors.  Custom console skins derive from this.
;
; If a console skin has an error while running, the error will be trapped and
; the system will revert to using a copy of this base object.
;
console!: make object! [
    name: _
    repl: true  ; used to identify this as a console! object
    is-loaded: false  ; if true then this is a loaded (external) skin
    was-updated: false  ; if true then console! object found in loaded skin
    last-result: void  ; last evaluated result (sent by HOST-CONSOLE)

    === APPEARANCE (can be overridden) ===

    prompt: {>>}
    result: {==}
    warning: {!!}
    error: {**}  ; errors FORM themselves, so this is not used yet
    info: {(i)}  ; was `to-text #{e29398}` for "(i)" symbol, caused problems
    greeting:
{Welcome to Revolt.  For more information please type in the commands below:

  HELP    - For starting information
  ABOUT   - Information about your Revolt
  CHANGES - What's different about this version}

    print-greeting: method [
        return: <void>
        {Adds live elements to static greeting content (build #, version)}
    ][
        boot-print [
            "R E V [o] L T" "-"
            mold compose [version: (system/version) build: (system/build)]
            newline
        ]

        boot-print greeting
    ]

    print-prompt: method [return: <void>] [
        ;
        ; Note: See example override in skin in the Debugger extension, which
        ; adds the stack "level" number and "current" function name.

        ; We don't want to use PRINT here because it would put the cursor on
        ; a new line.
        ;
        write-stdout unspaced prompt
        write-stdout space
    ]

    print-result: method [return: <void> v [<opt> any-value!]] [
        if void? last-result: get/any 'v [
            return  ; e.g. result of PRINT or HELP, best to output nothing.
        ]

        case [
            null? :v [
                print "; null"  ; no representation, use comment
            ]

            free? :v [
                ; Molding a freed value would cause an error...which is
                ; usually okay (you shouldn't be working with freed series)
                ; but if we didn't special case it here, the error would seem
                ; to be in the console code itself.
                ;
                print-error make error! "Series data unavailable due to FREE"
            ]

            port? :v [
                ; PORT!s are returned by many operations on files, to
                ; permit chaining.  They contain many fields so their
                ; molding is excessive, and there's not a ton to learn
                ; about them.  Cut down the output more than the mold/limit.
                ;
                print [result "#[port! [...] [...]]"]
            ]
        ]
        else [
            ; print the first 20 lines of the first 2048 characters of mold
            ;
            pos: molded: mold/limit :v 2048
            loop 20 [
                pos: next (find pos newline else [break])
            ] then [  ; e.g. didn't break
                insert clear pos "..."
            ]
            print [result (molded)]
        ]
    ]

    print-warning: method [s] [print [warning reduce s]]

    print-error: method [e [error!]] [
        if :e/file = 'tmp-boot.r [
            e/file: e/line: _  ; errors in console showed this, junk
        ]
        print [e]
    ]

    print-halted: method [] [
        print "** Interrupted by Ctrl-C or HALT instruction"
    ]

    print-info: method [s] [print [info reduce s]]

    print-gap: method [] [print newline]

    === BEHAVIOR (can be overridden) ===

    input-hook: method [
        {Receives line input, parse/transform, send back to CONSOLE eval}

        return: "null if canceled, otherwise processed text line input"
            [<opt> text!]
    ][
        ask text!
    ]

    dialect-hook: method [
        {Receives code block, parse/transform, send back to CONSOLE eval}
        b [block!]
    ][
        ; By default we do nothing.  But see the Debug console skin for
        ; example of binding the code to the currently "focused" FRAME!, or
        ; this example on the forum of injecting the last value:
        ;
        ; https://forum.rebol.info/t/1071

        b
    ]

    shortcuts: make object! compose/deep [
        d: [dump]
        h: [help]
        q: [quit]
        dt: [delta-time]
        dp: [delta-profile]

        list-shortcuts: [print [system/console/shortcuts]]
        changes: [
            browse join https://github.com/metaeducation/ren-c/blob/master/ [
                %CHANGES.md "#"
                system/version/1 system/version/2 system/version/3
            ]
        ]
        topics: [
            browse https://r3n.github.io/topics/
        ]
    ]

    === HELPERS (could be overridden!) ===

    add-shortcut: method [
        {Add/Change console shortcut}
        return: <void>
        name [any-word!] "Shortcut name"
        block [block!] "Command(s) expanded to"
    ][
        extend shortcuts name block
    ]
]


start-console: function [
    "Called when a REPL is desired after command-line processing, vs quitting"

    return: <void>
    /skin "Custom skin (e.g. derived from MAKE CONSOLE!) or file"
        [file! object!]
    <static>
        o (system/options)  ; shorthand since options are often read/written
][
    === MAKE CONSOLE! INSTANCE FOR SKINNING ===

    ; Instantiate console! object into system/console.  This is updated via
    ; %console-skin.reb if in system/options/resources

    skin-file: case [
        file? skin [skin]
        object? skin [blank]
        default [%console-skin.reb]
    ]

    loud-print "Starting console..."
    loud-print newline
    proto-skin: match object! skin else [make console! []]
    skin-error: _

    all [
        skin-file
        not find o/suppress skin-file
        o/resources
        exists? skin-file: join o/resources skin-file
    ] then [
        trap [
            new-skin: do load skin-file

            ; if loaded skin returns console! object then use as prototype
            all [
                object? new-skin
                select new-skin 'repl  ; quacks like REPL, it's a console!
            ] then [
                proto-skin: new-skin
                proto-skin/was-updated: true
                proto-skin/name: default ["updated"]
            ]

            proto-skin/is-loaded: true
            proto-skin/name: default ["loaded"]
            append o/loaded skin-file

        ] then e => [
            skin-error: e  ; show error later if `--verbose`
            proto-skin/name: "error"
        ]
    ]

    proto-skin/name: default ["default"]

    system/console: proto-skin

    === HOOK FOR HELP ABOUT LAST ERROR ===

    ; The WHY command lets the user get help about the last error printed.
    ; To do so, it has to save the last error.  Adjust the error printing
    ; hook to save the last error printed.  Also inform people of the
    ; existence of the WHY function on the first error delivery.
    ;
    proto-skin/print-error: adapt :proto-skin/print-error [
        if not system/state/last-error [
            system/console/print-info "Info: use WHY for error information"
        ]

        system/state/last-error: e
    ]

    === PRINT BANNER ===

    if o/about [
        boot-print make-banner boot-banner  ; the fancier banner
    ]

    system/console/print-greeting

    === VERBOSE CONSOLE SKINNING MESSAGES ===

    loud-print [newline {Console skinning:} newline]
    if skin-error [
        loud-print [
            {  Error loading console skin  -} skin-file LF LF
            skin-error LF LF
            {  Fix error and restart CONSOLE}
        ]
    ] else [
       loud-print [
            space space
            if proto-skin/is-loaded [
                {Loaded skin}
            ] else [
                {Skin does not exist}
            ]
            "-" skin-file
            "(CONSOLE" if not proto-skin/was-updated [{not}] "updated)"
        ]
    ]
]


console-impl: func [
    {Revolt ACTION! that is called from C in a loop to implement the console}

    return: "Console is run as a GO routine, so return result does not matter"
        <void>
    requests "Channel to make sandboxed evaluation requests on"
        [object!]
    responses "Channel of (quoted) responses from evaluator (else ERROR!s)"
        [object!]
    resumable "Is the RESUME function allowed to exit this console"
        [logic!]
    skin "Console skin to use if the console has to be launched"
        [<opt> object! file!]
][
    let execute: func [
        {Ask the C engine to run code with Ctrl-C enabled}

        return: [<opt> any-value!]
        instruction "BLOCK! is code, INTEGER! is exit request"
            [integer! block!]
        /error "Return errors to caller vs. raising console internal error"
            [<output>]
        /debuggable "Should the execution be able to be debugged"
    ][
        all [
            not debuggable
            block? instruction
        ] then [
            instruction: as group! instruction  ; signal debugger invisibility
        ]

        send-chan requests instruction

        ; The initial protocol for the `result` is that it is quoted if it
        ; is valid, or an ERROR! if an error occurred.  Hence a QUOTED! error
        ; means that the code actually evaluated to an ERROR! value.
        ;
        let r: receive-chan responses

        === {QUIT HANDLING} ===

        ; A QUIT can be issued from either code asked to be executed on behalf
        ; of the user, or from console code...even PRINT-PROMPT (!)  At the
        ; moment this is allowed, but maybe there should be rules about it.

        ; https://en.wikipedia.org/wiki/Exit_status
        all [
            error? :r
            r/id = 'no-catch
            :r/arg2 = :QUIT  ; throw's /NAME
        ] then [
            execute switch type of get* 'r/arg1 [
                void! [0]  ; plain QUIT, no /WITH, call that success

                logic! [either :r/arg1 [0] [1]]  ; logic true is success

                integer! [r/arg1]  ; Note: may be too big for status range

                error! [1]  ; no default error-to-int mapping at present

                default [1]  ; generic error code
            ]
            assert ["Unreachable (should we overload RETURN or EXIT?)"]
            return
        ]

        === {OTHER ERROR HANDLING} ===

        ; If the caller requested the /ERROR return result, then it will be
        ; returned alongside a void result.  But if no error was requested,
        ; then this will fail...triggering a console internal error.

        if error? r [
            if error [
                set error r
                return void
            ] else [
                fail r  ; ...will cause reversion to the default skin
            ]
        ] else [
            if error [
                set error null
            ]
        ]

        return unquote r  ; If not an error, R was the quoted eval result
    ]

    === {DO STARTUP HOOK IF THIS IS THE FIRST TIME THE CONSOLE HAS RUN} ===

    ; !!! This was the first call before, and it would do some startup.
    ; Now it's probably reasonable to assume if there's anything to be
    ; done on a first call (printing notice of "you broke into debug" or
    ; something like that) then whoever broke into the REPL takes
    ; care of that.
    ;
    if (unset? 'system/console) or [not system/console] [
        execute compose [start-console/skin '(skin)]
    ]

    === {(PROMPT,) READ, EVAL, PRINT LOOP: (P)REPL)} ===

    ; This loop is enclosed in a TRAP, because an EXECUTE that does not ask
    ; to trap errors is running console skin code.  An error in that code
    ; means basic console functionality isn't working... so the error needs
    ; to be reported with the skin rolled back to the stock implementation.
    ;
    ; (Such recoveries are done once, and then a user must be able to
    ; successfully evaluate an expression to be willing to recover again.)

    let no-recover: false

    cycle [ trap [

        execute [system/console/print-gap]
        execute [system/console/print-prompt]

        let lines: copy []  ; lines of TEXT! accumulated by multi-line input
        let code: void  ; loaded code from lines of text

        === {LOOP FOR GATHERING MULTI-LINE INPUT} ===

        cycle [
            let error: void
            let line: void
            [line error]: execute [system/console/input-hook]

            ; Allow a Ctrl-C to terminate the INPUT-HOOK

            if error [
                all [
                    error/id = 'no-catch
                    :error/arg2 = :HALT  ; throw's /NAME
                ] then [
                    execute [system/console/print-halted]
                    break  ; end multi-line input, causes CONTINUE below
                ]

                fail error  ; have other INPUT-HOOK fails cause internal error
            ]

            append lines line

            trap [
                code: load/all delimit newline lines
                assert [block? code]  ; e.g. `load/all "word"` => `[word]`
            ]
            then error => [
                ;
                ; If loading gave back an error, check to see if it was the
                ; kind of error that comes from having partial input.  If so,
                ; CONTINUE and read more data until it's complete.
                ;
                if error/id = 'scan-missing [
                    ;
                    ; Error message tells you what's missing, not what's open
                    ; and needs to be closed.  Invert the symbol.
                    ;
                    switch error/arg1 [
                        "}" ["{"]
                        ")" ["("]
                        "]" ["["]
                    ]
                    also unclosed => [
                        ;
                        ; Backslash is used in the second column to help make
                        ; a pattern that isn't legal in Revolt code, which is
                        ; also uncommon in program output.
                        ;
                        ; !!! This could enable detection of transcripts,
                        ; potentially to replay them without running program
                        ; output or evaluation results.
                        ;
                        write-stdout unspaced [unclosed "\" space space]
                        continue  ; get the next line
                    ]
                ]

                ; Could be an unclosed double quote (unclosed tag?) which more
                ; input on a new line cannot legally close ATM.
                ;
                execute compose [system/console/print-error (error)]
                break
            ]

            stop <done>  ; code is ready
        ]
        else [
            continue  ; CYCLE was stopped with BREAK on unrecoverable error
        ]

        === {HANDLE CODE THAT HAS BEEN SUCCESSFULLY LOADED} ===

        let shortcut: select system/console/shortcuts try first code
        if shortcut [
            ;
            ; Shortcuts like `q => [quit]`, `d => [dump]`
            ;
            if (bound? code/1) and [not void? get* 'code/1] [
                ;
                ; Help confused user who might not know about the shortcut not
                ; panic by giving them a message.  Reduce noise for the casual
                ; shortcut by only doing so when a non-void variable exists.
                ;
                execute compose [system/console/print-warning (
                    spaced [
                        uppercase to text! code/1
                            "interpreted by console as:" mold :shortcut
                    ]
                )]
                execute compose [system/console/print-warning (
                    spaced ["use" to get-word! code/1 "to get variable."]
                )]
            ]
            take code
            insert code shortcut
        ]

        ; Run the "dialect hook", which can transform the completed code block
        ;
        code: execute compose [system/console/dialect-hook (code)]

        let error: void
        let result: void
        [result error]: execute/debuggable ensure block! code

        if error [
            assert [void? get* 'result]

            all [
                error/id = 'no-catch
                :error/arg2 = :HALT  ; throw's /NAME
            ] then [
                execute [system/console/print-halted]
                continue
            ]

            execute compose [system/console/print-error (error)]
        ]
        else [
            ; !!! Comment said "not all users may want CONST result, review
            ; configurability".
            ;
            execute compose [system/console/print-result '(get/any 'result)]
        ]

        no-recover: false  ; once success with read-eval-print, forgive fail

        comment </trap>
    ]
    then error => [

        if no-recover [
            print "** CONSOLE INTERNAL ERROR LOOP, PANICKING"
            panic-value error
        ]

        print "** CONSOLE (OR CONSOLE SKIN) RAISED INTERNAL ERROR"
        print mold error
        print "** REVERTING TO DEFAULT SKIN (...IT MIGHT HELP :-/)"

        system/console: make console! []

        no-recover: true  ; require sucessful console eval to recover again

    ] comment </cycle>]
]


why: function [
    "Explain the last error in more detail."
    return: <void>
    'err [<end> word! path! error!] "Optional error value"
][
    err: default [system/state/last-error]

    if match [word! path!] err [
        err: get err
    ]

    if error? err [
        err: lowercase unspaced [err/type #"-" err/id]
        browse join http://www.rebol.com/r3/docs/errors/ [err ".html"]
    ] else [
        print "No information is available."
    ]
]


upgrade: function [
    "Check for newer versions."
    return: <void>
][
    ; Should this be a console-detected command, like Q, or is it meaningful
    ; to define this as a function you could call from code?
    ;
    do <upgrade>
]


; !!! It should likely be the case that the namespace for the user natives in
; an extension would be shared with the Revolt code for a module, but there's
; also a likely need to be able to have several source-level Revolt files
; (and possibly several independent modules) in an extension.  This hasn't
; been completely hammered out yet.
;
; As a result, for the C code in %mod-console.c to be able to find the Revolt
; entry point for its mechanics, we export it to lib.  But this needs a much
; better solution.
;
append lib compose [
    console!: (ensure object! console!)
    console-impl: (:console-impl)
]

; !!! The whole host startup/console is currently very manually loaded
; into its own isolated context by the C startup code.  This way, changes
; to functions the console loop depends on (like PRINT or INPUT) that the
; user makes will not break the console's functionality.  It would be
; better if it used the module system, but since it doesn't, it does not
; have a place to put "exports" to lib or user.  We'd like people to be
; able to access the ABOUT, WHY, and USAGE functions... so export them
; here to LIB.  Again--this should be done by making this a module!
;
append lib compose [
    ;
    ; These must be <with>'d to be exported, otherwise the ABOUT: in
    ; the object key would be gathered as a local.
    ;
    why: (ensure action! :why)
    upgrade: (ensure action! :upgrade)
]
