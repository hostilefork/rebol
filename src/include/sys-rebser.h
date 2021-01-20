//
//  File: %sys-rebser.h
//  Summary: {any-series! defs BEFORE %tmp-internals.h (see: %sys-series.h)}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012-2021 Ren-C Open Source Contributors
// Copyright 2012 REBOL Technologies
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// `struct Reb_Series` (or "REBSER") is a small-ish fixed-size descriptor for
// series data.  Usually it contains a pointer to a larger allocation for the
// actual contents.  But if the series is small enough, the contents are
// embedded into the REBSER structure itself.
//
// Every string, block, path, etc. in Rebol has a REBSER.  Since Rebol does
// not depend on any data structure libraries--like C++'s std::vector--this
// means that the REBSER is also used internally when there is a need for a
// dynamically growable contiguous memory structure.
//
// REBSER behaves something like a "double-ended queue".  It can reserve
// capacity at both the tail and the head.  When data is taken from the head,
// it will retain that capacity...reusing it on later insertions at the head.
//
// The space at the head is called the "bias", and to save on pointer math
// per-access, the stored data pointer is actually adjusted to include the
// bias.  This biasing is backed out upon insertions at the head, and also
// must be subtracted completely to free the pointer using the address
// originally given by the allocator.
//
// The REBSER is fixed-size, and is allocated as a "node" from a memory pool.
// That pool quickly grants and releases memory ranges that are sizeof(REBSER)
// without needing to use malloc() and free() for each individual allocation.
// These nodes can also be enumerated in the pool without needing the series
// to be tracked via a linked list or other structure.  The garbage collector
// is one example of code that performs such an enumeration.
//
// A REBSER node pointer will remain valid as long as outstanding references
// to the series exist in values visible to the GC.  On the other hand, the
// series's data pointer may be freed and reallocated to respond to the needs
// of resizing.  (In the future, it may be reallocated just as an idle task
// by the GC to reclaim or optimize space.)  Hence pointers into data in a
// managed series *must not be held onto across evaluations*, without
// special protection or accomodation.
//
// REBSERs may be either manually memory managed or delegated to the garbage
// collector.  Free_Unmanaged_Series() may only be called on manual series.
// See Manage_Series()/PUSH_GC_GUARD() for remarks on how to work safely
// with pointers to garbage-collected series, to avoid having them be GC'd
// out from under the code while working with them.
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * For the forward declarations of series subclasses, see %reb-defs.h
//
// * Because a series contains a union member that embeds a REBVAL directly,
//   `struct Reb_Value` must be fully defined before this file can compile.
//   Hence %sys-rebval.h must already be included.
//
// * For the API of operations available on REBSER types, see %sys-series.h
//
// * REBARR is a series that contains Rebol values (REBVALs).  It has many
//   concerns specific to special treatment and handling, in interaction with
//   the garbage collector as well as handling "relative vs specific" values.
//
// * Several related types (REBACT for function, REBCTX for context) are
//   actually stylized arrays.  They are laid out with special values in their
//   content (e.g. at the [0] index), or by links to other series in their
//   `->misc` field of the REBSER node.  Hence series are the basic building
//   blocks of nearly all variable-size structures in the system.
//
// * The element size in a REBSER is known as the "width".  It is designed
//   to support widths of elements up to 255 bytes.


// While series are nodes, the token-pasting based GET_SERIES_FLAG() macros
// and their ilk look for flags of the form SERIES_FLAG_##name.  So alias the
// node flags as series flags.

#define SERIES_FLAG_MANAGED NODE_FLAG_MANAGED
#define SERIES_FLAG_ROOT NODE_FLAG_ROOT
#define SERIES_FLAG_MARKED NODE_FLAG_MARKED

//=//// SERIES_FLAG_LINK_NODE_NEEDS_MARK //////////////////////////////////=//
//
// This indicates that a series's LINK() field is the `custom` node element,
// and should be marked (if not null).
//
// Note: Even if this flag is not set, *link.any might still be a node*...
// just not one that should be marked.
//
#define SERIES_FLAG_LINK_NODE_NEEDS_MARK \
    NODE_FLAG_GC_ONE


