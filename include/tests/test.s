.section .text

    addi x5, x0, 5
    addi x6, x5, 3      # depends on x5 -> forwarding required
    beq  x6, x5, label  # branch not taken

    addi x7, x0, 1
    jal  x0, end

label:
    addi x7, x0, 99

end:
    addi x0, x0, 0

