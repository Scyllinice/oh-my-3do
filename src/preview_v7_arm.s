; OM3DVID7 decoding for the ARM60 (see preview_v7.h).
; Matches preview_v7.c bit for bit on valid packets. r10 (the APCS stack
; limit) is never touched.
;
; int preview7_decode(Preview7 *state, const unsigned char *data,
;                     unsigned long bytes, int picture)
;   Draws each block the packet changes into the screen bitmap (picture 0)
;   or the TV picture (1) and records it in the map. Returns 0, or -1 for an
;   op that would pass the last block or the packet's end.
; void preview7_render_arm(Preview7 *state, int picture)
;   Draws every block from the map.
;
; Registers:
;   r0 next packet byte (always even). When r0 & 2, r3 holds the word that
;      contains the halfword at r0, in its low half. Reads up to two bytes
;      past the packet.
;   r1 entry base for the current run   r2 destination of the current block
;   r4 blocks left in the current run   r5 where the current block row ends
;   r6 map entry of the current block   r7-r9 r11 r12 lr scratch
;
; void preview7_sum_arm(Preview7Sum *sum, const unsigned char *data,
;                       unsigned long bytes)
;   Adds whole, word-aligned words to the preview word sum: a += w; b += a.
;
;   [sp] the map's end, [sp,#4] 4x4 base, [sp,#8] 2x2 base, [sp,#12] the
;   packet's end
; Each op is checked against the blocks and bytes left, so a damaged packet
; cannot write outside the picture or the map; entry numbers need no checks
; because the dictionaries hold every number the format can express.

        AREA    |C$$code|, CODE, READONLY
        EXPORT  preview7_decode
        EXPORT  preview7_render_arm
        EXPORT  preview7_sum_arm

; Preview7 field offsets
S_SCREEN EQU    0
S_THUMB  EQU    4
S_MAP    EQU    8
S_DICT4  EQU    12
S_THUMB4 EQU    16
S_DICT2  EQU    20
S_THUMB2 EQU    24

PAIR    EQU     1280            ; bytes per line pair of a screen bitmap
TROW    EQU     320             ; bytes per line of the TV picture

; The halfword at r0 into $h; r0 advances.
        MACRO
        HALF    $h
        TST     r0, #2
        LDREQ   r3, [r0]
        MOVEQ   $h, r3, LSR #16
        MOVNE   $h, r3, LSL #16
        MOVNE   $h, $h, LSR #16
        ADD     r0, r0, #2
        MEND

; Step r2 to the next block, wrapping at the end of a block row.
        MACRO
        NEXT    $tv
        IF $tv = 0
        ADD     r2, r2, #16
        CMP     r2, r5
        ADDEQ   r2, r2, #PAIR
        ADDEQ   r5, r5, #2*PAIR
        ELSE
        ADD     r2, r2, #4
        CMP     r2, r5
        ADDEQ   r2, r2, #TROW
        ADDEQ   r5, r5, #2*TROW
        ENDIF
        MEND