//=//// SERIES_FLAG_MISC_NODE_NEEDS_MARK //////////////////////////////////=//
//
// This indicates that a series's MISC() field is the `custom` node element,
// and should be marked (if not null).
//
// Note: Even if this flag is not set, *misc.any might still be a node*...
// just not one that should be marked.
//
#define SERIES_FLAG_MISC_NODE_NEEDS_MARK \
    NODE_FLAG_GC_TWO


//=////////////////////////////////////////////////////////////////////////=//
//
// SERIES <<HEADER>> FLAGS
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Series have two places to store bits...in the "header" and in the "info".
// The following are the SERIES_FLAG_XXX and ARRAY_FLAG_XXX etc. that are used
// in the header, while the SERIES_INFO_XXX flags will be found in the info.
//
// ** Make_Series() takes SERIES_FLAG_XXX as a parameter, so anything that
// controls series creation should be a _FLAG_ as opposed to an _INFO_! **
//
// (Other general rules might be that bits that are to be tested or set as
// a group should be in the same flag group.  Perhaps things that don't change
// for the lifetime of the series might prefer header to the info, too?
// Such things might help with caching.)
//

#define SERIES_FLAGS_NONE \
    0 // helps locate places that want to say "no flags"


//=//// SERIES_FLAG_8 /////////////////////////////////////////////////////=//
//
#define SERIES_FLAG_8 \
    FLAG_LEFT_BIT(8)


//=//// SERIES_FLAG_FIXED_SIZE ////////////////////////////////////////////=//
//
// This means a series cannot be expanded or contracted.  Values within the
// series are still writable (assuming it isn't otherwise locked).
//
// !!! Is there checking in all paths?  Do series contractions check this?
//
// One important reason for ensuring a series is fixed size is to avoid
// the possibility of the data pointer being reallocated.  This allows
// code to ignore the usual rule that it is unsafe to hold a pointer to
// a value inside the series data.
//
// !!! Strictly speaking, SERIES_FLAG_NO_RELOCATE could be different
// from fixed size... if there would be a reason to reallocate besides
// changing size (such as memory compaction).  For now, just make the two
// equivalent but let the callsite distinguish the intent.
//
#define SERIES_FLAG_FIXED_SIZE \
    FLAG_LEFT_BIT(9)

#define SERIES_FLAG_DONT_RELOCATE SERIES_FLAG_FIXED_SIZE


//=//// SERIES_FLAG_POWER_OF_2 ////////////////////////////////////////////=//
//
// R3-Alpha would round some memory allocation requests up to a power of 2.
// This may well not be a good idea:
//
// http://stackoverflow.com/questions/3190146/
//
// But leaving it alone for the moment: there is a mechanical problem that the
// specific number of bytes requested for allocating series data is not saved.
// Only the series capacity measured in elements is known.
//
// Hence this flag is marked on the node, which is enough to recreate the
// actual number of allocator bytes to release when the series is freed.  The
// memory is accurately tracked for GC decisions, and balances back to 0 at
// program end.
//
// Note: All R3-Alpha's series had elements that were powers of 2, so this bit
// was not necessary there.
//
#define SERIES_FLAG_POWER_OF_2 \
    FLAG_LEFT_BIT(10)


//=//// SERIES_FLAG_ALWAYS_DYNAMIC ////////////////////////////////////////=//
//
// The optimization which uses small series will fit the data into the series
// node if it is small enough.  But doing this requires a test on SER_USED()
// and SER_DATA() to see if the small optimization is in effect.  Some
// code is more interested in the performance gained by being able to assume
// where to look for the data pointer and the length (e.g. paramlists and
// context varlists/keylists).  Passing this flag into series creation
// routines will avoid creating the shortened form.
//
// Note: Currently SERIES_INFO_INACCESSIBLE overrides this, but does not
// remove the flag...e.g. there can be inaccessible contexts that carry the
// SERIES_FLAG_ALWAYS_DYNAMIC bit but no longer have an allocation.
//
#define SERIES_FLAG_ALWAYS_DYNAMIC \
    FLAG_LEFT_BIT(11)


//=//// SERIES_FLAG_IS_STRING /////////////////////////////////////////////=//
//
// Indicates the series holds a UTF-8 encoded string.  Ren-C strings follow
// the "UTF-8 Everywhere" manifesto, where they are not decoded into a fixed
// number of bytes per character array, but remain in UTF8 at all times:
//
// http://utf8everywhere.org/
//
// There are two varieties of string series, those used by ANY-STRING! and
// those used by ANY-WORD!, tested with IS_STR_SYMBOL().  While they store
// their content the same, they use the MISC() and LINK() fields of the series
// node differently.
//
#define SERIES_FLAG_IS_STRING \
    FLAG_LEFT_BIT(12)


