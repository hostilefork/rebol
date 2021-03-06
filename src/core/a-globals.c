//
//  File: %a-globals.c
//  Summary: "global variables"
//  Section: environment
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
//=////////////////////////////////////////////////////////////////////////=//
//
// There are two types of global variables:
//   process vars - single instance for main process
//   thread vars - duplicated within each R3 task
//

/* To do: there are still a few globals in various modules that need to be
** incorporated back into sys-globals.h.
*/

#include "sys-core.h"

#undef PVAR
#undef TVAR

#define PVAR
#define TVAR

#include "sys-globals.h"