; Draw 4x4 entry r7 (base r1) at r2.
        MACRO
        DRAW4   $tv
        IF $tv = 0
        ADD     r7, r1, r7, LSL #5
        LDMIA   r7!, {r8, r9, r11, r12}
        STMIA   r2, {r8, r9, r11, r12}
        LDMIA   r7, {r8, r9, r11, r12}
        ADD     r7, r2, #PAIR
        STMIA   r7, {r8, r9, r11, r12}
        ELSE
        ADD     r7, r1, r7, LSL #3
        LDMIA   r7, {r8, r9}
        STR     r8, [r2]
        STR     r9, [r2, #TROW]
        ENDIF
        MEND

; Draw the quad with 2x2 entries packed in r7 (top two, bit 31 may be set)
; and lr (bottom two), base r1, at r2.
        MACRO
        DRAWQ   $tv
        IF $tv = 0
        MOV     r9, r7, LSL #8
        MOV     r9, r9, LSR #20
        ADD     r9, r1, r9, LSL #3
        MOV     r11, r7, LSL #20
        ADD     r11, r1, r11, LSR #17
        LDMIA   r9, {r8, r9}
        LDMIA   r11, {r11, r12}
        STMIA   r2, {r8, r9, r11, r12}
        MOV     r9, lr, LSR #12
        ADD     r9, r1, r9, LSL #3
        MOV     r11, lr, LSL #20
        ADD     r11, r1, r11, LSR #17
        LDMIA   r9, {r8, r9}
        LDMIA   r11, {r11, r12}
        ADD     r7, r2, #PAIR
        STMIA   r7, {r8, r9, r11, r12}
        ELSE
        MOV     r9, r7, LSL #8
        MOV     r9, r9, LSR #20
        LDR     r9, [r1, r9, LSL #2]
        MOV     r11, r7, LSL #20
        MOV     r11, r11, LSR #20
        LDR     r11, [r1, r11, LSL #2]
        ORR     r9, r9, r11, LSR #16
        STR     r9, [r2]
        MOV     r9, lr, LSR #12
        LDR     r9, [r1, r9, LSL #2]
        MOV     r11, lr, LSL #20
        MOV     r11, r11, LSR #20
        LDR     r11, [r1, r11, LSL #2]
        ORR     r9, r9, r11, LSR #16
        STR     r9, [r2, #TROW]
        ENDIF
        MEND

; Fail unless r4 blocks remain.
        MACRO
        BLOCKS_LEFT
        LDR     r8, [sp]
        SUB     r8, r8, r6
        CMP     r8, r4, LSL #3
        BLT     bad
        MEND

; Block = 4x4 entry r7.
        MACRO
        FOUR    $tv
        STR     r7, [r6], #8
        DRAW4   $tv
        NEXT    $tv
        MEND

; Block = quad r7/lr.
        MACRO
        QUAD    $tv
        ORR     r7, r7, #&80000000
        STMIA   r6!, {r7, lr}
        DRAWQ   $tv
        NEXT    $tv
        MEND

; The op loop for one picture; labels start with $p.
        MACRO
        OPS     $p, $tv
$p.op
        LDR     r8, [sp, #12]
        CMP     r0, r8
        BHS     bad
        HALF    r7
        MOV     r8, r7, LSR #13
        BIC     r7, r7, #&E000
        ADD     pc, pc, r8, LSL #2
        MOV     r0, r0
        B       $p.one
        B       $p.skip
        B       $p.run4
        B       $p.runq

$p.one
        LDR     r1, [sp, #4]
        FOUR    $tv
        B       $p.next

$p.skip
        ADD     r4, r7, #1
        BLOCKS_LEFT
        ADD     r6, r6, r4, LSL #3
$p.skip_row
        SUB     r8, r5, r2
        IF $tv = 0
        CMP     r4, r8, LSR #4
        ADDLT   r2, r2, r4, LSL #4
        BLT     $p.next
        SUB     r4, r4, r8, LSR #4
        ADD     r2, r5, #PAIR
        ADD     r5, r5, #2*PAIR
        ELSE
        CMP     r4, r8, LSR #2
        ADDLT   r2, r2, r4, LSL #2
        BLT     $p.next
        SUB     r4, r4, r8, LSR #2
        ADD     r2, r5, #TROW
        ADD     r5, r5, #2*TROW
        ENDIF
        B       $p.skip_row

$p.run4
        ADD     r4, r7, #1
        BLOCKS_LEFT
        LDR     r8, [sp, #12]
        SUB     r8, r8, r0
        CMP     r8, r4, LSL #1
        BLT     bad
        LDR     r1, [sp, #4]
        TST     r0, #2
        BEQ     $p.pairs
        ; The first entry is the pending halfword.
        MOV     r7, r3, LSL #19         ; (13 bits: a damaged entry number
        MOV     r7, r7, LSR #19         ; still stays in the dictionary)
        ADD     r0, r0, #2
        FOUR    $tv
        SUBS    r4, r4, #1
        BEQ     $p.next
$p.pairs
        SUBS    r4, r4, #2
        BLT     $p.last
$p.pair
        LDR     r3, [r0], #4
        MOV     r7, r3, LSR #16
        BIC     r7, r7, #&E000
        FOUR    $tv
        MOV     r7, r3, LSL #19
        MOV     r7, r7, LSR #19
        FOUR    $tv
        SUBS    r4, r4, #2
        BGE     $p.pair
$p.last
        ADDS    r4, r4, #2
        BEQ     $p.next
        LDR     r3, [r0]
        MOV     r7, r3, LSR #16
        BIC     r7, r7, #&E000
        ADD     r0, r0, #2
        FOUR    $tv
        B       $p.next

; Quads are 6 bytes, so they alternate between starting on a word and
; starting on its second half.
$p.runq
        ADD     r4, r7, #1
        BLOCKS_LEFT
        LDR     r8, [sp, #12]
        SUB     r8, r8, r0
        ADD     r9, r4, r4, LSL #1
        CMP     r8, r9, LSL #1
        BLT     bad
        LDR     r1, [sp, #8]
        TST     r0, #2
        BNE     $p.q_half
$p.q_word
        LDMIA   r0, {r7, lr}            ; the quad's first word, then its last
        ADD     r0, r0, #6              ; halfword and the next one
        MOV     r3, lr
        MOV     lr, r7, LSL #24
        MOV     lr, lr, LSR #8
        ORR     lr, lr, r3, LSR #16
        MOV     r7, r7, LSR #8
        QUAD    $tv
        SUBS    r4, r4, #1
        BEQ     $p.next
$p.q_half
        LDR     lr, [r0, #2]
        ADD     r0, r0, #6
        MOV     r7, r3, LSL #16
        MOV     r7, r7, LSR #8
        ORR     r7, r7, lr, LSR #24
        MOV     lr, lr, LSL #8
        MOV     lr, lr, LSR #8
        QUAD    $tv
        SUBS    r4, r4, #1
        BNE     $p.q_word

$p.next
        LDR     r8, [sp]
        CMP     r6, r8
        BNE     $p.op
        LDR     r8, [sp, #12]
        CMP     r0, r8
        BNE     bad
        B       done
        MEND

; Every block from the map; labels start with $p.
        MACRO
        RENDER  $p, $tv
$p.block
        LDMIA   r6!, {r7, lr}
        CMN     r7, #&80000001          ; black (0x7fffffff)?
        BEQ     $p.black
        TST     r7, #&80000000
        BNE     $p.quad
        LDR     r1, [sp, #4]
        DRAW4   $tv
        B       $p.next
$p.quad
        LDR     r1, [sp, #8]
        DRAWQ   $tv
        B       $p.next
$p.black
        MOV     r8, #0
        IF $tv = 0
        MOV     r9, #0
        MOV     r11, #0
        MOV     r12, #0
        STMIA   r2, {r8, r9, r11, r12}
        ADD     r7, r2, #PAIR
        STMIA   r7, {r8, r9, r11, r12}
        ELSE
        STR     r8, [r2]
        STR     r8, [r2, #TROW]
        ENDIF
$p.next
        NEXT    $tv
        LDR     r8, [sp]
        CMP     r6, r8
        BNE     $p.block
        B       done
        MEND

; Shared entry (SETUP): r0 state, r3 picture. Sets up r1 r2 r5 r6 and the stack.
        MACRO
        SETUP
        STMFD   sp!, {r4-r9, r11, lr}
        SUB     sp, sp, #16
        LDR     r6, [r0, #S_MAP]
        ADD     r8, r6, #4800*8
        STR     r8, [sp]
        CMP     r3, #0
        LDREQ   r8, [r0, #S_DICT4]
        LDREQ   r9, [r0, #S_DICT2]
        LDREQ   r2, [r0, #S_SCREEN]
        ADDEQ   r5, r2, #PAIR
        LDRNE   r8, [r0, #S_THUMB4]
        LDRNE   r9, [r0, #S_THUMB2]
        LDRNE   r2, [r0, #S_THUMB]
        ADDNE   r5, r2, #TROW
        STR     r8, [sp, #4]
        STR     r9, [sp, #8]
        MEND

preview7_decode
        ADD     r12, r1, r2             ; the packet's end (SETUP reuses r2)
        SETUP
        STR     r12, [sp, #12]
        CMP     r3, #0
        MOV     r0, r1
        BNE     tv_decode
        TST     r0, #2
        LDRNE   r3, [r0, #-2]
        OPS     s, 0

tv_decode
        TST     r0, #2
        LDRNE   r3, [r0, #-2]
        OPS     t, 1

preview7_render_arm
        MOV     r3, r1
        SETUP
        CMP     r3, #0
        BNE     tv_render
        RENDER  rs, 0

tv_render
        RENDER  rt, 1

        MACRO
        SUMW    $r
        ADD     r3, r3, $r
        ADD     r12, r12, r3
        MEND

preview7_sum_arm
        STMFD   sp!, {r4-r9, r11, lr}
        LDMIA   r0, {r3, r12}
        SUBS    r2, r2, #32
        BLT     sum_tail
sum_eight
        LDMIA   r1!, {r4-r9, r11, lr}
        SUMW    r4
        SUMW    r5
        SUMW    r6
        SUMW    r7
        SUMW    r8
        SUMW    r9
        SUMW    r11
        SUMW    lr
        SUBS    r2, r2, #32
        BGE     sum_eight
sum_tail
        ADDS    r2, r2, #32
        BEQ     sum_done
sum_word
        LDR     r4, [r1], #4
        SUMW    r4
        SUBS    r2, r2, #4
        BGT     sum_word
sum_done
        STMIA   r0, {r3, r12}
        LDMFD   sp!, {r4-r9, r11, pc}

done
        MOV     r0, #0
        ADD     sp, sp, #16
        LDMFD   sp!, {r4-r9, r11, pc}
bad
        MVN     r0, #0
        ADD     sp, sp, #16
        LDMFD   sp!, {r4-r9, r11, pc}

        END
