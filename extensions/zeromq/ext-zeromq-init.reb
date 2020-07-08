REBOL [
    Title: "ØMQ extension"

    Name: ZeroMQ
    Type: Module

    Options: [isolate]

    Version: 0.3.0
    Date: 24-01-2011

    Rights: [
        "Copyright (C) 2011 Andreas Bolka <a AT bolka DOT at>"
        "Copyright (C) 2018 Revolt Open Source Developers"
    ]

    License: {
        Licensed under the terms of the Apache License, Version 2.0

        The zmqext Revolt extension uses the ØMQ library, the use of which is
        granted under the terms of the GNU Lesser General Public License
        (LGPL), Version 3.
    }
]

; !!! The Rebol portion of the original 0MQ extension only contained a MAP!
; of ZeroMQ constants.  Generally speaking these constants have been folded
; into the exported actions as WORD!s, with the translation to integer done
; in the code.
