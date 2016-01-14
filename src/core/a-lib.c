/***********************************************************************
**
**  REBOL [R3] Language Interpreter and Run-time Environment
**
**  Copyright 2012 REBOL Technologies
**  REBOL is a trademark of REBOL Technologies
**
**  Licensed under the Apache License, Version 2.0 (the "License");
**  you may not use this file except in compliance with the License.
**  You may obtain a copy of the License at
**
**  http://www.apache.org/licenses/LICENSE-2.0
**
**  Unless required by applicable law or agreed to in writing, software
**  distributed under the License is distributed on an "AS IS" BASIS,
**  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**  See the License for the specific language governing permissions and
**  limitations under the License.
**
************************************************************************
**
**  Module:  a-lib.c
**  Summary: exported REBOL library functions
**  Section: environment
**  Author:  Carl Sassenrath
**  Notes:
**
***********************************************************************/

#include "sys-core.h"
#include "reb-dialect.h"
#include "reb-ext.h"
#include "reb-evtypes.h"

// Linkage back to HOST functions. Needed when we compile as a DLL
// in order to use the OS_* macro functions.
#ifdef REB_API  // Included by C command line
REBOL_HOST_LIB *Host_Lib;
#endif

#include "reb-lib.h"

//#define DUMP_INIT_SCRIPT
#ifdef DUMP_INIT_SCRIPT
#include <fcntl.h>
#include <sys/stat.h>
#include <io.h>
// #include <stdio.h> // !!! No <stdio.h> in Ren-C release builds
#endif

extern const REBRXT Reb_To_RXT[REB_MAX];
extern RXIARG Value_To_RXI(const REBVAL *val); // f-extension.c
extern void RXI_To_Value(REBVAL *val, RXIARG arg, REBCNT type); // f-extension.c
extern void RXI_To_Block(RXIFRM *frm, REBVAL *out); // f-extension.c
extern int Do_Callback(REBARR *obj, u32 name, RXIARG *args, RXIARG *result);


//
//  RL_Version: C
// 
// Obtain current REBOL interpreter version information.
// 
// Returns:
//     A byte array containing version, revision, update, and more.
// Arguments:
//     vers - a byte array to hold the version info. First byte is length,
//         followed by version, revision, update, system, variation.
// Notes:
//     This function can be called before any other initialization
//     to determine version compatiblity with the caller.
//
RL_API void RL_Version(REBYTE vers[])
{
    // [0] is length
    vers[1] = REBOL_VER;
    vers[2] = REBOL_REV;
    vers[3] = REBOL_UPD;
    vers[4] = REBOL_SYS;
    vers[5] = REBOL_VAR;
}


//
//  RL_Init: C
// 
// Initialize the REBOL interpreter.
// 
// Returns:
//     Zero on success, otherwise an error indicating that the
//     host library is not compatible with this release.
// Arguments:
//     rargs - REBOL command line args and options structure.
//         See the host-args.c module for details.
//     lib - the host lib (OS_ functions) to be used by REBOL.
//         See host-lib.c for details.
// Notes:
//     This function will allocate and initialize all memory
//     structures used by the REBOL interpreter. This is an
//     extensive process that takes time.
//
RL_API int RL_Init(REBARGS *rargs, void *lib)
{
    int marker;
    REBUPT bounds;
    const char *env_legacy = NULL;

    Host_Lib = cast(REBOL_HOST_LIB *, lib);

    if (Host_Lib->size < HOST_LIB_SIZE) return 1;
    if (((HOST_LIB_VER << 16) + HOST_LIB_SUM) != Host_Lib->ver_sum) return 2;

    bounds = (REBUPT)OS_CONFIG(1, 0);
    if (bounds == 0) bounds = (REBUPT)STACK_BOUNDS;

#ifdef OS_STACK_GROWS_UP
    Stack_Limit = (REBUPT)(&marker) + bounds;
#else
    if (bounds > (REBUPT) &marker) Stack_Limit = 100;
    else Stack_Limit = (REBUPT)&marker - bounds;
#endif

    Init_Core(rargs);

    GC_Active = TRUE; // Turn on GC
    if (rargs->options & RO_TRACE) {
        Trace_Level = 9999;
        Trace_Flags = 1;
    }

    return 0;
}


