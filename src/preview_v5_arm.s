; OM3DVID5 frame decoder for the ARM60 (see preview_v5.h for the format).
;
;   int preview_v5_decode(PreviewV5 *state, const unsigned char *data,
;                         unsigned long bytes, unsigned char *pixels)
;
; Returns 0, or -1 for a damaged packet. It produces exactly the same pixels
; as the C version in preview_v5.c, which tests/test_preview_asm.py checks.
; It exists because the C version was too slow on the console: this one
; writes whole rows of blocks at once with multi-register loads and stores
; (LDM/STM), which move several words per instruction.
;
; Calling convention (ARM APCS): arguments arrive in r0-r3, the result goes
; back in r0, and r4-r11 must be restored before returning. r10 is the
; stack limit register and is never touched.
;
; Main loop registers:
;   r0 data (the next packet byte)   r1 end of the packet
;   r2 destination: the top-left word of the current block in the frame
;   r3 block column (0-79)   r4 blocks left in the frame
;   r5 color cache (state->cache)   r6 next cache slot (state->next)
;   r8 blocks left in the current run
;   r7 r9 r11 r12 lr scratch
;
; A frame is 160x120 pixels of 2 bytes, so a pixel row is 320 bytes and a
; 4x4 block is 8 bytes wide. Two pixels share each 32-bit word, which is why
; a MOTION copy with an odd horizontal offset must stitch half-words
; together.

        AREA    |C$$code|, CODE, READONLY
        EXPORT  preview_v5_decode

WIDTH   EQU     160
HEIGHT  EQU     120
STRIDE  EQU     WIDTH*2         ; bytes per pixel row
STRIDE5 EQU     6               ; STRIDE = 5 << STRIDE5 (multiply by shifts)
WRAP    EQU     3*STRIDE        ; destination step at the end of a block row
COLUMNS EQU     WIDTH/4         ; blocks across
BLOCKS  EQU     COLUMNS*HEIGHT/4 ; blocks in a frame
MOTION_RUN EQU  32              ; longest MOTION run (PREVIEW_V5_MOTION_RUN)