//=//// SERIES_FLAG_13 ////////////////////////////////////////////////////=//
//
#define SERIES_FLAG_13 \
    FLAG_LEFT_BIT(13)


//=//// SERIES_FLAG_14 ////////////////////////////////////////////////////=//
//
#define SERIES_FLAG_14 \
    FLAG_LEFT_BIT(14)


//=//// SERIES_FLAG_IS_KEYLIKE ////////////////////////////////////////////=//
//
// This flag is used to have a shareable bit between arrays and non-arrays,
// specifically so that the same bit can indicate ARRAY_FLAG_IS_KEYLIST and
// STRING_FLAG_IS_SYMBOL.  This is to anticipate a future where single-length
// keylists can be represented compactly as just a pointer to the symbol.
//
#define SERIES_FLAG_IS_KEYLIKE \
    FLAG_LEFT_BIT(15)


//=/////// ^-- STOP GENERIC SERIES FLAGS AT FLAG_LEFT_BIT(15) --^ /////////=//

// If a series is not an array, then the rightmost 16 bits of the series flags
// are used to store an arbitrary per-series-type 16 bit number.  Right now,
// that's used by the string series to save their SYMID id integer (if they
// have one).

//=/////// SEE %sys-rebarr.h for the ARRAY_FLAG_XXX definitions here //////=//


//=////////////////////////////////////////////////////////////////////////=//
//
// SERIES <<INFO>> BITS
//
//=////////////////////////////////////////////////////////////////////////=//
//
// See remarks on SERIES <<FLAG>> BITS about the two places where series store
// bits.  These are the info bits, which are more likely to be changed over
// the lifetime of the series--defaulting to FALSE.
//
// See Endlike_Header() for why the reserved bits are chosen the way they are.
//

#define SERIES_INFO_0_IS_TRUE FLAG_LEFT_BIT(0) // IS a node
STATIC_ASSERT(SERIES_INFO_0_IS_TRUE == NODE_FLAG_NODE);

#define SERIES_INFO_1_IS_FALSE FLAG_LEFT_BIT(1) // is NOT free
STATIC_ASSERT(SERIES_INFO_1_IS_FALSE == NODE_FLAG_FREE);


//=//// SERIES_INFO_2 /////////////////////////////////////////////////////=//
//
// Note: Same bit position as NODE_FLAG_MANAGED in flags, if that is relevant.
//
#define SERIES_INFO_MISC_BIT \
    FLAG_LEFT_BIT(2)


//=//// SERIES_INFO_BLACK /////////////////////////////////////////////////=//
//
// This is a generic bit for the "coloring API", e.g. Is_Series_Black(),
// Flip_Series_White(), etc.  These let native routines engage in marking
// and unmarking nodes without potentially wrecking the garbage collector by
// reusing NODE_FLAG_MARKED.  Purposes could be for recursion protection or
// other features, to avoid having to make a map from REBSER to bool.
//
// Note: Same bit as NODE_FLAG_MARKED, interesting but irrelevant.
//
#define SERIES_INFO_BLACK \
    FLAG_LEFT_BIT(3)


//=//// SERIES_INFO_PROTECTED /////////////////////////////////////////////=//
//
// This indicates that the user had a tempoary desire to protect a series
// size or values from modification.  It is the usermode analogue of
// SERIES_INFO_FROZEN_DEEP, but can be reversed.
//
// Note: There is a feature in PROTECT (CELL_FLAG_PROTECTED) which protects
// a certain variable in a context from being changed.  It is similar, but
// distinct.  SERIES_INFO_PROTECTED is a protection on a series itself--which
// ends up affecting all values with that series in the payload.
//
#define SERIES_INFO_PROTECTED \
    FLAG_LEFT_BIT(4)


//=//// SERIES_INFO_5 /////////////////////////////////////////////////////=//
//
#define SERIES_INFO_5 \
    FLAG_LEFT_BIT(5)