//
//  RL_Start: C
// 
// Evaluate the default boot function.
// 
// Returns:
//     Zero on success, otherwise indicates an error occurred.
// Arguments:
//     bin - optional startup code (compressed), can be null
//     len - length of above bin
//     flags - special flags
// Notes:
//     This function completes the startup sequence by calling
//     the sys/start function.
//
RL_API int RL_Start(REBYTE *bin, REBINT len, REBYTE *script, REBINT script_len, REBCNT flags)
{
    REBVAL *val;
    REBSER *ser;

    struct Reb_State state;
    REBCON *error;
    int error_num;

    REBVAL result;
    VAL_INIT_WRITABLE_DEBUG(&result);

    if (bin) {
        ser = Decompress(bin, len, -1, FALSE, FALSE);
        if (!ser) return 1;

        val = CONTEXT_VAR(Sys_Context, SYS_CTX_BOOT_HOST);
        Val_Init_Binary(val, ser);
    }

    if (script && script_len > 4) {
        /* a 4-byte long payload type at the beginning */
        i32 ptype = 0;
        REBYTE *data = script + sizeof(ptype);
        script_len -= sizeof(ptype);

        memcpy(&ptype, script, sizeof(ptype));

        if (ptype == 1) {/* COMPRESSed data */
            ser = Decompress(data, script_len, -1, FALSE, FALSE);
        } else {
            ser = Make_Binary(script_len);
            if (ser == NULL) {
                OS_FREE(script);
                return 1;
            }
            memcpy(BIN_HEAD(ser), data, script_len);
        }
        OS_FREE(script);

        val = CONTEXT_VAR(Sys_Context, SYS_CTX_BOOT_EMBEDDED);
        Val_Init_Binary(val, ser);
    }

    PUSH_UNHALTABLE_TRAP(&error, &state);

// The first time through the following code 'error' will be NULL, but...
// `fail` can longjmp here, so 'error' won't be NULL *if* that happens!

    if (error) {
        //
        // !!! We are not allowed to ask for a print operation that can take
        // arbitrarily long without allowing for cancellation via Ctrl-C,
        // but here we are wanting to print an error.  If you're printing
        // out an error and get a halt, it won't print the halt.
        //
        REBCON *halt_error;

        // Save error for WHY?
        //
        REBVAL *last = Get_System(SYS_STATE, STATE_LAST_ERROR);
        Val_Init_Error(last, error);

        PUSH_UNHALTABLE_TRAP(&halt_error, &state);

// The first time through the following code 'error' will be NULL, but...
// `fail` can longjmp here, so 'error' won't be NULL *if* that happens!

        if (halt_error) {
            assert(ERR_NUM(halt_error) == RE_HALT);
            return ERR_NUM(halt_error);
        }

        Print_Value(last, 1024, FALSE);

        DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&state);

        // !!! When running in a script, whether or not the Rebol interpreter
        // just exits in an error case with a bad error code or breaks you
        // into the console to debug the environment should be controlled by
        // a command line option.  Defaulting to returning an error code
        // seems better, because kicking into an interactive session can
        // cause logging systems to hang.

        // For RE_HALT and all other errors we return the error
        // number.  Error numbers are not set in stone (currently), but
        // are never zero...which is why we can use 0 for success.
        //
        return ERR_NUM(error);
    }

    if (Apply_Func_Throws(
        &result, Sys_Func(SYS_CTX_FINISH_RL_START), END_VALUE
    )) {
        #if !defined(NDEBUG)
            if (LEGACY(OPTIONS_EXIT_FUNCTIONS_ONLY))
                fail (Error_No_Catch_For_Throw(&result));
        #endif

        if (
            IS_NATIVE(&result) && (
                VAL_FUNC_CODE(&result) == &N_quit
                || VAL_FUNC_CODE(&result) == &N_exit
            )
        ) {
            int status;

            CATCH_THROWN(&result, &result);
            status = Exit_Status_From_Value(&result);

            DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&state);

            Shutdown_Core();
            OS_EXIT(status);
            DEAD_END;
        }

        fail (Error_No_Catch_For_Throw(&result));
    }

    DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&state);

    // The convention in the API was to return 0 for success.  We use the
    // convention (as for FINISH_INIT_CORE) that any non-UNSET! result from
    // FINISH_RL_START indicates something went wrong.

    if (IS_UNSET(&result))
        error_num = 0; // no error
    else {
        assert(FALSE); // should not happen (raise an error instead)
        Debug_Fmt("** finish-rl-start returned non-NONE!:");
        Debug_Fmt("%r", &result);
        error_num = RE_MISC;
    }

    return error_num;
}


//
//  RL_Shutdown: C
// 
// Shut down a Rebol interpreter (that was initialized via RL_Init).
// 
// Returns:
//     nothing
// Arguments:
//     clean - whether you want Rebol to release all of its memory
//     accrued since initialization.  If you pass false, then it will
//     only do the minimum needed for data integrity (assuming you
//     are planning to exit the process, and hence the OS will
//     automatically reclaim all memory/handles/etc.)
//
RL_API void RL_Shutdown(REBOOL clean)
{
    // At time of writing, nothing Shutdown_Core() does pertains to
    // committing unfinished data to disk.  So really there is
    // nothing to do in the case of an "unclean" shutdown...yet.

#ifdef NDEBUG
    // Only do the work above this line in an unclean shutdown
    if (!clean) return;
#else
    // Run a clean shutdown anyway in debug builds--even if the
    // caller didn't need it--to see if it triggers any alerts.
#endif

    Shutdown_Core();
}


