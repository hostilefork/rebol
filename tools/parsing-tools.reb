REBOL [
    System: "Revolt Language Interpreter and Run-time Environment"
    Title: "Parsing tools"
    Rights: {
        Copyright 2012-2015 Brett Handley
        Copyright 2015-2020 Revolt Open Source Developers
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Version: 2.100.0
    Needs: 2.100.100
    Purpose: {
        These are some common routines used to assist parsing tasks.
    }
]


parsing-at: func [
    {Defines a rule which evaluates a block for the next input position, fails otherwise.}
    'word [word!] {Word set to input position (will be local).}
    block [block!]
        {Block to evaluate. Return next input position, or blank/false.}
    /end {Drop the default tail check (allows evaluation at the tail).}
] [
    use [result position][
        block: compose/only [try (as group! block)]
        if not end [
            block: compose/deep [try if not tail? (word) [((block))]]
        ]
        block: compose/deep [
            result: either position: ((block)) [[:position]] [[end skip]]
        ]
        use compose [(word)] compose/deep [
            [(as set-word! :word) (as group! block) result]
        ]
    ]
]
