REBOL [
    System: "Revolt Language Interpreter and Run-time Environment"
    Title: "Generate native specifications"
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Author: "@codebybrett"
    Needs: 2.100.100
]

do %common.r
do %common-parsers.r
do %native-emitters.r ;for emit-native-proto

print "------ Generate tmp-natives.r"

src-dir: %../src
output-dir: system/options/path/prep
mkdir/deep output-dir/boot

verbose: false

unsorted-buffer: make text! 20000

process: function [
    file
    <with> the-file
][
    the-file: file
    if verbose [probe [file]]

    source.text: read/string join src-dir/core/% file
    proto-parser/emit-proto: :emit-native-proto
    proto-parser/process source.text
]

;-------------------------------------------------------------------------

output-buffer: make text! 20000


proto-count: 0

files: sort read src-dir/core/%

remove-each file files [

    not all [
        %.c = suffix? file
        not find/match file "host-"
        not find/match file "os-"
    ]
]

for-each file files [process file]

append output-buffer unsorted-buffer

write-if-changed output-dir/boot/tmp-natives.r output-buffer

print [proto-count "natives"]
print " "


print "------ Generate tmp-generics.r"

clear output-buffer

append output-buffer {REBOL [
    System: "Revolt Language Interpreter and Run-time Environment"
    Title: "Action function specs"
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0.
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Note: {This is a generated file.}
]

}

boot-types: load src-dir/boot/types.r

append output-buffer mold/only load src-dir/boot/generics.r

append output-buffer unspaced [
    newline
    "_  ; C code expects BLANK! evaluation result, at present" newline
    newline
]

write-if-changed output-dir/boot/tmp-generics.r output-buffer