//
//  RL_Extend: C
// 
// Appends embedded extension to system/catalog/boot-exts.
// 
// Returns:
//     A pointer to the REBOL library (see reb-lib.h).
// Arguments:
//     source - A pointer to a UTF-8 (or ASCII) string that provides
//         extension module header, function definitions, and other
//         related functions and data.
//     call - A pointer to the extension's command dispatcher.
// Notes:
//     This function simply adds the embedded extension to the
//     boot-exts list. All other processing and initialization
//     happens later during startup. Each embedded extension is
//     queried and init using LOAD-EXTENSION system native.
//     See c:extensions-embedded
//
RL_API void *RL_Extend(const REBYTE *source, RXICAL call)
{
    REBVAL *value;
    REBARR *array;

    value = CONTEXT_VAR(Sys_Context, SYS_CTX_BOOT_EXTS);
    if (IS_BLOCK(value))
        array = VAL_ARRAY(value);
    else {
        array = Make_Array(2);
        Val_Init_Block(value, array);
    }
    value = Alloc_Tail_Array(array);
    Val_Init_Binary(value, Copy_Bytes(source, -1)); // UTF-8
    value = Alloc_Tail_Array(array);
    SET_HANDLE_CODE(value, cast(CFUNC*, call));

    return Extension_Lib();
}


//
//  RL_Escape: C
// 
// Signal that code evaluation needs to be interrupted.
// 
// Returns:
//     nothing
// Arguments:
//     reserved - must be set to zero.
// Notes:
//     This function set's a signal that is checked during evaluation
//     and will cause the interpreter to begin processing an escape
//     trap. Note that control must be passed back to REBOL for the
//     signal to be recognized and handled.
//
RL_API void RL_Escape(REBINT reserved)
{
    // How should HALT vs. BREAKPOINT be decided?  When does a Ctrl-C want
    // to quit entirely vs. begin an interactive debugging session?
    //
    // !!! For now default to halting, but use SIG_INTERRUPT when a decision
    // is made about how to debug break.
    //
    SET_SIGNAL(SIG_HALT);
}


//
//  RL_Do_String: C
// 
// Load a string and evaluate the resulting block.
// 
// Returns:
//     The datatype of the result if a positive number (or 0 if the
//     type has no representation in the "RXT" API).  An error code
//     if it's a negative number.  Two negative numbers are reserved
//     for non-error conditions: -1 for halting (e.g. Escape), and
//     -2 is reserved for exiting with exit_status set.
// 
// Arguments:
//     text - A null terminated UTF-8 (or ASCII) string to transcode
//         into a block and evaluate.
//     flags - set to zero for now
//     result - value returned from evaluation, if NULL then result
//         will be returned on the top of the stack
// 
// Notes:
//     This API was from before Rebol's open sourcing and had little
//     vetting and few clients.  The one client it did have was the
//     "sample" console code (which wound up being the "only"
//     console code for quite some time).
//
RL_API int RL_Do_String(
    int *exit_status,
    const REBYTE *text,
    REBCNT flags,
    RXIARG *out
) {
    REBARR *code;

    struct Reb_State state;
    REBCON *error;

    REBVAL result;
    VAL_INIT_WRITABLE_DEBUG(&result);

    // assumes it can only be run at the topmost level where
    // the data stack is completely empty.
    assert(DSP == -1);

    PUSH_UNHALTABLE_TRAP(&error, &state);

// The first time through the following code 'error' will be NULL, but...
// `fail` can longjmp here, so 'error' won't be NULL *if* that happens!

    if (error) {
        // Save error for WHY?
        REBVAL *last = Get_System(SYS_STATE, STATE_LAST_ERROR);
        Val_Init_Error(last, error);

        if (ERR_NUM(error) == RE_HALT)
            return -1; // !!! Revisit hardcoded #

        if (out)
            *out = Value_To_RXI(last);
        else
            DS_PUSH(last);

        return -ERR_NUM(error);
    }

    code = Scan_Source(text, LEN_BYTES(text));
    PUSH_GUARD_ARRAY(code);

    // Bind into lib or user spaces?
    if (flags) {
        // Top words will be added to lib:
        Bind_Values_Set_Forward_Shallow(ARRAY_HEAD(code), Lib_Context);
        Bind_Values_Deep(ARRAY_HEAD(code), Lib_Context);
    }
    else {
        REBCON *user = VAL_CONTEXT(Get_System(SYS_CONTEXTS, CTX_USER));

        REBVAL vali;
        VAL_INIT_WRITABLE_DEBUG(&vali);
        SET_INTEGER(&vali, CONTEXT_LEN(user) + 1);

        Bind_Values_All_Deep(ARRAY_HEAD(code), user);
        Resolve_Context(user, Lib_Context, &vali, FALSE, FALSE);
    }

    if (Do_At_Throws(&result, code, 0)) {
        DROP_GUARD_ARRAY(code);

        if (
            IS_NATIVE(&result) && (
                VAL_FUNC_CODE(&result) == &N_quit
                || VAL_FUNC_CODE(&result) == &N_exit
            )
        ) {
            CATCH_THROWN(&result, &result);
            DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&state);

            *exit_status = Exit_Status_From_Value(&result);
            return -2; // Revisit hardcoded #
        }

        fail (Error_No_Catch_For_Throw(&result));
    }

    DROP_GUARD_ARRAY(code);

    DROP_TRAP_SAME_STACKLEVEL_AS_PUSH(&state);

    if (out)
        *out = Value_To_RXI(&result);
    else
        DS_PUSH(&result);

    return Reb_To_RXT[VAL_TYPE(&result)];
}


