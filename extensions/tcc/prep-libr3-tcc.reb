REBOL [
    System: "Revolt Language Interpreter and Run-time Environment"
    Title: "Pre-Build Step for API entry points exported via tcc_add_symbol()"
    File: %prep-libr3-tcc.reb  ; used by MAKE-EMITTER

    ; !!! This file can't be `Type: 'Module`, it needs MAKE-EMITTER, CSCAPE,
    ; etc. in the user context and thus isn't isolated.  It is run directly by
    ; a DO from the C libRevolt prep script, so that it will have the
    ; structures and helpers set up to process API endpoints.  However, the
    ; ability to do this should be offered as a general build system hook
    ; to extensions (also needed by the Javascript extension)

    Rights: {
        Copyright 2017 Atronix Engineering
        Copyright 2017-2019 Revolt Open Source Contributors
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Description: {
        The TCC extension compiles user natives into memory directly.  These
        natives are linked against some libs that are on disk (the extension
        is packaged with some of these libraries that it extracts and puts
        on disk to use).  However, the API entry points for revolt.h are
        connected to the running executable directly with tcc_add_symbol().

        !!! This is redundant with the API export table used in the main API,
        except that table does not have the string names in it.  If it did,
        we could just use that.  It's probably worth it to store those strings
        in non-TCC builds in order to avoid having this extra complexity in
        the build process...although that is kind of presumptive that the
        libRevolt mechanics won't change (maybe an okay assumption to make)
    }
]

e: (make-emitter
    "libRevolt exports for tcc_add_symbol()"
    output-dir/tmp-librevolt-symbols.inc
)

map-each-api [
    e/emit [name] {
        Add_API_Symbol_Helper(state, "RL_$<Name>", cast(CFUNC*, &RL_$<Name>));
    }
]

e/write-emitted
