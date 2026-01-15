

    # Set up base address for data storage
    lui x10, 0x10000          # x10 = 0x10000000 (data base)
    lui x11, 0x10001          # x11 = 0x10001000 (output base)
    

    
    # Store 2.0 (0x40000000) at address 0x10000000
    lui x12, 0x40000          # x12 = 0x40000000
    sw x12, 0(x10)
    
    # Store 3.0 (0x40400000) at address 0x10000004
    lui x12, 0x40400          # x12 = 0x40400000
    sw x12, 4(x10)
    
    # Store 5.0 (0x40A00000) at address 0x10000008
    lui x12, 0x40A00          # x12 = 0x40A00000
    sw x12, 8(x10)

    flw f1, 0(x10)            # f1 = 2.0
    flw f2, 4(x10)            # f2 = 3.0
    flw f3, 8(x10)            # f3 = 5.0
    
    # f4 = (f1 * f2) + f3
    # f4 = (2.0 * 3.0) + 5.0 = 6.0 + 5.0 = 11.0
    fmadd.s f4, f1, f2, f3
    

    fsw f4, 0(x11)            # Store result at 0x10001000

    addi x0, x0, 0            