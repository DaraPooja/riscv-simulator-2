# lui   x5, 0x10000           # Base = 0x10000000
    
#     # Store 4.0 = 0x40800000
#     lui   x10, 0x40800
#     sw    x10, 0(x5)
    
#     # Store 2.0 = 0x40000000
#     lui   x11, 0x40000
#     sw    x11, 4(x5)
    
#     # Load
#     flw   f1, 0(x5)             # f1 = 4.0
#     flw   f2, 4(x5)             # f2 = 2.0
    
#     # Compare: is 2.0 < 4.0?
#     flt.s  x6, f2, f1           # x6 = 1 (true)
#     beq    x6, x0, 8            # Skip 2 instructions if false
    
#     # TRUE path: 4.0 + 2.0 = 6.0
#     fadd.s f3, f1, f2
#     fsw    f3, 8(x5)            # Store 0x40C00000
#     jal    x0, 8                # Jump over else path (2 instrs)
    
#     # FALSE path (should NOT execute)
#     fsub.s f3, f1, f2
#     fsw    f3, 8(x5)
    
#     # Done: store counter
#     addi   x7, x0, 100
#     sw     x7, 12(x5)

# File: test_fp_dependencies.s
# Test: RAW hazards with FP operations

    addi x10, x0, 0
    addi x11, x0, 64
    
    # Store 1.0
    lui x5, 0x3F800           # 1.0
    sw x5, 0(x10)
    
    # Load and create dependency chain
    flw f0, 0(x10)            # f0 = 1.0
    fadd.s f1, f0, f0         # f1 = 1.0 + 1.0 = 2.0 (RAW on f0)
    fmul.s f2, f1, f1         # f2 = 2.0 * 2.0 = 4.0 (RAW on f1)
    fadd.s f3, f2, f2         # f3 = 4.0 + 4.0 = 8.0 (RAW on f2)
    fmul.s f4, f3, f3         # f4 = 8.0 * 8.0 = 64.0 (RAW on f3)
    
    # Store final result
    fsw f4, 0(x11)            # Should be 0x42800000 (64.0)
