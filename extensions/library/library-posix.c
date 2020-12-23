//
//  File: %library-posix.c
//  Summary: "POSIX Library-related functions"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Ren-C Open Source Contributors
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
// This is for support of the LIBRARY! type from the host on
// systems that support 'dlopen'.
//

#ifndef __cplusplus
    // See feature_test_macros(7)
    // This definition is redundant under C++
    #define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <poll.h>
#include <fcntl.h>  // Includes `O_XXX` constant definitions
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "sys-core.h"


#ifndef NO_DL_LIB
    #include <dlfcn.h>
#endif


//
//  Open_Library: C
//
// Load a DLL library and return the handle to it.
// If zero is returned, error indicates the reason.
//
void *Open_Library(const REBVAL *path)
{
  #ifdef NO_DL_LIB
    return nullptr;
  #else
    // Usually you want to fully resolve local paths before making OS calls.
    // But the dlopen() function searches library directories by default.
    // So if %foo is passed in, you don't want to prepend the current dir to
    // make it absolute, because it will *only* look there.
    //
    // So don't use FILE-TO-LOCAL/FULL...
    //
    char *path_utf8 = rebSpell("file-to-local", path, rebEND);

    void *dll = dlopen(path_utf8, RTLD_LAZY/*|RTLD_GLOBAL*/);

    rebFree(path_utf8);

    if (not dll) // dlerror() gives const char*
        rebJumps("fail", rebT(dlerror()), rebEND);

    return dll;
  #endif
}


//
//  Close_Library: C
//
// Free a DLL library opened earlier.
//
void Close_Library(void *dll)
{
  #ifndef NO_DL_LIB
    dlclose(dll);
  #endif
}


//
//  Find_Function: C
//
// Get a DLL function address from its string name.
//
CFUNC *Find_Function(void *dll, const char *funcname)
{
  #ifndef NO_DL_LIB
    // !!! See notes about data pointers vs. function pointers in the
    // definition of CFUNC.  This is trying to stay on the right side
    // of the specification, but OS APIs often are not standard C.  So
    // this implementation is not guaranteed to work, just to suppress
    // compiler warnings.  See:
    //
    //      http://stackoverflow.com/a/1096349/211160

    CFUNC *fp;
    *cast(void**, &fp) = dlsym(dll, funcname);
    return fp;
  #else
    return nullptr;
  #endif
}
