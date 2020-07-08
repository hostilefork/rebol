REBOL []

name: 'FFI

source: %ffi/mod-ffi.c
depends: [
    %ffi/t-struct.c
    %ffi/t-routine.c
]

comment [
    switch user-config/with-ffi [
        'static 'dynamic [
            for-each var [includes cflags searches ldflags][
                x: rebmake/pkg-config
                    try any [user-config/pkg-config {pkg-config}]
                    var
                    %libffi
                if not empty? x [
                    set (in cfg-ffi var) x
                ]
            ]

            libs: rebmake/pkg-config
                try any [user-config/pkg-config {pkg-config}]
                'libraries
                %libffi

            cfg-ffi/libraries: map-each lib libs [
                make rebmake/ext-dynamic-class [
                    output: lib
                    flags: either user-config/with-ffi = 'static [[static]][_]
                ]
            ]
        ]
        _ 'no 'off 'false #[false] [
            ;pass
        ]

        fail [
            "WITH-FFI should be one of [dynamic static no]"
            "not" (user-config/with-ffi)
        ]
    ]
]

includes: compose [
    %prep/extensions/ffi

    ; For the FFI to be used, you pretty much require the ability to load a
    ; DLL or shared library.  Currently the `Find_Function()` API is linked
    ; statically (as opposed to making a libRevolt call to the LIBRARY! and
    ; giving it a text string, getting back a HANDLE! from which a C function
    ; pointer can be extracted).  Hence headers must be directly included.
    ; But that could be changed.
    ;
    (repo-dir/extensions/library)

    ; Vectors are used to model C array structures, and thus for the moment
    ; one must build the vector extension into the executable if you want
    ; FFI support.  This could be decoupled as an option, but the base demos
    ; (like %qsort.r) depend upon it.  Similarly to the issue of static link
    ; of the LIBRARY! extension, it could be possible (if VECTOR! exported it)
    ; to do this via libRevolt, asking for a HANDLE! memory pointer for the
    ; vector...but for now we go through the internal includes of the type.
    ;
    (repo-dir/extensions/vector)
]

definitions: []

cflags: [
    ; ffi_closure has an alignment specifier, which causes
    ; padding, and MSVC warns about that.
    ;
    <msc:/wd4324>
]

searches: []

ldflags: []

libraries: [%ffi]
