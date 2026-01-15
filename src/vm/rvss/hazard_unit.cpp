#include "vm/rvss/hazard_unit.h"
#include "vm/rvss/rvss_pipeline_vm.h"
#include "common/instructions.h"
#include <iostream>

using instruction_set::isFInstruction;
using instruction_set::isDInstruction;

// Returns "f" for floating-point registers and "x" for integer registers.
static inline const char* reg_prefix(bool is_fp) {
    return is_fp ? "f" : "x";
}
 
// Checks whether an instruction is a floating-point instruction.
// This includes both the regular FP opcodes and the FMADD family.
static inline bool is_fp_instruction_check(uint32_t instr) {
    uint8_t opcode = instr & 0x7F;
    
    // FMADD family opcodes always correspond to floating-point instructions.
    if (opcode == 0x43 || opcode == 0x47 || opcode == 0x4B || opcode == 0x4F) {
        return true;
    }
    
    // For other opcodes, rely on the instruction set helpers.
    bool is_f = isFInstruction(instr);
    bool is_d = isDInstruction(instr);
    
    return is_f || is_d;
}

void HazardUnit::Reset() {
    stall_IF_ = false;
    stall_ID_ = false;
    stalled_ = false;
}

void HazardUnit::Flush() {
    stall_IF_ = false;
    stall_ID_ = false;
    stalled_ = false;
}

// Helpers to classify opcodes into floating-point and integer load/store types.

static inline bool opcode_is_fp_rtype(uint8_t op) {
    return op == 0x53; // Standard FP R-type operations (for example, fadd.s/fadd.d).
}

static inline bool opcode_is_fmadd_family(uint8_t op) {
    // Opcodes for fmadd/fmsub/fnmadd/fnmsub (single and double precision families).
    return op == 0x43 || op == 0x47 || op == 0x4B || op == 0x4F;
}

static inline bool opcode_is_fp_load(uint8_t op) {
    return op == 0x07; // flw/fld (loads into an FP register).
}

static inline bool opcode_is_fp_store(uint8_t op) {
    return op == 0x27; // fsw/fsd (stores from an FP register).
}

static inline bool opcode_is_gpr_load(uint8_t op) {
    return op == 0x03; // lb/lh/lw/ld (loads into an integer register).
}

static inline bool opcode_is_gpr_store(uint8_t op) {
    return op == 0x23; // sb/sh/sw/sd (stores from an integer register).
}

// Checks whether the given FP instruction writes its result into a GPR (x-reg).
// Some FP instructions, such as comparisons and conversions, produce integer results.
static inline bool fp_instr_writes_to_gpr(uint32_t instr) {
    uint8_t funct7 = (instr >> 25) & 0x7F;

    // FP comparisons (feq, flt, fle) write to an integer register.
    // FP to integer conversions (fcvt.w.s, fcvt.l.s, etc.) write to an integer register.
    // Moves from FP to integer (fmv.x.w, fmv.x.d) also write to an integer register.
    // fclass instructions write to an integer register as well.
    if (funct7 == 0b1010000 ||  // feq.s/flt.s/fle.s (single precision)
        funct7 == 0b1010001 ||  // feq.d/flt.d/fle.d (double precision)
        funct7 == 0b1100000 ||  // fcvt.w.s/fcvt.wu.s/fcvt.l.s/fcvt.lu.s
        funct7 == 0b1100001 ||  // fcvt.w.d/fcvt.wu.d/fcvt.l.d/fcvt.lu.d
        funct7 == 0b1110000 ||  // fmv.x.w/fclass.s
        funct7 == 0b1110001) {  // fmv.x.d/fclass.d
        return true;
    }
    return false;
}

// Checks whether the FP instruction uses a GPR as its rs1 source.
// This happens for integer-to-FP conversions and moves from integer registers.
static inline bool fp_instr_reads_gpr_rs1(uint32_t instr) {
    uint8_t funct7 = (instr >> 25) & 0x7F;

    // Integer to FP conversions (fcvt.s.w etc.) read from a GPR.
    // Moves from integer to FP (fmv.w.x, fmv.d.x) also read from a GPR.
    if (funct7 == 0b1101000 ||  // fcvt.s.w/fcvt.s.wu/fcvt.s.l/fcvt.s.lu
        funct7 == 0b1101001 ||  // fcvt.d.w/fcvt.d.wu/fcvt.d.l/fcvt.d.lu
        funct7 == 0b1111000 ||  // fmv.w.x
        funct7 == 0b1111001) {  // fmv.d.x
        return true;
    }
    return false;
}

