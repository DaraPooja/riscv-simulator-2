#ifndef RVSS_PIPELINE_VM_H
#define RVSS_PIPELINE_VM_H

#include "vm/rvss/rvss_vm.h"
#include <cstdint>
#include <vector>
#include <string>

// Pipeline VM inherits from RVSS single-cycle VM to reuse existing components
class RVSSPipelineVM : public RVSSVM {
public:
    RVSSPipelineVM();
    virtual ~RVSSPipelineVM();
    
    // Override VmBase interface for pipeline execution
    void Run() override;
    void Step() override;
    void Reset() override;
    void PrintType() override;
    virtual void LoadProgram(const AssembledProgram &program);
    
    // Pipeline-specific initialization (called after LoadProgram)
    void InitializePipeline();
    
    // Pipeline-specific methods
    bool RunPipeline();
    bool ResumePipeline();
    void DumpRegisters();
    
    // Breakpoint state
    bool paused_at_breakpoint_ = false;
    uint64_t paused_pc_ = 0;

private:
    // Pipeline stage methods
    void IF_stage();
    void ID_stage();
    void EX_stage();
    void MEM_stage();
    void WB_stage();
    
    // Pipeline registers - store instruction state between stages
    struct IF_ID_t {
        uint32_t instr = 0;
        uint64_t pc = 0;
        bool valid = false;
        
        void clear() {
            instr = 0;
            pc = 0;
            valid = false;
        }
    } if_id_;
    
    struct ID_EX_t {
        uint32_t instr = 0;
        uint64_t pc = 0;
        
        // Decoded values
        uint8_t rd = 0, rs1 = 0, rs2 = 0;
        uint8_t opcode = 0, funct3 = 0, funct7 = 0;
        int32_t imm = 0;
        
        // Register values read in ID stage
        uint64_t rs1_val = 0;
        uint64_t rs2_val = 0;
        
        // Control signals (from control_unit_)
        bool alu_src = false;
        bool mem_read = false;
        bool mem_write = false;
        bool reg_write = false;
        bool branch = false;
        uint8_t alu_op = 0;
        
        bool valid = false;
        
        void clear() {
            instr = 0;
            pc = 0;
            rd = rs1 = rs2 = 0;
            opcode = funct3 = funct7 = 0;
            imm = 0;
            rs1_val = rs2_val = 0;
            alu_src = mem_read = mem_write = reg_write = branch = false;
            alu_op = 0;
            valid = false;
        }
    } id_ex_;
    
    struct EX_MEM_t {
        uint32_t instr = 0;
        uint64_t pc = 0;
        
        // ALU result
        int64_t alu_result = 0;
        
        // For stores
        uint64_t rs2_val = 0;
        
        // Destination register
        uint8_t rd = 0;
        uint8_t opcode = 0;
        uint8_t funct3 = 0;
        
        // Control signals
        bool mem_read = false;
        bool mem_write = false;
        bool reg_write = false;
        
        // Branch info
        bool branch_taken = false;
        uint64_t branch_target = 0;
        
        bool valid = false;
        
        void clear() {
            instr = 0;
            pc = 0;
            alu_result = 0;
            rs2_val = 0;
            rd = 0;
            opcode = funct3 = 0;
            mem_read = mem_write = reg_write = false;
            branch_taken = false;
            branch_target = 0;
            valid = false;
        }
    } ex_mem_;
    
    struct MEM_WB_t {
        uint32_t instr = 0;
        uint64_t pc = 0;
        
        // Data to write back
        int64_t alu_result = 0;
        int64_t mem_data = 0;
        
        // Destination register
        uint8_t rd = 0;
        uint8_t opcode = 0;
        
        // Control signals
        bool reg_write = false;
        bool mem_to_reg = false;
        
        bool valid = false;
        
        void clear() {
            instr = 0;
            pc = 0;
            alu_result = 0;
            mem_data = 0;
            rd = 0;
            opcode = 0;
            reg_write = mem_to_reg = false;
            valid = false;
        }
    } mem_wb_;
    
    // Resume state
    int resume_cycle_ = 0;
    
    // PC for fetching (separate from RVSSVM's program_counter_)
    uint64_t fetch_pc_ = 0;
};

#endif // RVSS_PIPELINE_VM_H