//=//// SERIES_INFO_FROZEN_DEEP ///////////////////////////////////////////=//
//
// Indicates that the length or values cannot be modified...ever.  It has been
// locked and will never be released from that state for its lifetime, and if
// it's an array then everything referenced beneath it is also frozen.  This
// means that if a read-only copy of it is required, no copy needs to be made.
//
// (Contrast this with the temporary condition like caused by something
// like SERIES_INFO_HOLD or SERIES_INFO_PROTECTED.)
//
// Note: This and the other read-only series checks are honored by some layers
// of abstraction, but if one manages to get a raw non-const pointer into a
// value in the series data...then by that point it cannot be enforced.
//
#define SERIES_INFO_FROZEN_DEEP \
    FLAG_LEFT_BIT(6)


// !!! Currently we lie here and act like a cell, because it allows the
// END marker trick to work without sacrificing a SERIES_FLAG.
//
#define SERIES_INFO_7_IS_TRUE FLAG_LEFT_BIT(7)
STATIC_ASSERT(SERIES_INFO_7_IS_TRUE == NODE_FLAG_CELL);


//=//// BITS 8-15 ARE FOR SER_WIDE() //////////////////////////////////////=//

// The "width" is the size of the individual elements in the series.  For an
// ANY-ARRAY this is always 0, to indicate IS_END() for arrays of length 0-1
// (singulars) which can be held completely in the content bits before the
// ->info field.  Hence this is also used for IS_SER_ARRAY()

#define FLAG_WIDE_BYTE_OR_0(wide) \
    FLAG_SECOND_BYTE(wide)

#define WIDE_BYTE_OR_0(s) \
    SECOND_BYTE((s)->info.bits)

#define mutable_WIDE_BYTE_OR_0(s) \
    mutable_SECOND_BYTE((s)->info.bits)


//=//// BITS 16-23 ARE SER_USED() FOR NON-DYNAMIC SERIES //////////////////=//

// 255 indicates that this series has a dynamically allocated portion.  If it
// is another value, then it's the length of content which is found directly
// in the series node's embedded Reb_Series_Content.
//
// (See also: SERIES_FLAG_ALWAYS_DYNAMIC to prevent creating embedded data.)
//

#define FLAG_LEN_BYTE_OR_255(len) \
    FLAG_THIRD_BYTE(len)

#define LEN_BYTE_OR_255(s) \
    THIRD_BYTE((s)->info)

#define mutable_LEN_BYTE_OR_255(s) \
    mutable_THIRD_BYTE((s)->info)


//=//// SERIES_INFO_AUTO_LOCKED ///////////////////////////////////////////=//
//
// Some operations lock series automatically, e.g. to use a piece of data as
// map keys.  This approach was chosen after realizing that a lot of times,
// users don't care if something they use as a key gets locked.  So instead
// of erroring by telling them they can't use an unlocked series as a map key,
// this locks it but changes the SERIES_FLAG_HAS_FILE_LINE to implicate the
// point where the locking occurs.
//
// !!! The file-line feature is pending.
//
#define SERIES_INFO_AUTO_LOCKED \
    FLAG_LEFT_BIT(24)


//=//// SERIES_INFO_INACCESSIBLE //////////////////////////////////////////=//
//
// Currently this used to note when a CONTEXT_INFO_STACK series has had its
// stack level popped (there's no data to lookup for words bound to it).
//
// !!! This is currently redundant with checking if a CONTEXT_INFO_STACK
// series has its `misc.f` (REBFRM) nulled out, but it means both can be
// tested at the same time with a single bit.
//
// !!! It is conceivable that there would be other cases besides frames that
// would want to expire their contents, and it's also conceivable that frames
// might want to *half* expire their contents (e.g. have a hybrid of both
// stack and dynamic values+locals).  These are potential things to look at.
//
#define SERIES_INFO_INACCESSIBLE \
    FLAG_LEFT_BIT(25)


//=//// SERIES_INFO_26 ////////////////////////////////////////////////////=//
//
// This bit is being taken as an easy way of identifying a canon symbol form
// (all lowercase).
//
#define SERIES_INFO_26 \
    FLAG_LEFT_BIT(26)

#define SERIES_INFO_SYMBOL_IS_CANON SERIES_INFO_26


//=//// SERIES_INFO_27 ////////////////////////////////////////////////////=//
//
#define SERIES_INFO_27 \
    FLAG_LEFT_BIT(27)


