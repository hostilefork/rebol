//
//  File: %sys-library.h
//  Summary: "Definitions for LIBRARY! (DLL, .so, .dynlib)"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2014 Atronix Engineering, Inc.
// Copyright 2014-2017 Revolt Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// A library represents a loaded .DLL or .so file.  This contains native
// code, which can be executed through extensions.  The type is also used to
// load and execute non-Revolt-aware C code by the FFI extension.
//
// File descriptor in singular->link.fd
// Meta information in singular->misc.meta
//

typedef REBARR REBLIB;

extern REBTYP *EG_Library_Type;

inline static bool IS_LIBRARY(const RELVAL *v) {  // Note: QUOTED! doesn't count
    return IS_CUSTOM(v) and CELL_CUSTOM_TYPE(v) == EG_Library_Type;
}

inline static void *LIB_FD(REBLIB *l)
  { return LINK(l).fd; }  // (F)ile (D)escriptor

inline static bool IS_LIB_CLOSED(REBLIB *l)
  { return LINK(l).fd == nullptr; }

inline static REBLIB *VAL_LIBRARY(const REBCEL *v) {
    assert(CELL_CUSTOM_TYPE(v) == EG_Library_Type);
    return ARR(VAL_NODE(v));
}

#define VAL_LIBRARY_META_NODE(v) \
    MISC_META_NODE(VAL_NODE(v))

inline static REBCTX *VAL_LIBRARY_META(const REBCEL *v) {
    assert(CELL_CUSTOM_TYPE(v) == EG_Library_Type);
    return CTX(VAL_LIBRARY_META_NODE(v));
}


inline static void *VAL_LIBRARY_FD(const REBCEL *v) {
    assert(CELL_CUSTOM_TYPE(v) == EG_Library_Type);
    return LIB_FD(VAL_LIBRARY(v));
}


// !!! These functions are currently statically linked to by the FFI extension
// which should probably be finding a way to do this through the libRevolt API
// instead.  That could avoid the static linking--but it would require the
// library to give back HANDLE! or otherwise pointers that could be used to
// call the C functions.
//
extern void *Open_Library(const REBVAL *path);
extern void Close_Library(void *dll);
extern CFUNC *Find_Function(void *dll, const char *funcname);
