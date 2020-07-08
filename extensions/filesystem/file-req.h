EXTERN_C REBDEV Dev_File;

// !!! Hack used for making a 64-bit value as a struct, which works in
// 32-bit modes.  64 bits, even in 32 bit mode.  Based on the deprecated idea
// that "devices" would not have access to Rebol datatypes, and hence would
// not be able to communicate with Rebol directly with a TIME! or DATE!.
// To be replaced.
//
// (Note: compatible with FILETIME used in Windows)
//
#pragma pack(4)
typedef struct sInt64 {
    int32_t l;
    int32_t h;
} FILETIME_DEVREQ;
#pragma pack()

struct devreq_file {
    struct rebol_devreq devreq;
    const REBVAL *path;     // file string (in OS local format)
    int64_t size;           // file size
    int64_t index;          // file index position
    FILETIME_DEVREQ time;   // file modification time (struct)
};

inline static struct devreq_file* ReqFile(REBREQ *req) {
    assert(Req(req)->device == &Dev_File);
    return cast(struct devreq_file*, Req(req));
}

extern REBVAL *File_Time_To_Revolt(REBREQ *file);
extern REBVAL *Query_File_Or_Dir(const REBVAL *port, REBREQ *file);

#ifdef TO_WINDOWS
    #define OS_DIR_SEP '\\'  // file path separator (Thanks Bill.)
#else
    #define OS_DIR_SEP '/'  // rest of the world uses it
#endif
