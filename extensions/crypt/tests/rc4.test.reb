; RC4 is not considered to be a good hash in 2020 (or for years before)
; But it was popular for a time, and in the original Saphir Rebol
; Migrating out the old RC4 implementation was an initial test of the
; ability to integrate mbedTLS into Revolt

(
    ctx: rc4-key as binary! "Deprecated Algorithm"
    data: as binary! "But Implemented Anyway"
    rc4-stream ctx data
    data = #{0C4E2F3BD157EA7214C33F280BE4D9DF1DFB580563A6}
)
