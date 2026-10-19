; OM3DVID5 frame decoder for the ARM60 (see preview_v5.h for the format).
; int preview_v5_decode(PreviewV5 *state, const unsigned char *data,
;                       unsigned long bytes, unsigned char *pixels)
; Returns 0, or -1 for a damaged packet. Matches preview_v5.c bit for bit.
;
; Main loop registers:
;   r0 data   r1 end   r2 destination (top-left word of the current block)
;   r3 block column (0-79)   r4 blocks left   r5 colour cache   r6 next slot
;   r8 blocks left in the current run; r7 r9 r11 r12 lr scratch.
; r10 (the APCS stack limit) is never touched. Runs of MOTION and SOLID are
; drawn a block row at a time as strips, with LDM/STM.

        AREA    |C$$code|, CODE, READONLY
        EXPORT  preview_v5_decode

WIDTH   EQU     160
HEIGHT  EQU     120
STRIDE  EQU     WIDTH*2         ; bytes per pixel row
STRIDE5 EQU     6               ; STRIDE = 5 << STRIDE5
WRAP    EQU     3*STRIDE        ; destination step at the end of a block row
COLUMNS EQU     WIDTH/4
BLOCKS  EQU     COLUMNS*HEIGHT/4
MOTION_RUN EQU  32              ; longest MOTION run (PREVIEW_V5_MOTION_RUN)