// Main data hazard detection function.
// This checks for load-use hazards and general RAW hazards across the pipeline stages.
bool HazardUnit::CheckDataHazard() {
    // Clear flags for this check.
    stall_IF_ = stall_ID_ = stalled_ = false;
    
    if (!vm_) return false;
    
    auto &if_id = vm_->if_id_;
    auto &id_ex = vm_->id_ex_;
    auto &ex_mem = vm_->ex_mem_;
    auto &mem_wb = vm_->mem_wb_;
    
    if (!if_id.valid) return false;
    
    // Determine whether the IF/ID instruction is floating-point.
    bool if_id_is_fp = is_fp_instruction_check(if_id.instr);
    
    // Determine whether the ID/EX instruction behaves as an FP producer.
    bool id_ex_is_fp = false;
    if (id_ex.valid) {
        id_ex_is_fp = is_fp_instruction_check(id_ex.instr);
        
        // Some FP encodings actually write into GPRs; treat those as integer producers.
        if (id_ex_is_fp) {
            uint8_t funct7 = (id_ex.instr >> 25) & 0x7F;
            if (funct7 == 0b1100000 || funct7 == 0b1100001 ||
                funct7 == 0b1110000 || funct7 == 0b1110001 ||
                funct7 == 0b1010000 || funct7 == 0b1010001) {
                id_ex_is_fp = false;
            }
        }
    }
    
    std::cout << "  [HazardCheck] id_ex.valid=" << id_ex.valid 
              << " mem_read=" << id_ex.mem_read 
              << " rd=" << reg_prefix(id_ex_is_fp) << (int)id_ex.rd << std::endl;

    // Decode source register fields from the IF/ID instruction.
    uint32_t instr = if_id.instr;
    uint8_t opcode = instr & 0x7F;
    uint8_t funct7 = (instr >> 25) & 0x7F;

    uint8_t g_rs1 = (instr >> 15) & 0x1F;
    uint8_t g_rs2 = (instr >> 20) & 0x1F;
    uint8_t f_rs1 = (instr >> 15) & 0x1F;
    uint8_t f_rs2 = (instr >> 20) & 0x1F;
    uint8_t f_rs3 = (instr >> 27) & 0x1F;

    // These flags describe which sources are actually used by the IF/ID instruction.
    bool uses_gpr_rs1 = false;
    bool uses_gpr_rs2 = false;
    bool uses_fp_rs1  = false;
    bool uses_fp_rs2  = false;
    bool uses_fp_rs3  = false;

    // Integer instruction classes and their source usage.
    if (opcode == 0x33 || opcode == 0x3B) {
        // Integer R-type.
        uses_gpr_rs1 = true;
        uses_gpr_rs2 = true;
    } else if (opcode == 0x13 || opcode == 0x1B) {
        // Integer I-type ALU.
        uses_gpr_rs1 = true;
    } else if (opcode == 0x03) {
        // Integer load.
        uses_gpr_rs1 = true;
    } else if (opcode == 0x23) {
        // Integer store.
        uses_gpr_rs1 = true;
        uses_gpr_rs2 = true;
    } else if (opcode == 0x63) {
        // Branch instruction.
        uses_gpr_rs1 = true;
        uses_gpr_rs2 = true;
    } else if (opcode == 0x67) {
        // JALR.
        uses_gpr_rs1 = true;
    }

    // Floating-point instruction classes and their source usage.
    if (opcode_is_fp_rtype(opcode)) {
        // FP R-type can either use FP registers or read rs1 from a GPR for conversions.
        if (fp_instr_reads_gpr_rs1(instr)) {
            uses_gpr_rs1 = true;
        } else {
            uses_fp_rs1 = true;
            uses_fp_rs2 = true;
        }
    }
    
    if (opcode_is_fmadd_family(opcode)) {
        // FMADD-family instructions use three FP source registers.
        uses_fp_rs1 = true;
        uses_fp_rs2 = true;
        uses_fp_rs3 = true;
    }
    
    if (opcode_is_fp_load(opcode)) {
        // FP load uses a GPR base address.
        uses_gpr_rs1 = true;
    }
    
    if (opcode_is_fp_store(opcode)) {
        // FP store uses a GPR base address and an FP source value.
        uses_gpr_rs1 = true;
        uses_fp_rs2  = true;
    }

    // Load-use hazard where the producer is the instruction in ID/EX.
    // The consumer is the current IF/ID instruction.
    if (id_ex.valid && id_ex.mem_read) {
        uint8_t producer_rd = id_ex.rd;
        uint8_t prod_opcode = id_ex.opcode;
        
        // For integer loads, x0 as a destination can be ignored.
        bool skip_rd_zero = (opcode_is_gpr_load(prod_opcode) && producer_rd == 0);
        
        if (!skip_rd_zero) {
            // FP load followed by an instruction that needs the FP result.
            if (opcode_is_fp_load(prod_opcode)) {
                if ((uses_fp_rs1 && producer_rd == f_rs1) ||
                    (uses_fp_rs2 && producer_rd == f_rs2) ||
                    (uses_fp_rs3 && producer_rd == f_rs3)) {
                    std::cout << "  [HazardUnit] Load-use hazard (FP load from ID/EX)!"
                              << " Load to f" << (int)producer_rd
                              << " needed by instr at PC=0x" << std::hex << if_id.pc << std::dec << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }
            
            // Integer load followed by an instruction that needs the integer result.
            if (opcode_is_gpr_load(prod_opcode)) {
                if ((uses_gpr_rs1 && producer_rd == g_rs1) ||
                    (uses_gpr_rs2 && producer_rd == g_rs2)) {
                    std::cout << "  [HazardUnit] Load-use hazard (GPR load from ID/EX)!"
                              << " Load to x" << (int)producer_rd
                              << " needed by instr at PC=0x" << std::hex << if_id.pc << std::dec << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }
        }
    }

    // Load hazard where the producer is the instruction in EX/MEM.
    // The data will only become available in MEM/WB.
    if (ex_mem.valid && ex_mem.mem_read) {
        uint8_t producer_rd = ex_mem.rd;
        uint8_t prod_opcode = ex_mem.opcode;
        
        bool skip_rd_zero = (opcode_is_gpr_load(prod_opcode) && producer_rd == 0);
        
        if (!skip_rd_zero) {
            // FP load result still in flight.
            if (opcode_is_fp_load(prod_opcode)) {
                if ((uses_fp_rs1 && producer_rd == f_rs1) ||
                    (uses_fp_rs2 && producer_rd == f_rs2) ||
                    (uses_fp_rs3 && producer_rd == f_rs3)) {
                    std::cout << "  [HazardUnit] Load hazard (FP load from EX/MEM)!"
                              << " Load to f" << (int)producer_rd
                              << " needed by instr at PC=0x" << std::hex << if_id.pc << std::dec 
                              << " - data not ready until MEM/WB" << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }
            
            // Integer load result still in flight.
            if (opcode_is_gpr_load(prod_opcode)) {
                if ((uses_gpr_rs1 && producer_rd == g_rs1) ||
                    (uses_gpr_rs2 && producer_rd == g_rs2)) {
                    std::cout << "  [HazardUnit] Load hazard (GPR load from EX/MEM)!"
                              << " Load to x" << (int)producer_rd
                              << " needed by instr at PC=0x" << std::hex << if_id.pc << std::dec
                              << " - data not ready until MEM/WB" << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }
        }
    }

    // Load hazard when the value is being written in MEM/WB and forwarding is disabled.
    // In this case the consumer must wait until the register file is updated.
    if (!vm_->enable_forwarding_ && mem_wb.valid && mem_wb.mem_to_reg) {
        uint8_t producer_rd = mem_wb.rd;
        uint8_t prod_opcode = mem_wb.opcode;
        
        bool skip_rd_zero = (opcode_is_gpr_load(prod_opcode) && producer_rd == 0);
        
        if (!skip_rd_zero) {
            // FP load to be written back in this cycle.
            if (opcode_is_fp_load(prod_opcode)) {
                if ((uses_fp_rs1 && producer_rd == f_rs1) ||
                    (uses_fp_rs2 && producer_rd == f_rs2) ||
                    (uses_fp_rs3 && producer_rd == f_rs3)) {
                    std::cout << "  [HazardUnit] Load hazard (FP load from MEM/WB)!"
                              << " Load to f" << (int)producer_rd
                              << " needed by instr at PC=0x" << std::hex << if_id.pc << std::dec 
                              << " - stalling for WB write" << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }
            
            // Integer load to be written back in this cycle.
            if (opcode_is_gpr_load(prod_opcode)) {
                if ((uses_gpr_rs1 && producer_rd == g_rs1) ||
                    (uses_gpr_rs2 && producer_rd == g_rs2)) {
                    std::cout << "  [HazardUnit] Load hazard (GPR load from MEM/WB)!"
                              << " Load to x" << (int)producer_rd
                              << " needed by instr at PC=0x" << std::hex << if_id.pc << std::dec
                              << " - stalling for WB write" << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }
        }
    }

    // When forwarding is disabled, we must also handle RAW hazards for
    // non-load producers in EX/MEM and MEM/WB stages.
    if (!vm_->enable_forwarding_) {
        // RAW hazard where the producer is in EX/MEM and not a load.
        if (ex_mem.valid && ex_mem.reg_write && !ex_mem.mem_read && ex_mem.rd != 0) {
            uint8_t prod_rd = ex_mem.rd;
            uint8_t prod_opcode = ex_mem.opcode;
            uint32_t prod_instr = ex_mem.instr;
            
            bool prod_writes_fp = false;
            bool prod_writes_gpr = false;
            
            // Decide whether this producer writes to FP or GPR.
            if (opcode_is_fp_rtype(prod_opcode) || opcode_is_fmadd_family(prod_opcode)) {
                if (fp_instr_writes_to_gpr(prod_instr)) {
                    prod_writes_gpr = true;
                } else {
                    prod_writes_fp = true;
                }
            } else if (opcode_is_fp_load(prod_opcode)) {
                prod_writes_fp = true;
            } else {
                prod_writes_gpr = true;
            }

            // Check dependences on FP sources.
            if (prod_writes_fp) {
                if ((uses_fp_rs1 && prod_rd == f_rs1) ||
                    (uses_fp_rs2 && prod_rd == f_rs2) ||
                    (uses_fp_rs3 && prod_rd == f_rs3)) {
                    std::cout << "  [HazardUnit] RAW hazard (EX/MEM -> FP). Waiting for f" 
                              << (int)prod_rd << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }

            // Check dependences on GPR sources.
            if (prod_writes_gpr) {
                if ((uses_gpr_rs1 && prod_rd == g_rs1) ||
                    (uses_gpr_rs2 && prod_rd == g_rs2)) {
                    std::cout << "  [HazardUnit] RAW hazard (EX/MEM -> GPR). Waiting for x" 
                              << (int)prod_rd << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }
        }

        // RAW hazard where the producer is in MEM/WB and is writing a register.
        if (mem_wb.valid && mem_wb.reg_write && mem_wb.rd != 0) {
            uint8_t prod_rd = mem_wb.rd;
            uint8_t prod_opcode = mem_wb.opcode;
            uint32_t prod_instr = mem_wb.instr;
            
            bool prod_writes_fp = false;
            bool prod_writes_gpr = false;
            
            // Decide whether this producer writes to FP or GPR.
            if (opcode_is_fp_rtype(prod_opcode) || opcode_is_fmadd_family(prod_opcode)) {
                if (fp_instr_writes_to_gpr(prod_instr)) {
                    prod_writes_gpr = true;
                } else {
                    prod_writes_fp = true;
                }
            } else if (opcode_is_fp_load(prod_opcode)) {
                prod_writes_fp = true;
            } else {
                prod_writes_gpr = true;
            }

            // Check dependences on FP sources.
            if (prod_writes_fp) {
                if ((uses_fp_rs1 && prod_rd == f_rs1) ||
                    (uses_fp_rs2 && prod_rd == f_rs2) ||
                    (uses_fp_rs3 && prod_rd == f_rs3)) {
                    std::cout << "  [HazardUnit] RAW hazard (MEM/WB -> FP). Waiting for f" 
                              << (int)prod_rd << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }

            // Check dependences on GPR sources.
            if (prod_writes_gpr) {
                if ((uses_gpr_rs1 && prod_rd == g_rs1) ||
                    (uses_gpr_rs2 && prod_rd == g_rs2)) {
                    std::cout << "  [HazardUnit] RAW hazard (MEM/WB -> GPR). Waiting for x" 
                              << (int)prod_rd << std::endl;
                    stall_IF_ = stall_ID_ = stalled_ = true;
                    return true;
                }
            }
        }
    }

    return false;
}

// Applies the stall once a hazard has been detected.
// This inserts a bubble into the pipeline by clearing the ID/EX stage.
void HazardUnit::ApplyStall() {
    if (!vm_) return;
    
    vm_->id_ex_.clear();
    vm_->id_ex_.valid = false;
    stalled_ = true;
}