//=//// SERIES_INFO_KEYLIST_SHARED ////////////////////////////////////////=//
//
// This is indicated on the keylist array of a context when that same array
// is the keylist for another object.  If this flag is set, then modifying an
// object using that keylist (such as by adding a key/value pair) will require
// that object to make its own copy.
//
// Note: This flag did not exist in R3-Alpha, so all expansions would copy--
// even if expanding the same object by 1 item 100 times with no sharing of
// the keylist.  That would make 100 copies of an arbitrary long keylist that
// the GC would have to clean up.
//
#define SERIES_INFO_KEYLIST_SHARED \
    FLAG_LEFT_BIT(28)


//=//// SERIES_INFO_HOLD //////////////////////////////////////////////////=//
//
// Set in the header whenever some stack-based operation wants a temporary
// hold on a series, to give it a protected state.  This will happen with a
// DO, or PARSE, or enumerations.  Even REMOVE-EACH will transition the series
// it is operating on into a HOLD state while the removal signals are being
// gathered, and apply all the removals at once before releasing the hold.
//
// It will be released when the execution is finished, which distinguishes it
// from SERIES_INFO_FROZEN_DEEP, which will never be cleared once set.
//
// Note: This is set to be the same bit as DETAILS_FLAG_IS_NATIVE in order to
// make it possible to use non-branching masking to set a frame to read-only
// as far as usermode operations are concerned.
//
#define SERIES_INFO_HOLD \
    FLAG_LEFT_BIT(29)


//=//// SERIES_INFO_FROZEN_SHALLOW ////////////////////////////////////////=//
//
// A series can be locked permanently, but only at its own top level.
//
#define SERIES_INFO_FROZEN_SHALLOW \
    FLAG_LEFT_BIT(30)


#ifdef DEBUG_MONITOR_SERIES

    //=//// SERIES_INFO_MONITOR_DEBUG /////////////////////////////////////=//
    //
    // Simple feature for tracking when a series gets freed or otherwise
    // messed with.  Setting this bit on it asks for a notice.
    //
    #define SERIES_INFO_MONITOR_DEBUG \
        FLAG_LEFT_BIT(31)
#endif


// ^-- STOP AT FLAG_LEFT_BIT(31) --^
//
// While 64-bit systems have another 32-bits available in the header, core
// functionality shouldn't require using them...only optimization features.
//
STATIC_ASSERT(31 < 32);


//=////////////////////////////////////////////////////////////////////////=//
//
// SERIES NODE ("REBSER") STRUCTURE DEFINITION
//
//=////////////////////////////////////////////////////////////////////////=//
//
// A REBSER node is normally the size of two REBVALs (though compiling with
// certain debug flags can add tracking information).  See %sys-rebnod.h for
// explanations of how obeying the header-in-first-slot convention allows a
// REBSER to be distinguished from a REBVAL or a UTF-8 string and not run
// afoul of strict aliasing requirements.
//
// There are 3 basic layouts which can be overlaid inside the union (the
// "leader" is the "header" with a different name to avoid uses which might
// alias with bits initialized through REBVAL->header):
//
//      Dynamic: [leader link [allocation tracking] info misc]
//     Singular: [leader link [REBVAL cell] info misc]
//      Pairing: [[REBVAL cell] [REBVAL cell]]
//
// The singular form has space the *size* of a cell, but can be addressed as
// raw bytes used for UTF-8 strings or other smallish data.  If a REBSER is
// aligned on a 64-bit boundary, the internal cell should be on a 64-bit
// boundary as well, even on a 32-bit platform where the header and link are
// each 32-bits.  See ALIGN_SIZE for notes on why this is important.
//
// `info` is not the start of a "Rebol Node" (REBNODE, e.g. either a REBSER or
// a REBVAL cell).  But in the singular case it is positioned right where
// the next cell after the embedded cell *would* be.  Hence the second byte
// in the info corresponding to VAL_TYPE() is 0, making it conform to the
// "terminating array" pattern.  To lower the risk of this implicit terminator
// being accidentally overwritten (which would corrupt link and misc), the
// bit corresponding to NODE_FLAG_CELL is clear.
//
// Singulars have widespread applications in the system.  One is that a
// "single element array living in a series node" makes a very efficient
// implementation of an API handle to a value.  Plus it's used notably in the
// efficient implementation of FRAME!.  They also narrow the gap in overhead
// between COMPOSE [A (B) C] vs. REDUCE ['A B 'C] such that the memory cost
// of the array is nearly the same as just having another value in the array.
//
// Pair REBSERs are allocated from the REBSER pool instead of their own to
// help exchange a common "currency" of allocation size more efficiently.
// They are used in the PAIR! datatype, but can have other interesting
// applications when exactly two values (with no termination) are needed.
//
// Most of the time, code does not need to be concerned about distinguishing
// Pair from the Dynamic and Singular layouts--because it already knows
// which kind it has.  Only the GC needs to be concerned when marking
// and sweeping.
//