//
//  RL_Do_Binary: C
// 
// Evaluate an encoded binary script such as compressed text.
// 
// Returns:
//     The datatype of the result or zero if error in the encoding.
// Arguments:
//     bin - by default, a REBOL compressed UTF-8 (or ASCII) script.
//     length - the length of the data.
//     flags - special flags (set to zero at this time).
//     key - encoding, encryption, or signature key.
//     result - value returned from evaluation.
// Notes:
//     As of A104, only compressed scripts are supported, however,
//     rebin, cloaked, signed, and encrypted formats will be supported.
//
RL_API int RL_Do_Binary(
    int *exit_status,
    const REBYTE *bin,
    REBINT length,
    REBCNT flags,
    REBCNT key,
    RXIARG *out
) {
    REBSER *text;
#ifdef DUMP_INIT_SCRIPT
    int f;
#endif
    int maybe_rxt; // could be REBRXT, or negative number for error :-/

    text = Decompress(bin, length, -1, FALSE, FALSE);
    if (!text) return 0;
    Append_Codepoint_Raw(text, 0);

#ifdef DUMP_INIT_SCRIPT
    f = _open("host-boot.r", _O_CREAT | _O_RDWR, _S_IREAD | _S_IWRITE );
    _write(f, BIN_HEAD(text), LEN_BYTES(BIN_HEAD(text)));
    _close(f);
#endif

    PUSH_GUARD_SERIES(text);
    maybe_rxt = RL_Do_String(exit_status, BIN_HEAD(text), flags, out);
    DROP_GUARD_SERIES(text);

    Free_Series(text);
    return maybe_rxt;
}


//
//  RL_Do_Commands: C
// 
// Evaluate a block of extension commands at high speed.
// 
// Returns:
//     Nothing
// Arguments:
//     array - a pointer to the REBVAL array series
//     flags - set to zero for now
//     context - command evaluation context struct or zero if not used.
// Notes:
//     For command blocks only, not for other blocks.
//     The context allows passing to each command a struct that is
//     used for back-referencing your environment data or for tracking
//     the evaluation block and its index.
//
RL_API void RL_Do_Commands(REBARR *array, REBCNT flags, REBCEC *context)
{
    REBVAL result;
    VAL_INIT_WRITABLE_DEBUG(&result); // !!! necessary?  presumably...
    Do_Commands(&result, array, context);

    // !!! Ignored result?  Throws?  etc.  But it's old  RL_Api, so...not
    // really a core concern going forward in Ren-C.
}


//
//  RL_Print: C
// 
// Low level print of formatted data to the console.
// 
// Returns:
//     nothing
// Arguments:
//     fmt - A format string similar but not identical to printf.
//         Special options are available.
//     ... - Values to be formatted.
// Notes:
//     This function is low level and handles only a few C datatypes
//     at this time.
//
RL_API void RL_Print(const REBYTE *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Debug_Buf(cs_cast(fmt), &args);
    va_end(args);
}


//
//  RL_Event: C
// 
// Appends an application event (e.g. GUI) to the event port.
// 
// Returns:
//     Returns TRUE if queued, or FALSE if event queue is full.
// Arguments:
//     evt - A properly initialized event structure. The
//         contents of this structure are copied as part of
//         the function, allowing use of locals.
// Notes:
//     Sets a signal to get REBOL attention for WAIT and awake.
//     To avoid environment problems, this function only appends
//     to the event queue (no auto-expand). So if the queue is full
//
RL_API int RL_Event(REBEVT *evt)
{
    REBVAL *event = Append_Event();     // sets signal

    if (event) {                        // null if no room left in series
        VAL_RESET_HEADER(event, REB_EVENT); // has more space, if needed
        event->payload.event = *evt;

        return 1;
    }

    return 0;
}


