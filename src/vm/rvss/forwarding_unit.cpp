#include "vm/rvss/forwarding_unit.h"
#include <iostream>

ForwardingUnit::ForwardingUnit() {
    Reset();
}

void ForwardingUnit::Reset() {
    ForwardA = 0;
    ForwardB = 0;
    ForwardC = 0;
}

void ForwardingUnit::Update(
    bool ex_mem_regwrite,
    bool ex_mem_fp_write,
    bool ex_mem_mem_read,
    uint32_t ex_mem_rd,

    bool mem_wb_regwrite,
    bool mem_wb_fp_write,
    uint32_t mem_wb_rd,
    uint32_t id_ex_rs1,
    uint32_t id_ex_rs2,
    uint32_t id_ex_rs3,
    bool id_ex_uses_rs1,
    bool id_ex_uses_rs2,
    bool id_ex_uses_rs3,
    bool id_ex_alu_src
) {
    
    // Start with no forwarding, and update only when needed.
    ForwardA = 0;
    ForwardB = 0;
    ForwardC = 0;

    // This helper checks if a GPR value can be forwarded to the current instruction.
    auto try_forward_gpr = [&](uint32_t src_reg, bool uses, uint8_t &out_forward) {
        if (!uses) {
            out_forward = 0;
            return;
        }
        
        // Integer register x0 always holds zero, so there is nothing to forward.
        if (src_reg == 0) {
            out_forward = 0;
            return;
        }
        
        // Forwarding from EX/MEM has higher priority than forwarding from MEM/WB.
        // For integer loads, data is not available yet in EX/MEM, so avoid forwarding for loads here.
        if (ex_mem_regwrite && !ex_mem_fp_write && ex_mem_rd != 0 && 
            ex_mem_rd == src_reg && !ex_mem_mem_read) {
            out_forward = 0b10;   // Forward from EX/MEM
            return;
        }
        
        // If the value is not in EX/MEM, check whether it is being written in MEM/WB.
        // This path covers loads which finish in this stage.
        if (mem_wb_regwrite && !mem_wb_fp_write && mem_wb_rd != 0 && mem_wb_rd == src_reg) {
            out_forward = 0b01;   // Forward from MEM/WB
            return;
        }
        
        out_forward = 0;
    };

    // This helper checks forwarding for FP registers.
    auto try_forward_fp = [&](uint32_t src_reg, bool uses, uint8_t &out_forward) {
        if (!uses) {
            out_forward = 0;
            return;
        }
        
        // Unlike integer register x0, f0 is a valid floating-point register,
        // so forwarding is allowed for register 0 here.

        // Try forwarding from EX/MEM when the result belongs to an FP instruction.
        // FP loads still cannot be forwarded from EX/MEM because data is not ready yet.
        if (ex_mem_regwrite && ex_mem_fp_write && ex_mem_rd == src_reg && !ex_mem_mem_read) {
            out_forward = 0b10;   // Forward from EX/MEM
            return;
        }
        
        // If the value is ready in MEM/WB, forward from there.
        if (mem_wb_regwrite && mem_wb_fp_write && mem_wb_rd == src_reg) {
            out_forward = 0b01;   // Forward from MEM/WB
            return;
        }
        
        out_forward = 0;
    };

    // Forwarding logic for rs1.
    // rs1 might be an integer or floating-point register, so try both.
    try_forward_gpr(id_ex_rs1, id_ex_uses_rs1, ForwardA);
    if (ForwardA == 0) {
        try_forward_fp(id_ex_rs1, id_ex_uses_rs1, ForwardA);
    }

    // Forwarding logic for rs2.
    try_forward_gpr(id_ex_rs2, id_ex_uses_rs2, ForwardB);
    if (ForwardB == 0) {
        try_forward_fp(id_ex_rs2, id_ex_uses_rs2, ForwardB);
    }

    // Forwarding logic for rs3, which is used by FMADD-family instructions.
    try_forward_gpr(id_ex_rs3, id_ex_uses_rs3, ForwardC);
    if (ForwardC == 0) {
        try_forward_fp(id_ex_rs3, id_ex_uses_rs3, ForwardC);
    }

    // Print debug information only when forwarding is actually applied.
    if (ForwardA) {
        std::cout << "  [ForwardingUnit] ForwardA=" << (int)ForwardA 
                  << " for rs1=" << id_ex_rs1 << std::endl;
    }
    if (ForwardB) {
        std::cout << "  [ForwardingUnit] ForwardB=" << (int)ForwardB 
                  << " for rs2=" << id_ex_rs2 << std::endl;
    }
    if (ForwardC) {
        std::cout << "  [ForwardingUnit] ForwardC=" << (int)ForwardC 
                  << " for rs3=" << id_ex_rs3 << std::endl;
    }
}
