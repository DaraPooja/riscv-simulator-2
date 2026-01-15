#ifndef RVSS_PIPELINE_VM_H
#define RVSS_PIPELINE_VM_H

#include "vm/rvss/rvss_vm.h"
#include "vm/rvss/hazard_unit.h"
#include "vm/rvss/forwarding_unit.h"
#include "vm/rvss/branch_predictor.h"
#include "vm/rvss/branch_target_buffer.h"
#include "vm/rvss/instruction_scheduler.h"
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>

// Pipeline VM inherits from RVSS single-cycle VM to reuse existing components
class RVSSPipelineVM : public RVSSVM {
public:
    RVSSPipelineVM();
    virtual ~RVSSPipelineVM();

    bool debug_mode_ = false;
    std::ofstream debug_file_;
    std::streambuf* original_cout_;
    

    BranchTargetBuffer btb; 

    struct WB_Bypass_Info {
        bool valid = false;
        uint8_t rd = 0;
        uint64_t value = 0;
        uint32_t instr = 0;
    };
    WB_Bypass_Info wb_bypass_data_;

    // Feature flags
    bool enable_hazard_detection_ = false;
    bool enable_forwarding_ = false;
    bool enable_branch_prediction_ = false;
    bool enable_btb_ = false;  //New flag to explicitly control BTB usage
    bool enable_instruction_scheduling_ = false;


    // Override VmBase interface for pipeline execution
    void Run() override;
    void Step() override;
    void Reset() override;
    void PrintType() override;
    virtual void LoadProgram(const AssembledProgram &program);



    void EnableDebugLogging(const std::string& filename);
    void DisableDebugLogging();
    void PrintFinalRegisters();

    // Pipeline-specific initialization (called after LoadProgram)
    void InitializePipeline();

    // Pipeline-specific methods
    bool RunPipeline();
    bool ResumePipeline();
    void DumpRegisters();

    void EnableForwarding(bool enable) { enable_forwarding_ = enable; forwarding_unit_.Reset(); }

    void SetBranchPredictor(BranchPredictor::Type type, size_t table_size = 1024) {
    branch_predictor_ = BranchPredictor(type, table_size);
}

    // Breakpoint state
    bool paused_at_breakpoint_ = false;
    uint64_t paused_pc_ = 0;

    // Allow HazardUnit to access pipeline registers
    friend class HazardUnit;



private:
    // Units
    HazardUnit hazard_unit_;
    ForwardingUnit forwarding_unit_;
    BranchPredictor branch_predictor_{BranchPredictor::BACKWARD_TAKEN_FORWARD_NOT_TAKEN};

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

        // Prediction info computed at IF and carried to ID
        bool predicted_taken = false;
        uint64_t predicted_target = 0;

        void clear() {
            instr = 0;
            pc = 0;
            valid = false;
            predicted_taken = false;
            predicted_target = 0;
        }
    } if_id_;


struct ID_EX_t {
    uint32_t instr = 0;
    uint64_t pc = 0;

    // Prediction info propagated from IF/ID
    bool predicted_taken = false;
    uint64_t predicted_target = 0;

    // Decoded values
    uint8_t rd = 0, rs1 = 0, rs2 = 0, rs3 = 0;  
    uint8_t opcode = 0, funct3 = 0, funct7 = 0;
    int32_t imm = 0;
    bool uses_rs1 = true;
    bool uses_rs2 = false;
    bool uses_rs3 = false;  // only for FMADD / FMSUB

    // Register values read in ID stage
    uint64_t rs1_val = 0;
    uint64_t rs2_val = 0;
    uint64_t rs3_val = 0;

    // Control signals
    bool alu_src = false;
    bool mem_read = false;
    bool mem_write = false;
    bool reg_write = false;
    bool fp_write = false;
    bool branch = false;
    uint8_t alu_op = 0;

    bool valid = false;

    void clear() {
        instr = 0;
        pc = 0;
        predicted_taken = false;
        predicted_target = 0;
        rd = rs1 = rs2 = rs3 = 0;  
        opcode = funct3 = funct7 = 0;
        imm = 0;
        rs1_val = rs2_val = rs3_val = 0;
        alu_src = mem_read = mem_write = reg_write = fp_write = branch = false;
        uses_rs1 = true; 
        uses_rs2 = uses_rs3 = false;
        alu_op = 0;
        valid = false;
    }
} id_ex_;

    struct EX_MEM_t {
        uint32_t instr = 0;
        uint64_t pc = 0;

        // ALU result
        int64_t alu_result = 0;

        // For stores (value to write to memory)
        uint64_t rs2_val = 0;

        // Destination register
        uint8_t rd = 0;
        uint8_t opcode = 0;
        uint8_t funct3 = 0;

        // Control signals
        bool mem_read = false;
        bool mem_write = false;
        bool reg_write = false;
        bool fp_write = false;

        // Branch info resolved in EX
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
            mem_read = mem_write = reg_write = fp_write = false;
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
        bool fp_write = false;
        bool valid = false;

        void clear() {
            instr = 0;
            pc = 0;
            alu_result = 0;
            mem_data = 0;
            rd = 0;
            opcode = 0;
            reg_write = mem_to_reg = fp_write = false;
            valid = false;
        }
    } mem_wb_;

    // Resume state
    int resume_cycle_ = 0;

    // PC for fetching (separate from RVSSVM's program_counter_)
    uint64_t fetch_pc_ = 0;
};

#endif // RVSS_PIPELINE_VM_H