//
//  RL_Update_Event: C
// 
// Updates an application event (e.g. GUI) to the event port.
// 
// Returns:
//     Returns 1 if updated, or 0 if event appended, and -1 if full.
// Arguments:
//     evt - A properly initialized event structure. The
//          model and type of the event are used to address
//          the unhandled event in the queue, when it is found,
//          it will be replaced with this one
//
RL_API int RL_Update_Event(REBEVT *evt)
{
    REBVAL *event = Find_Last_Event(evt->model, evt->type);

    if (event) {
        event->payload.event = *evt;
        return 1;
    }

    return RL_Event(evt) - 1;
}


//
//  RL_Find_Event: C
// 
// Find an application event (e.g. GUI) to the event port.
//
// Returns:
//     A pointer to the find event
// Arguments:
//     model - event model
//     type - event type
//
RL_API REBEVT *RL_Find_Event (REBINT model, REBINT type)
{
    REBVAL * val = Find_Last_Event(model, type);
    if (val != NULL) {
        return &val->payload.event;
    }
    return NULL;
}


//
//  RL_Make_Block: C
// 
// Allocate a series suitable for storing Rebol values.  This series
// can be used as a backing store for a BLOCK!, but also for any
// other Rebol Array type (GROUP!, PATH!, GET-PATH!, SET-PATH!, or
// LIT-PATH!).
// 
// Returns:
//     A pointer to a block series.
// Arguments:
//     size - the length of the block. The system will add one extra
//         for the end-of-block marker.
// Notes:
//     Blocks are allocated with REBOL's internal memory manager.
//     Internal structures may change, so NO assumptions should be made!
//     Blocks are automatically garbage collected if there are
//     no references to them from REBOL code (C code does nothing.)
//     However, you can lock blocks to prevent deallocation. (?? default)
//
RL_API void *RL_Make_Block(u32 size)
{
    REBARR * array = Make_Array(size);
    MANAGE_ARRAY(array);
    return array;
}


//
//  RL_Make_String: C
// 
// Allocate a new string or binary series.
// 
// Returns:
//     A pointer to a string or binary series.
// Arguments:
//     size - the length of the string. The system will add one extra
//         for a null terminator (not strictly required, but good for C.)
//     unicode - set FALSE for ASCII/Latin1 strings, set TRUE for Unicode.
// Notes:
//     Strings can be REBYTE or REBCHR sized (depends on R3 config.)
//     Strings are allocated with REBOL's internal memory manager.
//     Internal structures may change, so NO assumptions should be made!
//     Strings are automatically garbage collected if there are
//     no references to them from REBOL code (C code does nothing.)
//     However, you can lock strings to prevent deallocation. (?? default)
//
RL_API void *RL_Make_String(u32 size, REBOOL unicode)
{
    REBSER *result = unicode ? Make_Unicode(size) : Make_Binary(size);

    // !!! Assume client does not have Free_Series() or MANAGE_SERIES()
    // APIs, so the series we give back must be managed.  But how can
    // we be sure they get what usage they needed before the GC happens?
    MANAGE_SERIES(result);
    return result;
}


//
//  RL_Make_Image: C
// 
// Allocate a new image of the given size.
// 
// Returns:
//     A pointer to an image series, or zero if size is too large.
// Arguments:
//     width - the width of the image in pixels
//     height - the height of the image in lines
// Notes:
//     Images are allocated with REBOL's internal memory manager.
//     Image are automatically garbage collected if there are
//     no references to them from REBOL code (C code does nothing.)
//
RL_API void *RL_Make_Image(u32 width, u32 height)
{
    REBSER *ser = Make_Image(width, height, FALSE);
    MANAGE_SERIES(ser);
    return ser;
}


//
//  RL_Protect_GC: C
// 
// Protect memory from garbage collection.
// 
// Returns:
//     nothing
// Arguments:
//     series - a series to protect (block, string, image, ...)
//     flags - set to 1 to protect, 0 to unprotect
// Notes:
//     You should only use this function when absolutely necessary,
//     because it bypasses garbage collection for the specified series.
//     Meaning: if you protect a series, it will never be freed.
//     Also, you only need this function if you allocate several series
//     such as strings, blocks, images, etc. within the same command
//     and you don't store those references somewhere where the GC can
//     find them, such as in an existing block or object (variable).
//
RL_API void RL_Protect_GC(REBSER *series, u32 flags)
{
    // !!! With series flags in short supply, this undesirable routine
    // was removed along with SER_KEEP.  (Note that it is not possible
    // to simply flip off the OPT_SER_MANAGED bit, because there is more
    // involved in tracking the managed state than just that bit.)
    //
    // For the purpose intended by this routine, use the GC_Mark_Hook (or
    // its hopeful improved successors.)

    panic (Error(RE_MISC));
}