; Reads one color code into $r (an RGB555 value); $t is scratch.
; A byte with the top bit set names a cached color; otherwise it and the
; next byte are a new color, which is also stored in the cache at slot r6
; (round robin over 128 slots).
        MACRO
        COLOR  $r, $t
        LDRB    $r, [r0], #1            ; first byte
        TST     $r, #0x80
        BICNE   $r, $r, #0x80           ; cached: slot number
        LDRNE   $r, [r5, $r, LSL #2]    ; cached: the color
        LDREQB  $t, [r0], #1            ; new: second byte
        ORREQ   $r, $t, $r, LSL #8      ; new: the two bytes as one color
        STREQ   $r, [r5, r6, LSL #2]    ; new: remember it...
        ADDEQ   r6, r6, #1              ; ...in the next slot
        ANDEQ   r6, r6, #127
        MEND

; PREVIEW_V5_MIX: from $x = 2*spread(a)+spread(b) into $d. "Spread" puts the
; three 5-bit channels in 10-bit lanes, so all three are mixed with one
; multiply by 11 and then divided by 32 (11/32 is close to 1/3).
        MACRO
        MIX     $d, $x, $t
        ADD     $t, $x, $x, LSL #2      ; 5x
        ADD     $x, $x, $t, LSL #1      ; 11x: lanes stay below 1024
        MOV     $d, $x, LSR #5          ; blue lane / 32
        AND     $d, $d, #0x1F
        MOV     $t, $x, LSR #10         ; green lane / 32, into place
        AND     $t, $t, #0x3E0
        ORR     $d, $d, $t
        MOV     $t, $x, LSR #15         ; red lane / 32, into place
        AND     $t, $t, #0x7C00
        ORR     $d, $d, $t
        MEND

; One row of a GRAD block: the row's four 2-bit palette indexes are at bits
; $s, $s-2, $s-4 and $s-6 of r7; the four palette colors are on the stack;
; r12 holds 3 (the index mask). Writes the row's two words at [r2 + $off].
        MACRO
        GROW    $s, $off
        AND     r9, r12, r7, LSR #$s
        LDR     r9, [sp, r9, LSL #2]    ; pixel 0
        AND     r11, r12, r7, LSR #$s-2
        LDR     r11, [sp, r11, LSL #2]  ; pixel 1
        ORR     r9, r11, r9, LSL #16    ; pixels 0 and 1 in one word
        AND     r11, r12, r7, LSR #$s-4
        LDR     r11, [sp, r11, LSL #2]  ; pixel 2
        IF $s = 6
        AND     lr, r12, r7             ; (a shift of 0 is written without LSR)
        ELSE
        AND     lr, r12, r7, LSR #$s-6
        ENDIF
        LDR     lr, [sp, lr, LSL #2]    ; pixel 3
        ORR     r11, lr, r11, LSL #16   ; pixels 2 and 3 in one word
        STR     r9, [r2, #$off]
        STR     r11, [r2, #$off+4]
        MEND

preview_v5_decode
        STMFD   sp!, {r4-r9, r11, lr}   ; save the registers APCS says to keep
        SUB     sp, sp, #12             ; three words of locals:
        STR     r3, [sp, #0]            ; [sp] frame start
        LDR     r12, =(HEIGHT-3)*STRIDE
        ADD     r12, r3, r12
        STR     r12, [sp, #4]           ; [sp+4] no source block starts on or after this row
        STR     r0, [sp, #8]            ; [sp+8] state
        MOV     r5, r0                  ; r5 = the color cache (at offset 0 of state)
        LDR     r6, [r5, #512]          ; r6 = state->next
        ADD     r12, r1, r2             ; end = data + bytes
        MOV     r0, r1                  ; r0 = data
        MOV     r1, r12                 ; r1 = end
        MOV     r2, r3                  ; r2 = the first block's pixels
        MOV     r3, #0                  ; column 0
        MOV     r4, #BLOCKS

; Each op byte is mode << 6 | (run - 1).
next_op
        CMP     r4, #0
        BEQ     finish                  ; every block covered
        LDRB    r7, [r0], #1            ; past the end only into the read-ahead
        AND     r8, r7, #63
        ADD     r8, r8, #1              ; r8 = run length
        SUBS    r4, r4, r8
        BMI     fail                    ; the run would pass the last block
        MOV     r7, r7, LSR #6          ; r7 = mode
        ADD     pc, pc, r7, LSL #2      ; jump through the table below (pc reads
        MOV     r0, r0                  ; two instructions ahead, so this filler
        B       op_skip                 ; is skipped)
        B       op_motion
        B       op_solid
        B       op_grad
op_done
        CMP     r0, r1                  ; overruns are caught after the run
        BLS     next_op
fail
        MVN     r0, #0                  ; return -1
        B       leave
finish
        CMP     r0, r1                  ; the packet must end exactly here
        BNE     fail
        LDR     r12, [sp, #8]
        STR     r6, [r12, #512]         ; save state->next for the next frame
        MOV     r0, #0                  ; return 0
leave
        ADD     sp, sp, #12
        LDMFD   sp!, {r4-r9, r11, pc}   ; restore registers and return

; SKIP: just move past the blocks.
op_skip
        ADD     r3, r3, r8
        ADD     r2, r2, r8, LSL #3      ; 8 bytes per block across
skip_wrap
        CMP     r3, #COLUMNS
        BLO     op_done
        SUB     r3, r3, #COLUMNS        ; past the row's end: wrap to the next
        ADD     r2, r2, #WRAP           ; block row (3 more pixel rows down)
        B       skip_wrap

; SOLID: fill the run with one color, a block row ("segment") at a time,
; writing four words (two blocks' width) per STM.
op_solid
        COLOR  r7, lr
        ORR     r7, r7, r7, LSL #16     ; the color in both halves of a word
        MOV     r9, r7
        MOV     r11, r7
        MOV     r12, r7
        STMFD   sp!, {r0, r1, r5, r6}   ; free four registers for the loop
so_seg
        RSB     r5, r3, #COLUMNS
        CMP     r5, r8
        MOVHI   r5, r8                  ; r5 = blocks in this block row
        MOV     lr, r2
        MOV     r6, #4                  ; 4 pixel rows per block
so_row
        MOV     r0, lr
        SUBS    r1, r5, #2
        BLT     so_rem
so_pair
        STMIA   r0!, {r7, r9, r11, r12} ; two blocks' row
        SUBS    r1, r1, #2
        BGE     so_pair
so_rem
        TST     r1, #1
        STMNEIA r0!, {r7, r9}           ; an odd block left over
        ADD     lr, lr, #STRIDE
        SUBS    r6, r6, #1
        BNE     so_row
        ADD     r2, r2, r5, LSL #3      ; move past this segment
        ADD     r3, r3, r5
        CMP     r3, #COLUMNS
        MOVEQ   r3, #0
        ADDEQ   r2, r2, #WRAP
        SUBS    r8, r8, r5
        BNE     so_seg
        LDMFD   sp!, {r0, r1, r5, r6}
        B       op_done

; GRAD: per block, two colors, their two mixes, and 32 bits of 2-bit
; indexes choosing among those four for each of the 16 pixels.
op_grad
        STR     r4, [sp, #-4]!          ; free r4 as scratch for MIX
gr_block
        COLOR  r7, lr                  ; a
        COLOR  r9, lr                  ; b
        AND     r11, r7, #0x1F          ; spread a and b into 10-bit lanes
        AND     lr, r7, #0x3E0
        ORR     r11, r11, lr, LSL #5
        AND     lr, r7, #0x7C00
        ORR     r11, r11, lr, LSL #10
        AND     r12, r9, #0x1F
        AND     lr, r9, #0x3E0
        ORR     r12, r12, lr, LSL #5
        AND     lr, r9, #0x7C00
        ORR     r12, r12, lr, LSL #10
        ADD     lr, r12, r11, LSL #1    ; 2a+b
        ADD     r12, r11, r12, LSL #1   ; 2b+a
        MIX     r11, lr, r4
        MIX     lr, r12, r4
        MOV     r12, lr
        STMFD   sp!, {r7, r9, r11, r12} ; palette a, b, mix(a,b), mix(b,a)
        LDRB    r7, [r0], #1            ; the 32 index bits, big-endian
        LDRB    lr, [r0], #1
        ORR     r7, lr, r7, LSL #8
        LDRB    lr, [r0], #1
        ORR     r7, lr, r7, LSL #8
        LDRB    lr, [r0], #1
        ORR     r7, lr, r7, LSL #8
        MOV     r12, #3                 ; the 2-bit index mask
        GROW    30, 0                   ; the block's four pixel rows
        GROW    22, STRIDE
        GROW    14, 2*STRIDE
        GROW    6, 3*STRIDE
        ADD     sp, sp, #16             ; drop the palette
        ADD     r2, r2, #8              ; next block
        ADD     r3, r3, #1
        CMP     r3, #COLUMNS
        MOVEQ   r3, #0
        ADDEQ   r2, r2, #WRAP
        SUBS    r8, r8, #1
        BNE     gr_block
        LDR     r4, [sp], #4
        B       op_done

; MOTION, one block: copy the 4x4 block at offset (dx, dy) from this one.
; No registers are saved. The rows go in an order that reads each source row
; before it could be overwritten: bottom-up when the source is above.
op_motion
        LDRB    r7, [r0], #1            ; the vector byte (dx + 8) << 4 | (dy + 8)
        CMP     r8, #MOTION_RUN
        BHI     fail
        CMP     r8, #1
        BNE     mo_run                  ; runs of several blocks are done below
        MOV     r9, r7, LSR #4
        SUB     r9, r9, #8              ; dx
        AND     r11, r7, #15
        SUB     r11, r11, #8            ; dy
        ADDS    r12, r9, r3, LSL #2     ; the source's left column...
        BMI     fail                    ; ...inside the frame
        CMP     r12, #WIDTH-4
        BGT     fail
        ADD     r12, r11, r11, LSL #2   ; dy * 5...
        MOV     r12, r12, LSL #STRIDE5  ; ...* 64 = dy * STRIDE
        MOV     lr, r9, ASR #1          ; dx / 2 words...
        ADD     r12, r12, lr, LSL #2    ; ...in bytes
        ADD     r12, r2, r12            ; source
        LDR     lr, [sp, #0]
        CMP     r12, lr                 ; the source's rows inside the frame
        BLO     fail
        LDR     lr, [sp, #4]
        CMP     r12, lr
        BHS     fail
        MOV     r7, r2                  ; r7 = destination
        MOV     r8, #STRIDE             ; r8 = row step: down...
        CMP     r11, #0
        RSBLT   r8, r8, #0              ; ...or up, starting at the last row,
        ADDLT   r7, r7, #3*STRIDE       ; when the source is above
        ADDLT   r12, r12, #3*STRIDE
        TST     r9, #1
        BNE     m1_odd
        LDMIA   r12, {r9, r11}          ; even dx: whole words, row by row
        STMIA   r7, {r9, r11}
        ADD     r12, r12, r8
        ADD     r7, r7, r8
        LDMIA   r12, {r9, r11}
        STMIA   r7, {r9, r11}
        ADD     r12, r12, r8
        ADD     r7, r7, r8
        LDMIA   r12, {r9, r11}
        STMIA   r7, {r9, r11}
        ADD     r12, r12, r8
        ADD     r7, r7, r8
        LDMIA   r12, {r9, r11}
        STMIA   r7, {r9, r11}
        B       m1_done
m1_odd                                  ; odd dx: each word joins two source halves
        LDMIA   r12, {r9, r11, lr}
        MOV     r9, r9, LSL #16
        ORR     r9, r9, r11, LSR #16
        MOV     r11, r11, LSL #16
        ORR     r11, r11, lr, LSR #16
        STMIA   r7, {r9, r11}
        ADD     r12, r12, r8
        ADD     r7, r7, r8
        LDMIA   r12, {r9, r11, lr}
        MOV     r9, r9, LSL #16
        ORR     r9, r9, r11, LSR #16
        MOV     r11, r11, LSL #16
        ORR     r11, r11, lr, LSR #16
        STMIA   r7, {r9, r11}
        ADD     r12, r12, r8
        ADD     r7, r7, r8
        LDMIA   r12, {r9, r11, lr}
        MOV     r9, r9, LSL #16
        ORR     r9, r9, r11, LSR #16
        MOV     r11, r11, LSL #16
        ORR     r11, r11, lr, LSR #16
        STMIA   r7, {r9, r11}
        ADD     r12, r12, r8
        ADD     r7, r7, r8
        LDMIA   r12, {r9, r11, lr}
        MOV     r9, r9, LSL #16
        ORR     r9, r9, r11, LSR #16
        MOV     r11, r11, LSL #16
        ORR     r11, r11, lr, LSR #16
        STMIA   r7, {r9, r11}
m1_done
        ADD     r2, r2, #8              ; next block
        ADD     r3, r3, #1
        CMP     r3, #COLUMNS
        MOVEQ   r3, #0
        ADDEQ   r2, r2, #WRAP
        B       op_done

; MOTION run: every block of the run copies from the same offset, taken
; from the frame as it was before the run. Done a block row ("segment") at a
; time as strips 4 pixel rows tall, choosing the copy direction so no source
; pixel is overwritten before it is read: rows bottom-up when the source is
; above, and right to left when it is directly to the left.
; Registers: r4 dx, r5 dy, r6 byte offset of the source, r9 strip blocks.
; [sp+20] and [sp+24] are the frame bounds once r0 r1 r4 r5 r6 are saved.
mo_run
        STMFD   sp!, {r0, r1, r4, r5, r6}
        MOV     r4, r7, LSR #4
        SUB     r4, r4, #8              ; dx
        AND     r5, r7, #15
        SUB     r5, r5, #8              ; dy
        ADD     r6, r5, r5, LSL #2
        MOV     r6, r6, LSL #STRIDE5    ; dy * STRIDE
        MOV     r7, r4, ASR #1
        ADD     r6, r6, r7, LSL #2      ; + dx / 2 words, in bytes
mo_seg
        RSB     r9, r3, #COLUMNS
        CMP     r9, r8
        MOVHI   r9, r8                  ; r9 = blocks in this strip
        ADDS    r7, r4, r3, LSL #2      ; first source column >= 0
        BMI     mo_fail
        ADD     r7, r7, r9, LSL #2      ; last source column + 4 <= WIDTH
        CMP     r7, #WIDTH
        BGT     mo_fail
        ADD     r1, r2, r6              ; r1 = source
        LDR     r7, [sp, #20]           ; source rows inside the frame
        CMP     r1, r7
        BLO     mo_fail
        LDR     r7, [sp, #24]
        CMP     r1, r7
        BHS     mo_fail
        STMFD   sp!, {r2-r6, r8, r9}
        MOV     r0, r2                  ; r0 = destination
        MOV     r2, r9, LSL #1          ; r2 = words per strip row
        MOV     r3, #STRIDE
        CMP     r5, #0
        BGT     mo_fwd                  ; source below: top-down
        BEQ     mo_flat
        RSB     r3, r3, #0              ; source above: rows bottom-up
        ADD     r0, r0, #3*STRIDE
        ADD     r1, r1, #3*STRIDE
        B       mo_fwd
mo_flat
        CMP     r4, #0                  ; source to the left: right to left
        BLT     mo_bwd
mo_fwd
        SUB     r3, r3, r2, LSL #2      ; r3 = step to the next row's start
        TST     r4, #1
        MOV     r4, #4                  ; r4 = rows left
        BNE     mo_fo_rows
mo_fa_row                               ; even dx: whole words, 4 at a time
        SUBS    r5, r2, #4
        BLT     mo_fa_rem
mo_fa_4
        LDMIA   r1!, {r6-r9}
        STMIA   r0!, {r6-r9}
        SUBS    r5, r5, #4
        BGE     mo_fa_4
mo_fa_rem
        TST     r5, #2                  ; then 2 left over (strips are whole blocks)
        LDMNEIA r1!, {r6, r7}
        STMNEIA r0!, {r6, r7}
        ADD     r0, r0, r3
        ADD     r1, r1, r3
        SUBS    r4, r4, #1
        BNE     mo_fa_row
        B       mo_next
mo_fo_rows                              ; odd dx: each word joins two source words
        SUB     r12, r3, #4             ; (the source reads one word ahead)
mo_fo_row
        LDR     r6, [r1], #4
        SUBS    r5, r2, #4
        BLT     mo_fo_rem
mo_fo_4
        LDMIA   r1!, {r7, r8, r9, r11}
        MOV     r6, r6, LSL #16
        ORR     r6, r6, r7, LSR #16
        MOV     r7, r7, LSL #16
        ORR     r7, r7, r8, LSR #16
        MOV     r8, r8, LSL #16
        ORR     r8, r8, r9, LSR #16
        MOV     r9, r9, LSL #16
        ORR     r9, r9, r11, LSR #16
        STMIA   r0!, {r6-r9}
        MOV     r6, r11                 ; carry the last word into the next four
        SUBS    r5, r5, #4
        BGE     mo_fo_4
mo_fo_rem
        TST     r5, #2
        BEQ     mo_fo_end
        LDMIA   r1!, {r7, r8}
        MOV     r6, r6, LSL #16
        ORR     r6, r6, r7, LSR #16
        MOV     r7, r7, LSL #16
        ORR     r7, r7, r8, LSR #16
        STMIA   r0!, {r6, r7}
mo_fo_end
        ADD     r0, r0, r3
        ADD     r1, r1, r12
        SUBS    r4, r4, #1
        BNE     mo_fo_row
        B       mo_next
mo_bwd                                  ; right to left, from each row's end
        ADD     r3, r3, r2, LSL #2      ; r3 = step to the next row's end
        ADD     r0, r0, r2, LSL #2
        ADD     r1, r1, r2, LSL #2
        TST     r4, #1
        MOV     r4, #4                  ; r4 = rows left
        BNE     mo_bo_row
mo_ba_row                               ; even dx: whole words, 4 at a time
        SUBS    r5, r2, #4
        BLT     mo_ba_rem
mo_ba_4
        LDMDB   r1!, {r6-r9}
        STMDB   r0!, {r6-r9}
        SUBS    r5, r5, #4
        BGE     mo_ba_4
mo_ba_rem
        TST     r5, #2
        LDMNEDB r1!, {r6, r7}
        STMNEDB r0!, {r6, r7}
        ADD     r0, r0, r3
        ADD     r1, r1, r3
        SUBS    r4, r4, #1
        BNE     mo_ba_row
        B       mo_next
mo_bo_row                               ; odd dx, right to left
        LDR     r6, [r1]
        SUBS    r5, r2, #4
        BLT     mo_bo_rem
mo_bo_4
        LDMDB   r1!, {r7, r8, r9, r11}
        MOV     lr, r6, LSR #16
        ORR     lr, lr, r11, LSL #16
        MOV     r11, r11, LSR #16
        ORR     r11, r11, r9, LSL #16
        MOV     r9, r9, LSR #16
        ORR     r9, r9, r8, LSL #16
        MOV     r8, r8, LSR #16
        ORR     r8, r8, r7, LSL #16
        STMDB   r0!, {r8, r9, r11, lr}
        MOV     r6, r7                  ; carry the first word into the next four
        SUBS    r5, r5, #4
        BGE     mo_bo_4
mo_bo_rem
        TST     r5, #2
        BEQ     mo_bo_end
        LDMDB   r1!, {r7, r8}
        MOV     r9, r6, LSR #16
        ORR     r9, r9, r8, LSL #16
        MOV     r8, r8, LSR #16
        ORR     r8, r8, r7, LSL #16
        STMDB   r0!, {r8, r9}
mo_bo_end
        ADD     r0, r0, r3
        ADD     r1, r1, r3
        SUBS    r4, r4, #1
        BNE     mo_bo_row
mo_next                                 ; the next strip of the run
        LDMFD   sp!, {r2-r6, r8, r9}
        ADD     r2, r2, r9, LSL #3
        ADD     r3, r3, r9
        CMP     r3, #COLUMNS
        MOVEQ   r3, #0
        ADDEQ   r2, r2, #WRAP
        SUBS    r8, r8, r9
        BNE     mo_seg
        LDMFD   sp!, {r0, r1, r4, r5, r6}
        B       op_done
mo_fail
        LDMFD   sp!, {r0, r1, r4, r5, r6}
        B       fail

        LTORG                           ; constants used by LDR rX, =value
        END