struct Reb_Series_Dynamic {
    //
    // `data` is the "head" of the series data.  It might not point directly
    // at the memory location that was returned from the allocator if it has
    // bias included in it.
    //
    // !!! We use `char*` here to ease debugging in systems that don't show
    // ASCII by default for unsigned characters, for when it's UTF-8 data.
    //
    char *data;

    // `used` is the count of *physical* elements.  If a series is byte-sized
    // and holding a UTF-8 string, then this may be a size in bytes distinct
    // than the count of "logical" elements, e.g. codepoints.  The actual
    // logical length in such cases will be in the MISC(length) field.
    //
    REBLEN used;

    // `rest` is the total number of units from bias to end.  Having a
    // slightly weird name draws attention to the idea that it's not really
    // the "capacity", just the "rest of the capacity after the bias".
    //
    REBLEN rest;

    // This is the 4th pointer on 32-bit platforms which could be used for
    // something when a series is dynamic.  Previously the bias was not
    // a full REBLEN but was limited in range to 16 bits or so.  This means
    // 16 info bits are likely available if needed for dynamic series.
    //
    REBLEN bias;
};


union Reb_Series_Content {
    //
    // If the series does not fit into the REBSER node, then it must be
    // dynamically allocated.  This is the tracking structure for that
    // dynamic data allocation.
    //
    struct Reb_Series_Dynamic dynamic;

    // If LEN_BYTE_OR_255() != 255, 0 or 1 length arrays can be held in
    // the series node.  This trick is accomplished via "implicit termination"
    // in the ->info bits that come directly after ->content.  For how this is
    // done, see Endlike_Header()
    //
    union {
        // Due to strict aliasing requirements, this has to be initialized
        // as a value to read cell data.  It is a Reb_Cell in order to let
        // series nodes be memcpy()'d as part of their mechanics, but this
        // should not be used to actually "move" cells!  Use Move_Value()
        //
        REBRAW cells[1];

      #if defined(DEBUG_USE_UNION_PUNS)
        char utf8_pun[sizeof(RELVAL)];  // debug watchlist insight into UTF-8
        REBWCHAR ucs2_pun[sizeof(RELVAL)/sizeof(REBUNI)];  // wchar_t insight
      #endif
    } fixed;
};

#define SER_CELL(s) \
    cast(RELVAL*, &(s)->content.fixed.cells[0])  // unchecked ARR_SINGLE()


union Reb_Series_Link {
    //
    // If you assign one member in a union and read from another, then that's
    // technically undefined behavior.  But this field is used as the one
    // that is "trashed" in the debug build when the series is created, and
    // hopefully it will lead to the other fields reading garbage (vs. zero)
    //
  #if !defined(NDEBUG)
    void *trash;
  #endif

    // For LIBRARY!, the file descriptor.  This is set to NULL when the
    // library is not loaded.
    //
    // !!! As with some other types, this may not need the optimization of
    // being in the Reb_Series node--but be handled via user defined types
    //
    void *fd;

    // native dispatcher code, the LINK() on ACT_DETAILS() for actions
    //
    REBNAT dispatcher;

    // If a REBNOD* is stored in the link field, it has to use this union
    // member for SERIES_INFO_LINK_NODE_NEEDS_MARK to see it.  To help make
    // the reference sites be unique for each purpose and still be type safe,
    // see the LINK() macro helpers.
    //
    union Reb_Any any;
};


// The `misc` field is an extra pointer-sized piece of data which is resident
// in the series node, and hence visible to all REBVALs that might be
// referring to the series.
//
union Reb_Series_Misc {
    //
    // Used to preload bad data in the debug build; see notes on link.trash
    //
  #if !defined(NDEBUG)
    void *trash;
  #endif