//
//  RL_Get_String: C
// 
// Obtain a pointer into a string (bytes or unicode).
//
// Returns:
//     The length and type of string. When len > 0, string is unicode.
//     When len < 0, string is bytes.
// Arguments:
//     series - string series pointer
//     index - index from beginning (zero-based)
//     str   - pointer to first character
// Notes:
//     If the len is less than zero, then the string is optimized to
//     codepoints (chars) 255 or less for ASCII and LATIN-1 charsets.
//     Strings are allowed to move in memory. Therefore, you will want
//     to make a copy of the string if needed.
//
RL_API int RL_Get_String(REBSER *series, u32 index, void **str)
{   // ret: len or -len
    int len;

    if (index >= SERIES_LEN(series))
        len = 0;
    else
        len = SERIES_LEN(series) - index;

    if (BYTE_SIZE(series)) {
        *str = BIN_AT(series, index);
        len = -len;
    }
    else {
        *str = UNI_AT(series, index);
    }

    return len;
}


//
//  RL_Map_Word: C
// 
// Given a word as a string, return its global word identifier.
// 
// Returns:
//     The word identifier that matches the string.
// Arguments:
//     string - a valid word as a UTF-8 encoded string.
// Notes:
//     Word identifiers are persistent, and you can use them anytime.
//     If the word is new (not found in master symbol table)
//     it will be added and the new word identifier is returned.
//
RL_API u32 RL_Map_Word(REBYTE *string)
{
    return Make_Word(string, LEN_BYTES(string));
}


//
//  RL_Map_Words: C
// 
// Given a block of word values, return an array of word ids.
// 
// Returns:
//     An array of global word identifiers (integers). The [0] value is the size.
// Arguments:
//     series - block of words as values (from REBOL blocks, not strings.)
// Notes:
//     Word identifiers are persistent, and you can use them anytime.
//     The block can include any kind of word, including set-words, lit-words, etc.
//     If the input block contains non-words, they will be skipped.
//     The array is allocated with OS_ALLOC and you can OS_FREE it any time.
//
RL_API u32 *RL_Map_Words(REBARR *array)
{
    REBCNT i = 1;
    u32 *words;
    REBVAL *val = ARRAY_HEAD(array);

    words = OS_ALLOC_N(u32, ARRAY_LEN(array) + 2);

    for (; NOT_END(val); val++) {
        if (ANY_WORD(val)) words[i++] = VAL_WORD_CANON(val);
    }

    words[0] = i;
    words[i] = 0;

    return words;
}


//
//  RL_Word_String: C
// 
// Return a string related to a given global word identifier.
// 
// Returns:
//     A copy of the word string, null terminated.
// Arguments:
//     word - a global word identifier
// Notes:
//     The result is a null terminated copy of the name for your own use.
//     The string is always UTF-8 encoded (chars > 127 are encoded.)
//     In this API, word identifiers are always canonical. Therefore,
//     the returned string may have different spelling/casing than expected.
//     The string is allocated with OS_ALLOC and you can OS_FREE it any time.
//
RL_API REBYTE *RL_Word_String(u32 word)
{
    REBYTE *s1, *s2;
    // !!This code should use a function from c-words.c (but nothing perfect yet.)
    if (word == 0 || word >= ARRAY_LEN(PG_Word_Table.array)) return 0;
    s1 = VAL_SYM_NAME(ARRAY_AT(PG_Word_Table.array, word));
    s2 = OS_ALLOC_N(REBYTE, LEN_BYTES(s1) + 1);
    COPY_BYTES(s2, s1, LEN_BYTES(s1) + 1);
    return s2;
}


//
//  RL_Find_Word: C
// 
// Given an array of word ids, return the index of the given word.
// 
// Returns:
//     The index of the given word or zero.
// Arguments:
//     words - a word array like that returned from MAP_WORDS (first element is size)
//     word - a word id
// Notes:
//     The first element of the word array is the length of the array.
//
RL_API u32 RL_Find_Word(u32 *words, u32 word)
{
    REBCNT n = 0;

    if (words == 0) return 0;

    for (n = 1; n < words[0]; n++) {
        if (words[n] == word) return n;
    }
    return 0;
}


//
//  RL_Series: C
// 
// Get series information.
// 
// Returns:
//     Returns information related to a series.
// Arguments:
//     series - any series pointer (string or block)
//     what - indicates what information to return (see RXI_SER enum)
// Notes:
//     Invalid what arg nums will return zero.
//
RL_API REBUPT RL_Series(REBSER *series, REBCNT what)
{
    switch (what) {
    case RXI_SER_DATA: return cast(REBUPT, SERIES_DATA_RAW(series));
    case RXI_SER_TAIL: return SERIES_LEN(series);
    case RXI_SER_LEFT: return SERIES_AVAIL(series);
    case RXI_SER_SIZE: return SERIES_REST(series);
    case RXI_SER_WIDE: return SERIES_WIDE(series);
    }
    return 0;
}


