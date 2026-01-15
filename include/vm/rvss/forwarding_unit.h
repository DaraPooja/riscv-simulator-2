#ifndef FORWARDING_UNIT_H
#define FORWARDING_UNIT_H
#include <cstdint>

class ForwardingUnit {
public:
    // Outputs
    // 00 = no forward, 10 = from EX/MEM, 01 = from MEM/WB
    uint8_t ForwardA; // for rs1
    uint8_t ForwardB; // for rs2
    uint8_t ForwardC; // for rs3 (fmadd family)

    ForwardingUnit();
    void Reset();
    void Update(
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
    );
};

#endif // FORWARDING_UNIT_H