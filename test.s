# hazard_test.s
# This program demonstrates a data hazard
# Without hazard handling, the result will be incorrect

    li x1, 5       # x1 = 5
    li x2, 10      # x2 = 10

    add x3, x1, x2 # x3 = x1 + x2 = 15
    add x4, x3, x2 # x4 = x3 + x2
    add x5, x4, x3 # x5 = x4 + x3 

    # Done