//
//  RL_Get_Char: C
// 
// Get a character from byte or unicode string.
// 
// Returns:
//     A Unicode character point from string. If index is
//     at or past the tail, a -1 is returned.
// Arguments:
//     series - string series pointer
//     index - zero based index of character
// Notes:
//     This function works for byte and unicoded strings.
//     The maximum size of a Unicode char is determined by
//     R3 build options. The default is 16 bits.
//
RL_API int RL_Get_Char(REBSER *series, u32 index)
{
    if (index >= SERIES_LEN(series)) return -1;
    return GET_ANY_CHAR(series, index);
}


//
//  RL_Set_Char: C
// 
// Set a character into a byte or unicode string.
// 
// Returns:
//     The index passed as an argument.
// Arguments:
//     series - string series pointer
//     index - where to store the character. If past the tail,
//         the string will be auto-expanded by one and the char
//         will be appended.
//
RL_API u32 RL_Set_Char(REBSER *series, u32 index, u32 chr)
{
    if (index >= SERIES_LEN(series)) {
        index = SERIES_LEN(series);
        EXPAND_SERIES_TAIL(series, 1);
    }
    SET_ANY_CHAR(series, index, chr);
    return index;
}


//
//  RL_Get_Value: C
// 
// Get a value from a block.
// 
// Returns:
//     Datatype of value or zero if index is past tail.
// Arguments:
//     series - block series pointer
//     index - index of the value in the block (zero based)
//     result - set to the value of the field
//
RL_API int RL_Get_Value(REBARR *array, u32 index, RXIARG *result)
{
    REBVAL *value;
    if (index >= ARRAY_LEN(array)) return 0;
    value = ARRAY_AT(array, index);
    *result = Value_To_RXI(value);
    return Reb_To_RXT[VAL_TYPE(value)];
}


//
//  RL_Set_Value: C
// 
// Set a value in a block.
// 
// Returns:
//     TRUE if index past end and value was appended to tail of block.
// Arguments:
//     series - block series pointer
//     index - index of the value in the block (zero based)
//     val  - new value for field
//     type - datatype of value
//
RL_API REBOOL RL_Set_Value(REBARR *array, u32 index, RXIARG val, int type)
{
    REBVAL value;
    VAL_INIT_WRITABLE_DEBUG(&value);
    RXI_To_Value(&value, val, type);

    if (index >= ARRAY_LEN(array)) {
        Append_Value(array, &value);
        return TRUE;
    }

    *ARRAY_AT(array, index) = value;

    return FALSE;
}


//
//  RL_Words_Of_Object: C
// 
// Returns information about the object.
// 
// Returns:
//     Returns an array of words used as fields of the object.
// Arguments:
//     obj  - object pointer (e.g. from RXA_OBJECT)
// Notes:
//     Returns a word array similar to MAP_WORDS().
//     The array is allocated with OS_ALLOC. You can OS_FREE it any time.
//
RL_API u32 *RL_Words_Of_Object(REBSER *obj)
{
    REBCNT index;
    u32 *syms;
    REBVAL *key;
    REBCON *context = AS_CONTEXT(obj);

    key = CONTEXT_KEYS_HEAD(context);

    // We don't include hidden keys (e.g. SELF), but terminate by 0.
    // Conservative estimate that there are no hidden keys, add one.
    //
    syms = OS_ALLOC_N(u32, CONTEXT_LEN(context) + 1);

    index = 0;
    for (; NOT_END(key); key++) {
        if (VAL_GET_EXT(key, EXT_TYPESET_HIDDEN))
            continue;

        syms[index] = VAL_TYPESET_CANON(key);
        index++;
    }

    syms[index] = SYM_0; // Null terminate

    return syms;
}


//
//  RL_Get_Field: C
// 
// Get a field value (context variable) of an object.
// 
// Returns:
//     Datatype of value or zero if word is not found in the object.
// Arguments:
//     obj  - object pointer (e.g. from RXA_OBJECT)
//     word - global word identifier (integer)
//     result - gets set to the value of the field
//
RL_API int RL_Get_Field(REBSER *obj, u32 word, RXIARG *result)
{
    REBCON *context = AS_CONTEXT(obj);
    REBVAL *value;
    if (!(word = Find_Word_In_Context(context, word, FALSE))) return 0;
    value = CONTEXT_VAR(context, word);
    *result = Value_To_RXI(value);
    return Reb_To_RXT[VAL_TYPE(value)];
}


