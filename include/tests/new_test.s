    addi x1, x0, 5          # INT ops
    addi x2, x0, 10

# RAW chain (forwarding tests)
    add  x3, x1, x2
    add  x4, x3, x1
    add  x5, x4, x3

# Load-use hazard
    lw   x6, 0(x3)
    add  x7, x6, x1

# Store-load
    sw   x4, 4(x0)
    lw   x8, 4(x0)

# Branch predictor test: always taken
    beq  x1, x1, L1
    addi x9, x0, 111

L1:
    addi x9, x0, 222

# Mispredict branch (usually)
    beq  x5, x0, WRONG
    addi x10, x0, 333
    jal  x0, L2

WRONG:
    addi x10, x0, 444

L2:
    addi x11, x10, 1

# Loop for BTB testing
L_LOOP:
    addi x12, x12, 1
    blt  x12, x2, L_LOOP


    # Initial FP loads
    flw  f1, 0(x0)          # assume mem[0] = 1.5
    flw  f2, 4(x0)          # assume mem[4] = 2.5
    flw  f3, 8(x0)          # assume mem[8] = -3.0

    fadd.s f4, f1, f2       # f4 = 1.5 + 2.5 = 4.0
    fmul.s f5, f4, f2       # f5 = 4.0 * 2.5 = 10.0

# Mixed INT-FP dependency
    addi x13, x0, 8
    fsw  f5, 12(x13)




# -------- fmadd.s ( f7 = f1*f2 + f3 ) --------
    fmadd.s f7, f1, f2, f3

# -------- fmsub.s ( f8 = f1*f2 - f3 ) --------
    fmsub.s f8, f1, f2, f3

# -------- fnmsub.s ( f9 = -f1*f2 + f3 ) --------
    fnmsub.s f9, f1, f2, f3

# -------- fnmadd.s ( f10 = -f1*f2 - f3 ) --------
    fnmadd.s f10, f1, f2, f3

# FP branch based on integer value
    addi x14, x0, 1
    beq  x14, x14, FP_TAKEN
    addi x15, x0, 999

FP_TAKEN:
    addi x15, x0, 777

# Final combined loop (INT+FP)
COMB_LOOP:
    lw     x16, 0(x3)
    fadd.s f11, f4, f7
    addi   x17, x17, 1
    blt    x17, x1, COMB_LOOP

# End with NOP
    addi x0, x0, 0