    // See ARRAY_FLAG_FILE_LINE.  Ordinary source series store the line number
    // here.  It perhaps could have some bits taken out of it, vs. being a
    // full 32-bit integer on 32-bit platforms or 64-bit integer on 64-bit
    // platforms...or have some kind of "extended line" flag which interprets
    // it as a dynamic allocation otherwise to get more bits.
    //
    REBLIN line;

    // Under UTF-8 everywhere, strings are byte-sized...so the series "used"
    // is actually counting *bytes*, not logical character codepoint units.
    // SER_USED() and STR_LEN() can therefore be different...where STR_LEN()
    // on a string series comes from here, vs. just report the used units.
    //
    REBLEN length;

    // When binding words into a context, it's necessary to keep a table
    // mapping those words to indices in the context's keylist.  R3-Alpha
    // had a global "binding table" for the spellings of words, where
    // those spellings were not garbage collected.  Ren-C uses REBSERs
    // to store word spellings, and then has a hash table indexing them.
    //
    // So the "binding table" is chosen to be indices reachable from the
    // REBSER nodes of the words themselves.  If it were necessary for
    // multiple clients to have bindings at the same time, this could be
    // done through a pointer that would "pop out" into some kind of
    // linked list.  For now, the binding API just demonstrates having
    // up to 2 different indices in effect at once.
    //
    // Note that binding indices can be negative, so the sign can be used
    // to encode a property of that particular binding.
    //
    struct {
        int high:16;
        int low:16;
    } bind_index;

    // Used on arrays for special instructions to Fetch_Next_In_Frame().
    //
    enum Reb_Api_Opcode opcode;

    // some HANDLE!s use this for GC finalization
    //
    CLEANUP_CFUNC *cleaner;

    // Because a bitset can get very large, the negation state is stored
    // as a boolean in the series.  Since negating a bitset is intended
    // to affect all values, it has to be stored somewhere that all
    // REBVALs would see a change--hence the field is in the series.
    //
    bool negated;

    // rebQ() and rebU() use this with ARRAY_FLAG_INSTRUCTION_ADJUST_QUOTING.
    //
    int quoting_delta;

    // If a REBNOD* is stored in the misc field, it has to use this union
    // member for SERIES_INFO_MISC_NODE_NEEDS_MARK to see it.  To help make
    // the reference sites be unique for each purpose and still be type safe,
    // see the MISC() macro helpers.
    //
    union Reb_Any any;
};


#ifdef CPLUSPLUS_11
    struct Reb_Series_Base
#else
    struct Reb_Series
#endif
{
    // See the description of SERIES_FLAG_XXX for the bits in this header.
    // It is in the same position as a REBVAL* header, and the first byte
    // can be read via NODE_BYTE() to determine which it is.  However, it is
    // not called `header` because that would suggest you could have a pointer
    // and not know if it was a REBVAL* or REBSER* and read/write it safely...
    // you cannot do that because of "strict aliasing":
    //
    // https://stackoverflow.com/q/51846048/
    //
    // So the series header is called "leader" to make accidents less likely.
    //
    union Reb_Header leader;

    // The `link` field is generally used for pointers to something that
    // when updated, all references to this series would want to be able
    // to see.  This cannot be done (easily) for properties that are held
    // in REBVAL cells directly.
    //
    // This field is in the second pointer-sized slot in the REBSER node to
    // push the `content` so it is 64-bit aligned on 32-bit platforms.  This
    // is because a REBVAL may be the actual content, and a REBVAL assumes
    // it is on a 64-bit boundary to start with...in order to position its
    // "payload" which might need to be 64-bit aligned as well.
    //
    // Use the LINK() macro to acquire this field...don't access directly.
    //
    union Reb_Series_Link link;

    // `content` is the sizeof(REBVAL) data for the series, which is thus
    // 4 platform pointers in size.  If the series is small enough, the header
    // contains the size in bytes and the content lives literally in these
    // bits.  If it's too large, it will instead be a pointer and tracking
    // information for another allocation.
    //
    union Reb_Series_Content content;

    // `info` consists of bits that could apply equally to any series, and
    // that may need to be tested together as a group.  Make_Series_Core()
    // calls presume all the info bits are initialized to zero, so any flag
    // that controls the allocation should be a SERIES_FLAG_XXX instead.
    //
    // It is purposefully positioned in the structure directly after the
    // ->content field, because its second byte is '\0' when the series is
    // an array.  Hence it appears to terminate an array of values if the
    // content is not dynamic.  Yet NODE_FLAG_CELL is set to false, so it is
    // not a writable location (an "implicit terminator").
    //
    // !!! Only 32-bits are used on 64-bit platforms.  There could be some
    // interesting added caching feature or otherwise that would use
    // it, while not making any feature specifically require a 64-bit CPU.
    //
    union Reb_Header info;

    // This is the second pointer-sized piece of series data that is used
    // for various purposes, similar to link.
    //
    union Reb_Series_Misc misc;

#if defined(DEBUG_SERIES_ORIGINS) || defined(DEBUG_COUNT_TICKS)
    intptr_t *guard; // intentionally alloc'd and freed for use by Panic_Series
    uintptr_t tick; // also maintains sizeof(REBSER) % sizeof(REBI64) == 0
#endif
};