//
//  RL_Set_Field: C
// 
// Set a field (context variable) of an object.
// 
// Returns:
//     The type arg, or zero if word not found in object or if field is protected.
// Arguments:
//     obj  - object pointer (e.g. from RXA_OBJECT)
//     word_id - global word identifier (integer)
//     val  - new value for field
//     type - datatype of value
//
RL_API int RL_Set_Field(REBSER *obj, u32 word_id, RXIARG val, int type)
{
    REBCON *context = AS_CONTEXT(obj);

    word_id = Find_Word_In_Context(context, word_id, FALSE);
    if (word_id == 0) return 0;

    if (VAL_GET_EXT(CONTEXT_KEY(context, word_id), EXT_TYPESET_LOCKED))
        return 0;

    RXI_To_Value(CONTEXT_VAR(context, word_id), val, type);

    return type;
}


//
//  RL_Callback: C
// 
// Evaluate a REBOL callback function, either synchronous or asynchronous.
// 
// Returns:
//     Sync callback: type of the result; async callback: true if queued
// Arguments:
//     cbi - callback information including special option flags,
//         object pointer (where function is located), function name
//         as global word identifier (within above object), argument list
//         passed to callback (see notes below), and result value.
// Notes:
//     The flag value will determine the type of callback. It can be either
//     synchronous, where the code will re-enter the interpreter environment
//     and call the specified function, or asynchronous where an EVT_CALLBACK
//     event is queued, and the callback will be evaluated later when events
//     are processed within the interpreter's environment.
//     For asynchronous callbacks, the cbi and the args array must be managed
//     because the data isn't processed until the callback event is
//     handled. Therefore, these cannot be allocated locally on
//     the C stack; they should be dynamic (or global if so desired.)
//     See c:extensions-callbacks
//
RL_API int RL_Callback(RXICBI *cbi)
{
    REBEVT evt;

    // Synchronous callback?
    if (!GET_FLAG(cbi->flags, RXC_ASYNC)) {
        return Do_Callback(cbi->obj, cbi->word, cbi->args, &(cbi->result));
    }

    CLEARS(&evt);
    evt.type = EVT_CALLBACK;
    evt.model = EVM_CALLBACK;
    evt.eventee.ser = cbi;
    SET_FLAG(cbi->flags, RXC_QUEUED);

    return RL_Event(&evt);  // (returns 0 if queue is full, ignored)
}


//
//  RL_Length_As_UTF8: C
// 
// Calculate the UTF8 length of an array of unicode codepoints
// 
// Returns:
// How long the UTF8 encoded string would be
// 
// Arguments:
// p - pointer to array of bytes or wide characters
// len - length of src in codepoints (not including terminator)
// unicode - true if src is in wide character format
// lf_to_crlf - convert linefeeds into carraige-return + linefeed
// 
// !!! Host code is not supposed to call any Rebol routines except
// for those in the RL_Api.  This exposes Rebol's internal UTF8
// length routine, as it was being used by host code.  It should
// be reviewed along with the rest of the RL_Api.
//
RL_API REBCNT RL_Length_As_UTF8(
    const void *p,
    REBCNT len,
    REBOOL unicode,
    REBOOL lf_to_crlf
) {
    return Length_As_UTF8(
        p,
        len,
        (unicode ? OPT_ENC_UNISRC : 0) | (lf_to_crlf ? OPT_ENC_CRLF : 0)
    );
}


//
//  RL_Encode_UTF8: C
// 
// Encode the unicode into UTF8 byte string.
// 
// Returns:
// Number of dst bytes used.
// 
// Arguments:
// dst - destination for encoded UTF8 bytes
// max - maximum size of the result in bytes
// src - source array of bytes or wide characters
// len - input is source length, updated to reflect src chars used
// unicode - true if src is in wide character format
// crlf_to_lf - convert carriage-return + linefeed into just linefeed
// 
// Notes:
// Does not add a terminator.
// 
// !!! Host code is not supposed to call any Rebol routines except
// for those in the RL_Api.  This exposes Rebol's internal UTF8
// length routine, as it was being used by the Linux host code by
// Atronix.  Should be reviewed along with the rest of the RL_Api.
//
RL_API REBCNT RL_Encode_UTF8(
    REBYTE *dst,
    REBINT max,
    const void *src,
    REBCNT *len,
    REBOOL unicode,
    REBOOL crlf_to_lf
) {
    return Encode_UTF8(
        dst,
        max,
        src,
        len,
        (unicode ? OPT_ENC_UNISRC : 0) | (crlf_to_lf ? OPT_ENC_CRLF : 0)
    );
}


#include "reb-lib-lib.h"

//
//  Extension_Lib: C
//
void *Extension_Lib(void)
{
    return &Ext_Lib;
}
