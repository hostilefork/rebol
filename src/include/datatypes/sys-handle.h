//
//  File: %sys-handle.h
//  Summary: "Definitions for GC-able and non-GC-able Handles"
//  Project: "Revolt Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Revolt Open Source Contributors
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
// In Rebol terminology, a HANDLE! is a pointer to a function or data that
// represents an arbitrary external resource.  While such data could also
// be encoded as a BINARY! "blob" (as it might be in XML), the HANDLE! type
// is intentionally "opaque" to user code so that it is a black box.
//
// Additionally, Revolt added the idea of a garbage collector callback for
// "Managed" handles.  This is implemented by means of making the handle cost
// a single REBSER node shared among its instances, which is a "singular"
// Array containing a canon value of the handle itself.  When there are no
// references left to the handle and the GC runs, it will run a hook stored
// in the ->misc field of the singular array.
//
// As an added benefit of the Managed form, the code and data pointers in the
// value itself are not used; instead preferring the data held in the REBARR.
// This allows one instance of a managed handle to have its code or data
// pointer changed and be reflected in all instances.  The simple form of
// handle however is such that each REBVAL copied instance is independent,
// and changing one won't change the others.
//
////=// NOTES /////////////////////////////////////////////////////////////=//
//
// * The ->extra field of the REBVAL may contain a singular REBARR that is
//   leveraged for its GC-awareness.  This leverages the GC-aware ability of a
//   REBSER to know when no references to the handle exist and call a cleanup
//   function.  The GC-aware variant allocates a "singular" array, which is
//   the exact size of a REBSER and carries the canon data.  If the cheaper
//   kind that's just raw data and no callback, ->extra is null.
//

#define VAL_HANDLE_SINGULAR_NODE(v) \
    PAYLOAD(Any, (v)).first.node

#define VAL_HANDLE_SINGULAR(v) \
    ARR(PAYLOAD(Any, (v)).first.node)

#define VAL_HANDLE_LENGTH_U(v) \
    PAYLOAD(Any, (v)).second.u

#define VAL_HANDLE_CDATA_P(v) \
    EXTRA(Any, (v)).p

#define VAL_HANDLE_CFUNC_P(v) \
    EXTRA(Any, (v)).cfunc


inline static bool Is_Handle_Cfunc(const REBCEL *v) {
    assert(CELL_KIND(v) == REB_HANDLE);
    return VAL_HANDLE_LENGTH_U(v) == 0;
}

inline static uintptr_t VAL_HANDLE_LEN(const REBCEL *v) {
    assert(not Is_Handle_Cfunc(v));
    REBARR *a = VAL_HANDLE_SINGULAR(v);
    if (a)
        return VAL_HANDLE_LENGTH_U(ARR_SINGLE(a));
    else
        return VAL_HANDLE_LENGTH_U(v);
}

inline static void *VAL_HANDLE_VOID_POINTER(const REBCEL *v) {
    assert(not Is_Handle_Cfunc(v));
    REBARR *a = VAL_HANDLE_SINGULAR(v);
    if (a)
        return VAL_HANDLE_CDATA_P(ARR_SINGLE(a));
    else
        return VAL_HANDLE_CDATA_P(v);
}

#define VAL_HANDLE_POINTER(t, v) \
    cast(t *, VAL_HANDLE_VOID_POINTER(v))

inline static CFUNC *VAL_HANDLE_CFUNC(const REBCEL *v) {
    assert(Is_Handle_Cfunc(v));
    REBARR *a = VAL_HANDLE_SINGULAR(v);
    if (a)
        return VAL_HANDLE_CFUNC_P(ARR_SINGLE(a));
    else
        return VAL_HANDLE_CFUNC_P(v);
}

inline static CLEANUP_CFUNC *VAL_HANDLE_CLEANER(const REBCEL *v) {
    assert(CELL_KIND(v) == REB_HANDLE);
    REBARR *a = VAL_HANDLE_SINGULAR(v);
    if (not a)
        return nullptr;
    return MISC(a).cleaner;
}

inline static void SET_HANDLE_LEN(REBCEL *v, uintptr_t length) {
    assert(CELL_KIND(v) == REB_HANDLE);
    REBARR *a = VAL_HANDLE_SINGULAR(v);
    if (a)
        VAL_HANDLE_LENGTH_U(ARR_SINGLE(a)) = length;
    else
        VAL_HANDLE_LENGTH_U(v) = length;
}