; One colour code into $r (an RGB555 value); $t is scratch.
        MACRO
        COLOUR  $r, $t
        LDRB    $r, [r0], #1
        TST     $r, #0x80
        BICNE   $r, $r, #0x80
        LDRNE   $r, [r5, $r, LSL #2]
        LDREQB  $t, [r0], #1
        ORREQ   $r, $t, $r, LSL #8
        STREQ   $r, [r5, r6, LSL #2]
        ADDEQ   r6, r6, #1
        ANDEQ   r6, r6, #127
        MEND

; PREVIEW_V5_MIX from $x = 2*spread(a)+spread(b) (10-bit lanes) into $d.
        MACRO
        MIX     $d, $x, $t
        ADD     $t, $x, $x, LSL #2
        ADD     $x, $x, $t, LSL #1      ; 11x: lanes stay below 1024
        MOV     $d, $x, LSR #5
        AND     $d, $d, #0x1F
        MOV     $t, $x, LSR #10
        AND     $t, $t, #0x3E0
        ORR     $d, $d, $t
        MOV     $t, $x, LSR #15
        AND     $t, $t, #0x7C00
        ORR     $d, $d, $t
        MEND

; One row of a GRAD block: indexes at bit $s of r7, palette on the stack.
        MACRO
        GROW    $s, $off
        AND     r9, r12, r7, LSR #$s
        LDR     r9, [sp, r9, LSL #2]
        AND     r11, r12, r7, LSR #$s-2
        LDR     r11, [sp, r11, LSL #2]
        ORR     r9, r11, r9, LSL #16
        AND     r11, r12, r7, LSR #$s-4
        LDR     r11, [sp, r11, LSL #2]
        IF $s = 6
        AND     lr, r12, r7
        ELSE
        AND     lr, r12, r7, LSR #$s-6
        ENDIF
        LDR     lr, [sp, lr, LSL #2]
        ORR     r11, lr, r11, LSL #16
        STR     r9, [r2, #$off]
        STR     r11, [r2, #$off+4]
        MEND

preview_v5_decode
        STMFD   sp!, {r4-r9, r11, lr}
        SUB     sp, sp, #12
        STR     r3, [sp, #0]            ; [sp] frame start
        LDR     r12, =(HEIGHT-3)*STRIDE
        ADD     r12, r3, r12
        STR     r12, [sp, #4]           ; [sp+4] no source block starts on or after this row
        STR     r0, [sp, #8]            ; [sp+8] state
        MOV     r5, r0
        LDR     r6, [r5, #512]
        ADD     r12, r1, r2
        MOV     r0, r1
        MOV     r1, r12
        MOV     r2, r3
        MOV     r3, #0
        MOV     r4, #BLOCKS
next_op
        CMP     r4, #0
        BEQ     finish
        LDRB    r7, [r0], #1            ; past the end only into the read-ahead
        AND     r8, r7, #63
        ADD     r8, r8, #1
        SUBS    r4, r4, r8
        BMI     fail
        MOV     r7, r7, LSR #6
        ADD     pc, pc, r7, LSL #2
        MOV     r0, r0
        B       op_skip
        B       op_motion
        B       op_solid
        B       op_grad
op_done
        CMP     r0, r1                  ; overruns are caught after the run
        BLS     next_op
fail
        MVN     r0, #0
        B       leave
finish
        CMP     r0, r1
        BNE     fail
        LDR     r12, [sp, #8]
        STR     r6, [r12, #512]
        MOV     r0, #0
leave
        ADD     sp, sp, #12
        LDMFD   sp!, {r4-r9, r11, pc}

op_skip
        ADD     r3, r3, r8
        ADD     r2, r2, r8, LSL #3
skip_wrap
        CMP     r3, #COLUMNS
        BLO     op_done
        SUB     r3, r3, #COLUMNS
        ADD     r2, r2, #WRAP
        B       skip_wrap

op_solid
        COLOUR  r7, lr
        ORR     r7, r7, r7, LSL #16
        MOV     r9, r7
        MOV     r11, r7
        MOV     r12, r7
        STMFD   sp!, {r0, r1, r5, r6}
so_seg
        RSB     r5, r3, #COLUMNS
        CMP     r5, r8
        MOVHI   r5, r8                  ; r5 = blocks in this block row
        MOV     lr, r2
        MOV     r6, #4
so_row
        MOV     r0, lr
        SUBS    r1, r5, #2
        BLT     so_rem
so_pair
        STMIA   r0!, {r7, r9, r11, r12}
        SUBS    r1, r1, #2
        BGE     so_pair
so_rem
        TST     r1, #1
        STMNEIA r0!, {r7, r9}
        ADD     lr, lr, #STRIDE
        SUBS    r6, r6, #1
        BNE     so_row
        ADD     r2, r2, r5, LSL #3
        ADD     r3, r3, r5
        CMP     r3, #COLUMNS
        MOVEQ   r3, #0
        ADDEQ   r2, r2, #WRAP
        SUBS    r8, r8, r5
        BNE     so_seg
        LDMFD   sp!, {r0, r1, r5, r6}
        B       op_done

op_grad
        STR     r4, [sp, #-4]!
gr_block
        COLOUR  r7, lr                  ; a
        COLOUR  r9, lr                  ; b
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
        LDRB    r7, [r0], #1
        LDRB    lr, [r0], #1
        ORR     r7, lr, r7, LSL #8
        LDRB    lr, [r0], #1
        ORR     r7, lr, r7, LSL #8
        LDRB    lr, [r0], #1
        ORR     r7, lr, r7, LSL #8
        MOV     r12, #3
        GROW    30, 0
        GROW    22, STRIDE
        GROW    14, 2*STRIDE
        GROW    6, 3*STRIDE
        ADD     sp, sp, #16
        ADD     r2, r2, #8
        ADD     r3, r3, #1
        CMP     r3, #COLUMNS
        MOVEQ   r3, #0
        ADDEQ   r2, r2, #WRAP
        SUBS    r8, r8, #1
        BNE     gr_block
        LDR     r4, [sp], #4
        B       op_done

; MOTION, one block: no registers saved; rows go in an order that reads
; each source row before it can be overwritten.
op_motion
        LDRB    r7, [r0], #1
        CMP     r8, #MOTION_RUN
        BHI     fail
        CMP     r8, #1
        BNE     mo_run
        MOV     r9, r7, LSR #4
        SUB     r9, r9, #8              ; dx
        AND     r11, r7, #15
        SUB     r11, r11, #8            ; dy
        ADDS    r12, r9, r3, LSL #2
        BMI     fail
        CMP     r12, #WIDTH-4
        BGT     fail
        ADD     r12, r11, r11, LSL #2
        MOV     r12, r12, LSL #STRIDE5
        MOV     lr, r9, ASR #1
        ADD     r12, r12, lr, LSL #2
        ADD     r12, r2, r12            ; source
        LDR     lr, [sp, #0]
        CMP     r12, lr
        BLO     fail
        LDR     lr, [sp, #4]
        CMP     r12, lr
        BHS     fail
        MOV     r7, r2
        MOV     r8, #STRIDE
        CMP     r11, #0
        RSBLT   r8, r8, #0
        ADDLT   r7, r7, #3*STRIDE
        ADDLT   r12, r12, #3*STRIDE
        TST     r9, #1
        BNE     m1_odd
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
        ADD     r12, r12, r8
        ADD     r7, r7, r8
        LDMIA   r12, {r9, r11}
        STMIA   r7, {r9, r11}
        B       m1_done
m1_odd
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
        ADD     r2, r2, #8
        ADD     r3, r3, #1
        CMP     r3, #COLUMNS
        MOVEQ   r3, #0
        ADDEQ   r2, r2, #WRAP
        B       op_done

; MOTION run: r4 dx, r5 dy, r6 byte offset of the source, r9 strip blocks.
; [sp+20] and [sp+24] are the frame bounds once r0 r1 r4 r5 r6 are saved.
mo_run
        STMFD   sp!, {r0, r1, r4, r5, r6}
        MOV     r4, r7, LSR #4
        SUB     r4, r4, #8
        AND     r5, r7, #15
        SUB     r5, r5, #8
        ADD     r6, r5, r5, LSL #2
        MOV     r6, r6, LSL #STRIDE5    ; dy * STRIDE
        MOV     r7, r4, ASR #1
        ADD     r6, r6, r7, LSL #2
mo_seg
        RSB     r9, r3, #COLUMNS
        CMP     r9, r8
        MOVHI   r9, r8
        ADDS    r7, r4, r3, LSL #2      ; first source column >= 0
        BMI     mo_fail
        ADD     r7, r7, r9, LSL #2      ; last source column + 4 <= WIDTH
        CMP     r7, #WIDTH
        BGT     mo_fail
        ADD     r1, r2, r6
        LDR     r7, [sp, #20]           ; source rows inside the frame
        CMP     r1, r7
        BLO     mo_fail
        LDR     r7, [sp, #24]
        CMP     r1, r7
        BHS     mo_fail
        STMFD   sp!, {r2-r6, r8, r9}
        MOV     r0, r2
        MOV     r2, r9, LSL #1          ; r2 = words per strip row
        MOV     r3, #STRIDE
        CMP     r5, #0
        BGT     mo_fwd
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
        MOV     r4, #4
        BNE     mo_fo_rows
mo_fa_row
        SUBS    r5, r2, #4
        BLT     mo_fa_rem
mo_fa_4
        LDMIA   r1!, {r6-r9}
        STMIA   r0!, {r6-r9}
        SUBS    r5, r5, #4
        BGE     mo_fa_4
mo_fa_rem
        TST     r5, #2
        LDMNEIA r1!, {r6, r7}
        STMNEIA r0!, {r6, r7}
        ADD     r0, r0, r3
        ADD     r1, r1, r3
        SUBS    r4, r4, #1
        BNE     mo_fa_row
        B       mo_next
mo_fo_rows                              ; odd dx: each word joins two source words
        SUB     r12, r3, #4
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
        MOV     r6, r11
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
mo_bwd
        ADD     r3, r3, r2, LSL #2      ; r3 = step to the next row's end
        ADD     r0, r0, r2, LSL #2
        ADD     r1, r1, r2, LSL #2
        TST     r4, #1
        MOV     r4, #4
        BNE     mo_bo_row
mo_ba_row
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
mo_bo_row
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
        MOV     r6, r7
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
mo_next
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

        LTORG
        END