// In C++, REBSTR and REBARR are declared as derived from REBSER.  This gives
// desirable type checking properties (like being able to pass an array to
// a routine that needs a series, but not vice versa).  And it also means
// that the fields are available.
//
// In order for the inheritance to be known, these definitions cannot occur
// until Reb_Series is fully defined.  So this is the earliest it can be done:
//
// https://stackoverflow.com/q/2159390/
//
#ifdef __cplusplus
    struct Reb_Series : public Reb_Node, public Reb_Series_Base {};

    struct Reb_Binary : public Reb_Series {};
    typedef struct Reb_Binary REBBIN;

    struct Reb_String : public Reb_Binary {};  // strings can act as binaries
    typedef struct Reb_String REBSTR;

    struct Reb_Symbol : public Reb_String {};  // word-constrained strings
    typedef struct Reb_Symbol REBSYM;

    struct Reb_Canon : public Reb_Symbol {};  // all-lowercase symbol
    typedef struct Reb_Canon REBCAN;

    struct Reb_Array : public Reb_Series {};
    typedef struct Reb_Array REBARR;

    struct Reb_Bookmark_List : public Reb_Series {};
    typedef struct Reb_Bookmark_List REBBMK;  // list of UTF-8 index=>offsets

#else
    typedef struct Reb_Series REBBIN;
    typedef struct Reb_Series REBSTR;
    typedef struct Reb_Series REBSYM;
    typedef struct Reb_Series REBCAN;
    typedef struct Reb_Series REBARR;
    typedef struct Reb_Series REBBMK;
#endif


// We want to be able to enumerate keys by incrementing across them.  The
// things we increment across aren't REBSER nodes, but pointers to REBSER
// nodes for the strings... so a "key" is a pointer.
//
// Due to strong philosophical feelings that Redbol languages should be case
// insensitive when it comes to binding, object keys must be canon symbols
// (REBCAN), e.g. lowercase canonized.  Having case-insensitive lookup but
// preserving case in keys would lead to inconsistent states.
//
// https://forum.rebol.info/t/1439
//
typedef const REBCAN *REBKEY;


// It may become interesting to say that a specifier can be a pairing or
// a REBVAL* of some kind.  But for the moment, that just complicates the
// issue of not being able to check the ->header bits safely (it would require
// checking the NODE_BYTE() first, then casting to a VAL() or SER()).  In
// the interests of making the code strict-aliasing-safe for starters, assume
// all specifiers are arrays.
//
typedef REBARR REBSPC;


// To help document places in the core that are complicit in the "extension
// hack", alias arrays being used for the FFI and GOB to another name.
//
typedef REBARR REBGOB;

typedef REBARR REBSTU;
typedef REBARR REBFLD;

typedef REBBIN REBTYP;  // Rebol Type (list of hook function pointers)


//=//// DON'T PUT ANY CODE (OR MACROS THAT MAY NEED CODE) IN THIS FILE! ///=//
//
// The %tmp-internals.h file has not been included, and hence none of the
// prototypes (even for things like Panic_Core()) are available.
//
// Even if a macro seems like it doesn't need code right at this moment, you
// might want to put some instrumentation into it, and that becomes a pain of
// manual forward declarations.
//
// So keep this file limited to structs and constants.  It's too long already.
//
//=////////////////////////////////////////////////////////////////////////=//