inline static void SET_HANDLE_CDATA(REBCEL *v, void *cdata) {
    assert(CELL_KIND(v) == REB_HANDLE);
    REBARR *a = VAL_HANDLE_SINGULAR(v);
    if (a) {
        assert(VAL_HANDLE_LENGTH_U(ARR_SINGLE(a)) != 0);
        VAL_HANDLE_CDATA_P(ARR_SINGLE(a)) = cdata;
    }
    else {
        assert(VAL_HANDLE_LENGTH_U(v) != 0);
        VAL_HANDLE_CDATA_P(v) = cdata;
    }
}

inline static void SET_HANDLE_CFUNC(REBCEL *v, CFUNC *cfunc) {
    assert(Is_Handle_Cfunc(v));
    REBARR *a = VAL_HANDLE_SINGULAR(v);
    if (a) {
        assert(VAL_HANDLE_LENGTH_U(ARR_SINGLE(a)) == 0);
        VAL_HANDLE_CFUNC_P(ARR_SINGLE(a)) = cfunc;
    }
    else {
        assert(VAL_HANDLE_LENGTH_U(v) == 0);
        VAL_HANDLE_CFUNC_P(v) = cfunc;
    }
}

inline static REBVAL *Init_Handle_Cdata(
    RELVAL *out,
    void *cdata,
    uintptr_t length
){
    assert(length != 0);  // can't be 0 unless cfunc (see also malloc(0))
    RESET_CELL(out, REB_HANDLE, CELL_MASK_NONE);  // payload first is not node
    VAL_HANDLE_SINGULAR_NODE(out) = nullptr;
    VAL_HANDLE_CDATA_P(out) = cdata;
    VAL_HANDLE_LENGTH_U(out) = length;  // non-zero signals cdata
    return KNOWN(out);
}

inline static REBVAL *Init_Handle_Cfunc(
    RELVAL *out,
    CFUNC *cfunc
){
    RESET_CELL(out, REB_HANDLE, CELL_MASK_NONE);  // payload first is not node
    VAL_HANDLE_SINGULAR_NODE(out) = nullptr;
    VAL_HANDLE_CFUNC_P(out) = cfunc;
    VAL_HANDLE_LENGTH_U(out) = 0;  // signals cfunc
    return KNOWN(out);
}

inline static void Init_Handle_Cdata_Managed_Common(
    RELVAL *out,
    uintptr_t length,
    CLEANUP_CFUNC *cleaner
){
    REBARR *singular = Alloc_Singular(NODE_FLAG_MANAGED);
    MISC(singular).cleaner = cleaner;

    RELVAL *single = ARR_SINGLE(singular);
    RESET_VAL_HEADER(single, REB_HANDLE, CELL_FLAG_FIRST_IS_NODE);
    VAL_HANDLE_SINGULAR_NODE(single) = NOD(singular); 
    VAL_HANDLE_LENGTH_U(single) = length;
    // caller fills in VAL_HANDLE_CDATA_P or VAL_HANDLE_CFUNC_P

    // Don't fill the handle properties in the instance if it's the managed
    // form.  This way, you can set the properties in the canon value and
    // effectively update all instances...since the bits live in the shared
    // series component.
    //
    RESET_CELL(out, REB_HANDLE, CELL_FLAG_FIRST_IS_NODE);
    VAL_HANDLE_SINGULAR_NODE(out) = NOD(singular);
    VAL_HANDLE_LENGTH_U(out) = 0xDECAFBAD;  // trash to avoid compiler warning
    VAL_HANDLE_CDATA_P(out) = nullptr;  // or complains about not initializing
}

inline static REBVAL *Init_Handle_Cdata_Managed(
    RELVAL *out,
    void *cdata,
    uintptr_t length,
    CLEANUP_CFUNC *cleaner
){
    Init_Handle_Cdata_Managed_Common(out, length, cleaner);

    // Leave the non-singular cfunc as trash; clients should not be using

    REBARR *a = VAL_HANDLE_SINGULAR(out);
    VAL_HANDLE_CDATA_P(ARR_SINGLE(a)) = cdata;
    return KNOWN(out);
}

inline static REBVAL *Init_Handle_Cdata_Managed_Cfunc(
    RELVAL *out,
    CFUNC *cfunc,
    CLEANUP_CFUNC *cleaner
){
    Init_Handle_Cdata_Managed_Common(out, 0, cleaner);

    // Leave the non-singular cfunc as trash; clients should not be using
    
    REBARR *a = VAL_HANDLE_SINGULAR(out);
    VAL_HANDLE_CFUNC_P(ARR_SINGLE(a)) = cfunc;
    return KNOWN(out);
}
