#include "vm/rvss/rvss_pipeline_vm.h"
#include "globals.h"
#include "common/instructions.h"
#include "utils.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>
#include "vm/rvss/hazard_unit.h"
#include "vm/rvss/branch_predictor.h"
#include "vm/rvss/branch_target_buffer.h"


using instruction_set::Instruction;
using instruction_set::get_instr_encoding;

//Helper to check if instruction is floating-point (including FMADD family)
static inline bool is_fp_instruction(uint32_t instr) {
    uint8_t opcode = instr & 0x7F;
    
    //Check FMADD family opcodes FIRST
    if (opcode == 0x43 || opcode == 0x47 || opcode == 0x4B || opcode == 0x4F) {
        return true;  // FMADD family is always FP
    }
    
    //Then check standard FP instructions
    bool is_f = instruction_set::isFInstruction(instr);
    bool is_d = instruction_set::isDInstruction(instr);
    
    return is_f || is_d;
}

RVSSPipelineVM::RVSSPipelineVM() 
    : RVSSVM()
    , hazard_unit_(this)
    , btb(256)
    , paused_at_breakpoint_(false)
    , paused_pc_(0)
    , resume_cycle_(0)
    , fetch_pc_(0)
    , debug_mode_(false)
    , original_cout_(nullptr)
{
}

RVSSPipelineVM::~RVSSPipelineVM() {}

void RVSSPipelineVM::EnableDebugLogging(const std::string& filename) {
    debug_file_.open(filename, std::ios::out | std::ios::trunc);
    if (debug_file_.is_open()) {
        original_cout_ = std::cout.rdbuf();  // Save original cout
        std::cout.rdbuf(debug_file_.rdbuf()); // Redirect cout to file
        debug_mode_ = true;
        std::cerr << "Debug logging enabled to: " << filename << std::endl;
    }
}

void RVSSPipelineVM::DisableDebugLogging() {
    if (debug_mode_ && original_cout_) {
        std::cout.rdbuf(original_cout_);  // Restore original cout
        debug_file_.close();
        debug_mode_ = false;
    }
}

void RVSSPipelineVM::PrintFinalRegisters() {
    std::cout << "\n=== Final Register Values ===" << std::endl;
    
    // Print GPRs in a compact format
    std::cout << "General Purpose Registers:" << std::endl;
    for (int i = 0; i < 32; i += 4) {
        for (int j = 0; j < 4 && (i + j) < 32; j++) {
            int reg = i + j;
            uint64_t val = registers_.ReadGpr(reg);
            std::cout << "  x" << std::setw(2) << std::setfill(' ') << reg << "=0x" 
                      << std::hex << std::setw(16) << std::setfill('0') << val << std::dec << std::endl;
        }
    }
    

    std::cout << "\nFloating Point Registers:" << std::endl;
    for (int i = 0; i < 32; i++) {
        uint64_t val = registers_.ReadFpr(i);

        std::cout << "  f" << std::setw(2) << std::setfill(' ') << i << "=0x" 
                    << std::hex << std::setw(16) << std::setfill('0') << val << std::dec << std::endl;
    }
    
    std::cout << std::endl;
}

void RVSSPipelineVM::LoadProgram(const AssembledProgram &program) {
    RVSSVM::LoadProgram(program);
    
    if (enable_instruction_scheduling_) {
        std::cout << "[Scheduler] Running basic-block instruction scheduling..." << std::endl;
        
        std::vector<uint32_t> instrs;
        for (size_t i = 0; i < program_size_; i += 4) {
            uint32_t instr = memory_controller_.ReadWord(i);
            instrs.push_back(instr);
        }
        
        std::cout << "[Scheduler] Before scheduling:" << std::endl;
        for (size_t i = 0; i < instrs.size(); ++i) {
            std::cout << "  0x" << std::hex << i * 4 << ": 0x" << instrs[i] << std::dec << std::endl;
        }
        
        auto scheduled = rvss::ScheduleBasicBlocks(instrs);
        
        std::cout << "[Scheduler] After scheduling:" << std::endl;
        for (size_t i = 0; i < scheduled.size(); ++i) {
            std::cout << "  0x" << std::hex << i * 4 << ": 0x" << scheduled[i] << std::dec << std::endl;
        }
        
        for (size_t i = 0; i < scheduled.size(); ++i) {
            memory_controller_.WriteWord(i * 4, scheduled[i]);
        }
        
        rvss::PrintSchedulingInfo(instrs, scheduled);
        
        std::cout << "[Scheduler] Scheduling complete. Instructions: " 
                  << instrs.size() << " → " << scheduled.size() << std::endl;
    }
    
    if_id_.clear();
    id_ex_.clear();
    ex_mem_.clear();
    mem_wb_.clear();

    wb_bypass_data_.valid = false;
    
    paused_at_breakpoint_ = false;
    paused_pc_ = 0;
    resume_cycle_ = 0;
    fetch_pc_ = 0;
    
    hazard_unit_.Reset();
    forwarding_unit_.Reset();
    
    std::cout << "Pipeline VM: Program loaded successfully" << std::endl;
}

void RVSSPipelineVM::DumpRegisters() {
    std::ofstream f(globals::registers_dump_file_path);
    if (!f.is_open()) {
        std::cerr << "Failed to open registers dump file: "
                  << globals::registers_dump_file_path << std::endl;
        return;
    }
    
    f << "{\n";
    for (int i = 0; i < 32; ++i) {
        f << "  \"x" << i << "\": \"0x"
          << std::hex << std::setw(16) << std::setfill('0')
          << registers_.ReadGpr(i) << "\"";
        if (i < 31) f << ",";
        f << "\n";
    }
    f << "}\n";
    f.close();
}

bool RVSSPipelineVM::RunPipeline() {
    if (program_size_ == 0 || program_.text_buffer.empty()) {
        std::cerr << "No program loaded in pipeline VM.\n";
        return true;
    }
    
    // Suppress output if NOT in debug mode
    std::ofstream null_stream;
    std::streambuf* old_cout = nullptr;
    
    if (!debug_mode_) {
        // Normal mode: suppress all stdout by redirecting to /dev/null
        #ifdef _WIN32
            null_stream.open("NUL");
        #else
            null_stream.open("/dev/null");
        #endif
        
        old_cout = std::cout.rdbuf();
        std::cout.rdbuf(null_stream.rdbuf());
    }
    // If debug_mode_ is true, let everything print normally to console
    
    stop_requested_ = false;
    int cycle = resume_cycle_;
    bool skip_first_bp_check = (resume_cycle_ > 0);
    
    while (if_id_.valid || id_ex_.valid || ex_mem_.valid || mem_wb_.valid || fetch_pc_ < program_size_) {
        cycle++;
        
        std::cout << "=== Cycle " << cycle << " === Fetch PC: 0x" << std::hex << fetch_pc_ << std::dec << std::endl;
        
        if (stop_requested_) {
            std::cout << "Pipeline stopped by external request.\n";
            
            //Restore cout BEFORE printing stop message
            if (!debug_mode_ && old_cout) {
                std::cout.rdbuf(old_cout);
                null_stream.close();
            }
            
            //NOW print to actual console (always visible)
            std::cout << "\n=== Pipeline Stopped ===" << std::endl;
            std::cout << "Pipeline stopped by external request at cycle " << cycle << std::endl;
            std::cout << "Fetch PC was at: 0x" << std::hex << fetch_pc_ << std::dec << std::endl;
            
            return false;
        }
        
        if (!skip_first_bp_check && if_id_.valid && CheckBreakpoint(if_id_.pc)) {
            std::cout << "Hit breakpoint at address: 0x" << std::hex << if_id_.pc << std::dec << std::endl;
            
            paused_at_breakpoint_ = true;
            paused_pc_ = if_id_.pc;
            resume_cycle_ = cycle;
            
            // Restore cout BEFORE printing breakpoint message
            if (!debug_mode_ && old_cout) {
                std::cout.rdbuf(old_cout);
                null_stream.close();
            }
            
            // NOW print to actual console (always visible)
            std::cout << "\n=== Breakpoint Hit ===" << std::endl;
            std::cout << "Execution paused at cycle " << cycle << std::endl;
            std::cout << "Breakpoint address: 0x" << std::hex << paused_pc_ << std::dec << std::endl;
            std::cout << "Type 'run' to resume execution." << std::endl;
            
            DumpRegisters();
            PrintFinalRegisters();
            
            return false;
        }
        
        skip_first_bp_check = false;
        Step();
    }
    
    // Restore cout BEFORE printing final summary
    if (!debug_mode_ && old_cout) {
        std::cout.rdbuf(old_cout);
        null_stream.close();
    }
    
    // NOW print final summary (will go to actual console)
    std::cout << "\n=== Pipeline Execution Complete ===" << std::endl;
    std::cout << "Total cycles: " << cycle << std::endl;
    
    if (enable_branch_prediction_) {
        branch_predictor_.PrintStats();
    }
    
    if (enable_btb_) {
        std::cout << "\n=== Branch Target Buffer Statistics ===" << std::endl;
        btb.print_stats();
    }
    
    PrintFinalRegisters();
    DumpRegisters();
    
    paused_at_breakpoint_ = false;
    resume_cycle_ = 0;
    
    return true;
}

bool RVSSPipelineVM::ResumePipeline() {
    if (!paused_at_breakpoint_) {
        std::cout << "No paused state to resume from.\n";
        return false;
    }

    std::cout << "\nResuming Execution" << std::endl;
    std::cout << "Resuming from breakpoint at PC = 0x"
              << std::hex << paused_pc_ << std::dec << std::endl;

    paused_at_breakpoint_ = false;
    
    // Just call RunPipeline - it will handle the redirection
    return RunPipeline();
}

void RVSSPipelineVM::Step() {
    // Save old pipeline registers for proper forwarding timing
    MEM_WB_t old_mem_wb = mem_wb_;
    
    // Execute stages in reverse order (WB → MEM → EX → ID → IF)
    WB_stage();   // Writes to register file AND updates wb_bypass_data_
    MEM_stage();  // Reads ex_mem_, writes to mem_wb_
    
    // Restore old mem_wb for EX stage (proper forwarding timing)
    MEM_WB_t new_mem_wb = mem_wb_;
    mem_wb_ = old_mem_wb;
    
    EX_stage();   // Uses old mem_wb for forwarding
    
    // Restore new mem_wb
    mem_wb_ = new_mem_wb;
    
    ID_stage();   // Reads register file (with fresh WB values) and uses wb_bypass_data_
    IF_stage();   // Fetches next instruction
}

void RVSSPipelineVM::Run() {
    RunPipeline();
}

void RVSSPipelineVM::PrintType() {
    if (enable_forwarding_) {
        std::cout << "Running in Pipeline Mode with Forwarding" << std::endl;
    } else if (enable_hazard_detection_) {
        std::cout << "Running in Pipeline Mode with Hazard Detection" << std::endl;
    } else {
        std::cout << "Running in Basic Pipeline Mode (no hazard detection)" << std::endl;
    }
}

void RVSSPipelineVM::IF_stage() {
    if (enable_hazard_detection_ && hazard_unit_.ShouldStall()) {
        std::cout << "  [IF] Stalled - not fetching" << std::endl;
        return;
    }

    uint64_t next_pc = fetch_pc_ + 4;

    if (fetch_pc_ < program_size_) {
        uint32_t instr = memory_controller_.ReadWord(fetch_pc_);

        if_id_.instr = instr;
        if_id_.pc = fetch_pc_;
        if_id_.valid = true;

        if (enable_branch_prediction_) {
            // Decode opcode to check if it's a branch/jump instruction
            uint8_t opcode = instr & 0x7F;
            bool is_branch_or_jump = (opcode == 0x63 ||  // Branch (BEQ, BNE, BLT, etc.)
                                      opcode == 0x6F ||  // JAL
                                      opcode == 0x67);   // JALR
            
            // Only predict and check BTB for actual branch/jump instructions
            if (is_branch_or_jump) {
                auto pred = branch_predictor_.predict(fetch_pc_, instr);

                if_id_.predicted_taken  = pred.predicted_taken;
                if_id_.predicted_target = pred.predicted_target;

                // Check BTB for cached target
                bool btb_hit = false;
                uint64_t btb_target = 0;
                
                if (enable_btb_) {
                    btb_hit = btb.hit(fetch_pc_);  // BTB access only for branches
                    if (btb_hit) {
                        btb_target = btb.get_target(fetch_pc_);
                    }
                }

                // Decide next PC based on prediction and BTB
                if (pred.predicted_taken) {
                    if (enable_btb_ && btb_hit) {
                        // Use BTB target
                        if (btb.is_jalr_entry(fetch_pc_)) {
                            std::cout << "  [IF] BTB HIT (JALR): using cached target 0x"
                                      << std::hex << btb_target << std::dec 
                                      << " (may be corrected in EX)" << std::endl;
                        } else {
                            std::cout << "  [IF] BTB HIT: predicted TAKEN to 0x"
                                      << std::hex << btb_target << std::dec << std::endl;
                        }
                        next_pc = btb_target;
                    } else if (enable_btb_) {
                        std::cout << "  [IF] BTB MISS: using predictor target 0x"
                                  << std::hex << pred.predicted_target << std::dec << std::endl;
                        next_pc = pred.predicted_target;
                    } else {
                        // No BTB, just use predictor target
                        next_pc = pred.predicted_target;
                    }
                } else {
                    // Predicted NOT TAKEN - fall through
                    if (enable_btb_ && btb_hit) {
                        std::cout << "  [IF] BTB HIT but predicted NOT TAKEN - fall through" << std::endl;
                    }
                    next_pc = fetch_pc_ + 4;
                }
            } else {
                // Not a branch/jump - no prediction needed
                if_id_.predicted_taken = false;
                if_id_.predicted_target = 0;
                next_pc = fetch_pc_ + 4;
            }
        }

        std::cout << "  [IF] Fetched instruction at PC=0x"
                  << std::hex << if_id_.pc 
                  << " instr=0x" << instr << std::dec << std::endl;
    } 
    else {
        if_id_.valid = false;
    }

    fetch_pc_ = next_pc;
}

// ID_stage() must properly handle same-cycle WB bypass

void RVSSPipelineVM::ID_stage() {
    if (!if_id_.valid) {
        id_ex_.valid = false;
        return;
    }

    uint32_t instr = if_id_.instr;

    uint8_t opcode = instr & 0x7F;
    uint8_t rd = (instr >> 7) & 0x1F;
    uint8_t funct3 = (instr >> 12) & 0x7;
    uint8_t rs1 = (instr >> 15) & 0x1F;
    uint8_t rs2 = (instr >> 20) & 0x1F;
    uint8_t funct7 = (instr >> 25) & 0x7F;
    uint8_t rs3 = (instr >> 27) & 0x1F;
    int32_t imm = ImmGenerator(instr);

    control_unit_.SetControlSignals(instr);

    bool is_f = instruction_set::isFInstruction(instr);
    bool is_d = instruction_set::isDInstruction(instr);
    
    if (opcode == 0x43 || opcode == 0x47 || opcode == 0x4B || opcode == 0x4F) {
        uint8_t fmt = (instr >> 25) & 0x3;
        if (fmt == 0b00) is_f = true;
        else if (fmt == 0b01) is_d = true;
    }

    bool uses_rs1 = true;
    bool uses_rs2 = false;
    bool uses_rs3 = false;
    
    if (opcode == 0b0110111 || opcode == 0b0010111) {
        uses_rs1 = false;
    }
    
    if (opcode == 0b0110011 || opcode == 0b0111011 ||
        opcode == 0b0100011 ||
        opcode == 0b1100011 ||
        opcode == 0b0100111 ||
        (is_f && funct7 != 0b1101000 && funct7 != 0b1111000 && funct7 != 0b1100000) ||
        (is_d && funct7 != 0b1101000 && funct7 != 0b1111000 && funct7 != 0b1100000)) {
        uses_rs2 = true;
    }
    
    if (opcode == 0x43 || opcode == 0x47 || opcode == 0x4B || opcode == 0x4F) {
        uses_rs3 = true;
    }
    
    if (enable_hazard_detection_) {
        id_ex_.rs1 = rs1;
        id_ex_.rs2 = rs2;
        id_ex_.rs3 = rs3;
        id_ex_.uses_rs1 = uses_rs1;
        id_ex_.uses_rs2 = uses_rs2;
        id_ex_.uses_rs3 = uses_rs3;
        id_ex_.instr = instr;
        
        bool hazard = hazard_unit_.CheckDataHazard();
        if (hazard) {
            hazard_unit_.ApplyStall();
            return;
        }
    }

    // Read register values with proper FP/GPR selection
    uint64_t rs1_val = 0;
    uint64_t rs2_val = 0;
    uint64_t rs3_val = 0;

    // Determine if this instruction uses FP registers or GPR
    bool uses_fp_rs1 = (is_f || is_d);
    bool uses_fp_rs2 = (is_f || is_d);
    bool uses_fp_rs3 = (is_f || is_d);

    // Special cases where FP instructions use GPR for rs1
    if (funct7 == 0b1101000 || funct7 == 0b1111000 ||
        opcode == get_instr_encoding(Instruction::kflw).opcode ||
        opcode == get_instr_encoding(Instruction::kfld).opcode ||
        opcode == get_instr_encoding(Instruction::kfsw).opcode ||
        opcode == get_instr_encoding(Instruction::kfsd).opcode) {
        uses_fp_rs1 = false;
    }

    // Read rs1
    if (uses_rs1) {
        if (uses_fp_rs1) {
            rs1_val = registers_.ReadFpr(rs1);
            std::cout << "  [ID] Read f" << (int)rs1 << " = 0x" 
                      << std::hex << rs1_val << std::dec << std::endl;
        } else {
            rs1_val = registers_.ReadGpr(rs1);
            std::cout << "  [ID] Read x" << (int)rs1 << " = 0x" 
                      << std::hex << rs1_val << std::dec << std::endl;
        }
    }

    // Read rs2
    if (uses_rs2) {
        if (uses_fp_rs2) {
            rs2_val = registers_.ReadFpr(rs2);
            std::cout << "  [ID] Read f" << (int)rs2 << " = 0x" 
                      << std::hex << rs2_val << std::dec << std::endl;
        } else {
            rs2_val = registers_.ReadGpr(rs2);
            std::cout << "  [ID] Read x" << (int)rs2 << " = 0x" 
                      << std::hex << rs2_val << std::dec << std::endl;
        }
        
        // Override with immediate if needed (but NOT for FP arithmetic!)
        if (control_unit_.GetAluSrc() && opcode != 0x23 && 
            opcode != 0x27) {  // Not store instructions
            rs2_val = static_cast<uint64_t>(static_cast<int64_t>(imm));
            std::cout << "  [ID] Override rs2 with imm = 0x" 
                      << std::hex << rs2_val << std::dec << std::endl;
        }
    }

    // Read rs3 (only for FMADD family)
    if (uses_rs3) {
        if (uses_fp_rs3) {
            rs3_val = registers_.ReadFpr(rs3);
            std::cout << "  [ID] Read f" << (int)rs3 << " = 0x" 
                      << std::hex << rs3_val << std::dec << std::endl;
        }
    }

    // Apply WB-to-ID bypass AFTER reading registers
    // This handles the case where WB writes a value in the same cycle
    if (wb_bypass_data_.valid && !enable_forwarding_) {
        uint8_t wb_rd = wb_bypass_data_.rd;
        uint64_t wb_value = wb_bypass_data_.value;
        bool wb_is_fp = is_fp_instruction(wb_bypass_data_.instr);
        
        std::cout << "  [ID-BYPASS-CHECK] WB has rd=" 
                  << (wb_is_fp ? "f" : "x") << (int)wb_rd 
                  << " value=0x" << std::hex << wb_value << std::dec << std::endl;
        
        // Check rs1
        if (uses_rs1 && rs1 == wb_rd && rs1 != 0) {
            if (uses_fp_rs1 == wb_is_fp) {
                rs1_val = wb_value;
                std::cout << "  [ID-WB-BYPASS] rs1=" 
                          << (uses_fp_rs1 ? "f" : "x") << (int)rs1 
                          << " = 0x" << std::hex << wb_value << std::dec << std::endl;
            }
        }
        
        // Check rs2
        if (uses_rs2 && rs2 == wb_rd && rs2 != 0 && !control_unit_.GetAluSrc()) {
            if (uses_fp_rs2 == wb_is_fp) {
                rs2_val = wb_value;
                std::cout << "  [ID-WB-BYPASS] rs2=" 
                          << (uses_fp_rs2 ? "f" : "x") << (int)rs2 
                          << " = 0x" << std::hex << wb_value << std::dec << std::endl;
            }
        }
        
        // Check rs3
        if (uses_rs3 && rs3 == wb_rd && rs3 != 0) {
            if (uses_fp_rs3 == wb_is_fp) {
                rs3_val = wb_value;
                std::cout << "  [ID-WB-BYPASS] rs3=f" << (int)rs3 
                          << " = 0x" << std::hex << wb_value << std::dec << std::endl;
            }
        }
    }

    // Populate ID/EX pipeline register
    id_ex_.instr = instr;
    id_ex_.pc = if_id_.pc;
    id_ex_.rd = rd;
    id_ex_.rs1 = rs1;
    id_ex_.rs2 = rs2;
    id_ex_.rs3 = rs3;
    id_ex_.opcode = opcode;
    id_ex_.funct3 = funct3;
    id_ex_.funct7 = funct7;
    id_ex_.imm = imm;
    id_ex_.rs1_val = rs1_val;
    id_ex_.rs2_val = rs2_val;
    id_ex_.rs3_val = rs3_val;
    id_ex_.uses_rs1 = uses_rs1;
    id_ex_.uses_rs2 = uses_rs2;
    id_ex_.uses_rs3 = uses_rs3;
    id_ex_.alu_src = control_unit_.GetAluSrc();
    id_ex_.mem_read = control_unit_.GetMemRead();
    id_ex_.mem_write = control_unit_.GetMemWrite();
    id_ex_.reg_write = control_unit_.GetRegWrite();
    id_ex_.branch = control_unit_.GetBranch();
    id_ex_.alu_op = control_unit_.GetAluOp();
    id_ex_.valid = true;

    id_ex_.predicted_taken = if_id_.predicted_taken;
    id_ex_.predicted_target = if_id_.predicted_target;

    const char* rd_prefix = "x";
    const char* rs1_prefix = "x";
    const char* rs2_prefix = "x";
    const char* rs3_prefix = "x";
    
    if (is_f || is_d) {
        if (opcode == get_instr_encoding(Instruction::kflw).opcode ||
            opcode == get_instr_encoding(Instruction::kfld).opcode) {
            rd_prefix = "f";
            rs1_prefix = "x";
            rs2_prefix = "f";
            rs3_prefix = "f";
        }
        else if (opcode == get_instr_encoding(Instruction::kfsw).opcode ||
                 opcode == get_instr_encoding(Instruction::kfsd).opcode) {
            rd_prefix = "f";
            rs1_prefix = "x";
            rs2_prefix = "f";
            rs3_prefix = "f";
        }
        else if (opcode == 0x43 || opcode == 0x47 || opcode == 0x4B || opcode == 0x4F) {
            rd_prefix = "f";
            rs1_prefix = "f";
            rs2_prefix = "f";
            rs3_prefix = "f";
        }
        else if (funct7 == 0b1101000 || funct7 == 0b1101001 ||
                 funct7 == 0b1111000 || funct7 == 0b1111001) {
            rd_prefix = "f";
            rs1_prefix = "x";
            rs2_prefix = "f";
            rs3_prefix = "f";
        }
        else if (funct7 == 0b1100000 || funct7 == 0b1100001 ||
                 funct7 == 0b1110000 || funct7 == 0b1110001 ||
                 funct7 == 0b1010000 || funct7 == 0b1010001) {
            rd_prefix = "x";
            rs1_prefix = "f";
            rs2_prefix = "f";
            rs3_prefix = "f";
        }
        else {
            rd_prefix = "f";
            rs1_prefix = "f";
            rs2_prefix = "f";
            rs3_prefix = "f";
        }
    }
    
    std::cout << "  [ID] Decoded: opcode=0x" << std::hex << (int)opcode
              << " rd=" << rd_prefix << std::dec << (int)rd
              << " rs1=" << rs1_prefix << (int)rs1 
              << " rs2=" << rs2_prefix << (int)rs2 
              << " rs3=" << rs3_prefix << (int)rs3
              << " uses_rs3=" << uses_rs3
              << " alu_src=" << id_ex_.alu_src
              << " isF=" << (is_f ? 1 : 0) 
              << " isD=" << (is_d ? 1 : 0) << std::endl;

    if_id_.valid = false;
}


void RVSSPipelineVM::EX_stage() {
    if (!id_ex_.valid) {
        ex_mem_.valid = false;
        return;
    }

    // Check if this is an FP instruction FIRST
    bool is_fp_inst = is_fp_instruction(id_ex_.instr);

    // Update forwarding unit if forwarding is enabled
    if (enable_forwarding_) {
        forwarding_unit_.Update(
            ex_mem_.reg_write,
            ex_mem_.fp_write,
            ex_mem_.mem_read,
            ex_mem_.rd,
            mem_wb_.reg_write,
            mem_wb_.fp_write,
            mem_wb_.rd,
            id_ex_.rs1,
            id_ex_.rs2,
            id_ex_.rs3,
            id_ex_.uses_rs1,
            id_ex_.uses_rs2,
            id_ex_.uses_rs3,
            id_ex_.alu_src
        );
    }

    // Initialize operands with values from ID stage
    uint64_t operand1 = id_ex_.rs1_val;
    uint64_t operand2 = id_ex_.alu_src ?
        static_cast<uint64_t>(static_cast<int64_t>(id_ex_.imm)) :
        id_ex_.rs2_val;
    uint64_t operand3 = id_ex_.rs3_val;  // For FMADD family
    
    uint64_t forwarded_rs2_val = id_ex_.rs2_val;

    // Apply forwarding if enabled
    if (enable_forwarding_) {
        // Forward rs1 (operand1)
        if (forwarding_unit_.ForwardA == 0b10) {
            operand1 = ex_mem_.alu_result;
            std::cout << "  [EX] ForwardA from EX/MEM: 0x" << std::hex
                      << ex_mem_.alu_result << std::dec << std::endl;
        } else if (forwarding_unit_.ForwardA == 0b01) {
            operand1 = mem_wb_.mem_to_reg ? mem_wb_.mem_data : mem_wb_.alu_result;
            std::cout << "  [EX] ForwardA from MEM/WB: 0x" << std::hex
                      << operand1 << std::dec << std::endl;
        }

        // Forward rs2 (operand2)
        if (forwarding_unit_.ForwardB == 0b10) {
            forwarded_rs2_val = ex_mem_.alu_result;
            if (!id_ex_.alu_src) operand2 = ex_mem_.alu_result;
            std::cout << "  [EX] ForwardB from EX/MEM: 0x" << std::hex
                      << ex_mem_.alu_result << std::dec << std::endl;
        } else if (forwarding_unit_.ForwardB == 0b01) {
            forwarded_rs2_val = mem_wb_.mem_to_reg ? mem_wb_.mem_data : mem_wb_.alu_result;
            if (!id_ex_.alu_src) operand2 = forwarded_rs2_val;
            std::cout << "  [EX] ForwardB from MEM/WB: 0x" << std::hex
                      << forwarded_rs2_val << std::dec << std::endl;
        }

        // Forward rs3 (operand3) -  for FMADD family
        if (forwarding_unit_.ForwardC == 0b10) {
            operand3 = ex_mem_.alu_result;
            std::cout << "  [EX] ForwardC from EX/MEM: 0x" << std::hex
                      << ex_mem_.alu_result << std::dec << std::endl;
        } else if (forwarding_unit_.ForwardC == 0b01) {
            operand3 = mem_wb_.mem_to_reg ? mem_wb_.mem_data : mem_wb_.alu_result;
            std::cout << "  [EX] ForwardC from MEM/WB: 0x" << std::hex
                      << operand3 << std::dec << std::endl;
        }
    }

    // Apply WB-to-EX bypass (same-cycle forwarding from WB stage)
    if (false && wb_bypass_data_.valid) {
        uint8_t wb_rd = wb_bypass_data_.rd;
        uint64_t wb_value = wb_bypass_data_.value;
        bool wb_is_fp = is_fp_instruction(wb_bypass_data_.instr);
        
        // Check if we need to bypass to operand1 (rs1)
        if (id_ex_.uses_rs1 && id_ex_.rs1 == wb_rd && id_ex_.rs1 != 0) {
            bool current_uses_fp_rs1 = is_fp_inst;
            // For FP loads, rs1 is GPR
            if (id_ex_.opcode == get_instr_encoding(Instruction::kflw).opcode ||
                id_ex_.opcode == get_instr_encoding(Instruction::kfld).opcode ||
                id_ex_.opcode == get_instr_encoding(Instruction::kfsw).opcode ||
                id_ex_.opcode == get_instr_encoding(Instruction::kfsd).opcode) {
                current_uses_fp_rs1 = false;
            }
            if (current_uses_fp_rs1 == wb_is_fp) {
                operand1 = wb_value;
                std::cout << "  [EX-WB-BYPASS] rs1=" << (int)id_ex_.rs1 
                          << " = 0x" << std::hex << wb_value << std::dec << std::endl;
            }
        }
        
        // Check if we need to bypass to operand2 (rs2)
        if (id_ex_.uses_rs2 && id_ex_.rs2 == wb_rd && id_ex_.rs2 != 0 && !id_ex_.alu_src) {
            bool current_uses_fp_rs2 = is_fp_inst;
            if (current_uses_fp_rs2 == wb_is_fp) {
                operand2 = wb_value;
                forwarded_rs2_val = wb_value;
                std::cout << "  [EX-WB-BYPASS] rs2=" << (int)id_ex_.rs2 
                          << " = 0x" << std::hex << wb_value << std::dec << std::endl;
            }
        }
        
        // Check if we need to bypass to operand3 (rs3)
        if (id_ex_.uses_rs3 && id_ex_.rs3 == wb_rd && id_ex_.rs3 != 0) {
            bool current_uses_fp_rs3 = is_fp_inst;
            if (current_uses_fp_rs3 == wb_is_fp) {
                operand3 = wb_value;
                std::cout << "  [EX-WB-BYPASS] rs3=" << (int)id_ex_.rs3 
                          << " = 0x" << std::hex << wb_value << std::dec << std::endl;
            }
        }
    }

    // FLOATING-POINT INSTRUCTION EXECUTION
    if (is_fp_inst) {
        uint8_t funct3 = id_ex_.funct3;
        uint8_t rm = funct3;

        // Get rounding mode from CSR if needed
        if (rm == 0b111) rm = registers_.ReadCsr(0x002);

        //: Use the forwarded operands directly
        uint64_t reg1_value = operand1;
        uint64_t reg2_value = operand2;
        uint64_t reg3_value = operand3;

        // For FP loads/stores, override with immediate if alu_src is set
        // But ONLY for loads/stores, NOT for arithmetic operations
        uint8_t opcode = id_ex_.opcode;
        if (id_ex_.alu_src && (opcode == 0x07 || opcode == 0x27)) {  // FLW/FLD or FSW/FSD
            reg2_value = static_cast<uint64_t>(static_cast<int64_t>(id_ex_.imm));
        }

        // Determine if single or double precision
        bool is_single_precision = false;

        // For FMADD family: check fmt field
        if (opcode == 0x43 || opcode == 0x47 || opcode == 0x4B || opcode == 0x4F) {
            uint8_t fmt = (id_ex_.instr >> 25) & 0x3;
            is_single_precision = (fmt == 0b00);  // 00 = single, 01 = double
        } else {
            // For other FP instructions, use existing function
            is_single_precision = instruction_set::isFInstruction(id_ex_.instr);
        }


        // Execute single-precision FP instruction
        if (is_single_precision) {
            uint8_t fcsr = 0;
            auto aluOp = control_unit_.GetAluSignal(id_ex_.instr, id_ex_.alu_op);
            
            std::cout << "    AluOp signal = " << (int)aluOp << std::endl;
            
            auto res_pair = alu::Alu::fpexecute(aluOp, reg1_value, reg2_value, reg3_value, rm);
            uint64_t fp_result_bits = res_pair.first;
            fcsr = static_cast<uint8_t>(res_pair.second);

            std::cout << "    FP result = 0x" << std::hex << fp_result_bits << std::dec << std::endl;

            // Update FCSR flags
            registers_.WriteCsr(0x003, fcsr);

            ex_mem_.alu_result = static_cast<int64_t>(fp_result_bits);
        } 
        // Execute double-precision FP instruction
        else {
            auto aluOp = control_unit_.GetAluSignal(id_ex_.instr, id_ex_.alu_op);
            
            std::cout << "    AluOp signal = " << (int)aluOp << std::endl;
            
            auto res_pair = alu::Alu::dfpexecute(aluOp, reg1_value, reg2_value, reg3_value, rm);
            uint64_t fp_result_bits = res_pair.first;

            std::cout << "    FP result = 0x" << std::hex << fp_result_bits << std::dec << std::endl;

            ex_mem_.alu_result = static_cast<int64_t>(fp_result_bits);
        }

        // Handle branches for FP comparisons (rare, but possible)
        bool branch_taken = false;
        uint64_t branch_target = 0;
        if (id_ex_.branch) {
            uint8_t opcode_local = id_ex_.opcode;
            uint8_t funct3_local = id_ex_.funct3;
            
            if (opcode_local == get_instr_encoding(Instruction::kjalr).opcode ||
                opcode_local == get_instr_encoding(Instruction::kjal).opcode) {
                branch_taken = true;
                branch_target = (opcode_local == get_instr_encoding(Instruction::kjalr).opcode)
                                ? (ex_mem_.alu_result & ~1ULL)
                                : (id_ex_.pc + id_ex_.imm);
            } else if (opcode_local == get_instr_encoding(Instruction::kbeq).opcode) {
                switch (funct3_local) {
                    case 0b000: branch_taken = (ex_mem_.alu_result == 0); break;
                    case 0b001: branch_taken = (ex_mem_.alu_result != 0); break;
                    case 0b100: branch_taken = (ex_mem_.alu_result == 1); break;
                    case 0b101: branch_taken = (ex_mem_.alu_result == 0); break;
                    case 0b110: branch_taken = (ex_mem_.alu_result == 1); break;
                    case 0b111: branch_taken = (ex_mem_.alu_result == 0); break;
                }
                if (branch_taken) branch_target = id_ex_.pc + id_ex_.imm;
            }
        }

        // Populate EX/MEM pipeline register for FP instructions
        ex_mem_.instr = id_ex_.instr;
        ex_mem_.pc = id_ex_.pc;
        ex_mem_.rs2_val = forwarded_rs2_val;
        ex_mem_.rd = id_ex_.rd;
        ex_mem_.opcode = id_ex_.opcode;
        ex_mem_.funct3 = id_ex_.funct3;
        ex_mem_.mem_read = id_ex_.mem_read;
        ex_mem_.mem_write = id_ex_.mem_write;
        ex_mem_.reg_write = id_ex_.reg_write;
        ex_mem_.fp_write = true;  
        ex_mem_.branch_taken = branch_taken;
        ex_mem_.branch_target = branch_target;
        ex_mem_.valid = true;

        std::cout << "  [EX-FP] ALU result: 0x" << std::hex << ex_mem_.alu_result << std::dec << std::endl;

        id_ex_.valid = false;
        return;
    }


    // INTEGER INSTRUCTION EXECUTION

    
    alu::AluOp alu_operation = control_unit_.GetAluSignal(id_ex_.instr, id_ex_.alu_op);
    std::cout << "  [EX-DEBUG] ALU operation: " << (int)alu_operation 
          << " operand1=0x" << std::hex << operand1 
          << " operand2=0x" << operand2 << std::dec << std::endl;
    
    bool overflow;
    int64_t alu_result;
    std::tie(alu_result, overflow) = alu_.execute(alu_operation, operand1, operand2);

    bool branch_taken = false;
    uint64_t branch_target = 0;

    // Handle branch instructions
    if (id_ex_.branch) {
        uint8_t opcode = id_ex_.opcode;
        uint8_t funct3 = id_ex_.funct3;

        // JAL and JALR - unconditional jumps
        if (opcode == get_instr_encoding(Instruction::kjalr).opcode ||
            opcode == get_instr_encoding(Instruction::kjal).opcode) {
            branch_taken = true;
            branch_target = (opcode == get_instr_encoding(Instruction::kjalr).opcode)
                            ? (alu_result & ~1ULL)
                            : (id_ex_.pc + id_ex_.imm);
        } 
        // Conditional branches (BEQ, BNE, BLT, BGE, BLTU, BGEU)
        else if (opcode == get_instr_encoding(Instruction::kbeq).opcode) {
            switch (funct3) {
                case 0b000: branch_taken = (alu_result == 0); break;  // BEQ
                case 0b001: branch_taken = (alu_result != 0); break;  // BNE
                case 0b100: branch_taken = (alu_result == 1); break;  // BLT
                case 0b101: branch_taken = (alu_result == 0); break;  // BGE
                case 0b110: branch_taken = (alu_result == 1); break;  // BLTU
                case 0b111: branch_taken = (alu_result == 0); break;  // BGEU
            }
            if (branch_taken) branch_target = id_ex_.pc + id_ex_.imm;
        }
    }

    // Handle special cases: AUIPC and LUI
    if (id_ex_.opcode == get_instr_encoding(Instruction::kauipc).opcode) {
        alu_result = id_ex_.pc + (id_ex_.imm << 12);
    }
    
    if (id_ex_.opcode == get_instr_encoding(Instruction::klui).opcode) {
        alu_result = static_cast<int64_t>(id_ex_.imm) << 12;
    }

    // Populate EX/MEM pipeline register for integer instructions
    ex_mem_.instr = id_ex_.instr;
    ex_mem_.pc = id_ex_.pc;
    ex_mem_.alu_result = alu_result;
    ex_mem_.rs2_val = forwarded_rs2_val;
    ex_mem_.rd = id_ex_.rd;
    ex_mem_.opcode = id_ex_.opcode;
    ex_mem_.funct3 = id_ex_.funct3;
    ex_mem_.mem_read = id_ex_.mem_read;
    ex_mem_.mem_write = id_ex_.mem_write;
    ex_mem_.reg_write = id_ex_.reg_write;
    ex_mem_.fp_write = false;  // Integer instructions don't write to FPR
    ex_mem_.branch_taken = branch_taken;
    ex_mem_.branch_target = branch_target;
    ex_mem_.valid = true;

    std::cout << "  [EX] ALU result: 0x" << std::hex << alu_result << std::dec << std::endl;


    // BRANCH PREDICTION UPDATE AND MISPREDICTION HANDLING

    if (id_ex_.branch) {
        bool predicted_taken = id_ex_.predicted_taken;
        uint64_t predicted_target = id_ex_.predicted_target;

        // Update branch predictor
        branch_predictor_.update(id_ex_.pc, id_ex_.instr, branch_taken);
        
        // Update BTB if enabled
        if (enable_btb_) {
            if (branch_taken) {
                bool is_jalr = (id_ex_.opcode == 0x67);  // JALR opcode
                btb.update(id_ex_.pc, branch_target, is_jalr);
                
                if (is_jalr) {
                    std::cout << "  [EX] Updated BTB with JALR target: PC=0x"
                              << std::hex << id_ex_.pc << " -> target=0x" 
                              << branch_target << std::dec << std::endl;
                }
            }
        }

        // Check for misprediction
        bool direction_mispredict = (predicted_taken != branch_taken);
        bool target_mispredict = (branch_taken && (predicted_target != branch_target));

        if (direction_mispredict || target_mispredict) {
            std::cout << "  [EX] MISPREDICT: predicted "
                      << (predicted_taken ? "TAKEN" : "NOT")
                      << " but actually "
                      << (branch_taken ? "TAKEN" : "NOT")
                      << ". Flushing IF/ID.\n";

            // Correct the fetch PC
            fetch_pc_ = branch_taken ? branch_target : id_ex_.pc + 4;

            // Flush the pipeline
            if_id_.clear();
            id_ex_.clear();
            if (enable_hazard_detection_) hazard_unit_.Flush();
            if (enable_forwarding_) forwarding_unit_.Reset();
        } else {
            if (branch_taken) {
                std::cout << "  [EX] Branch predicted TAKEN and actually TAKEN (target 0x"
                          << std::hex << branch_target << std::dec << ")\n";
            } else {
                std::cout << "  [EX] Branch predicted NOT-TAKEN and actually NOT-TAKEN\n";
            }
        }
    }

    id_ex_.valid = false;
}



void RVSSPipelineVM::MEM_stage() {
    // Only print MEM-Entry if ex_mem is actually valid
    // Otherwise we're printing stale register data
    if (!ex_mem_.valid) {
        std::cout << "  [MEM-Entry] ex_mem.valid=0 rd=x0 mem_read=0" << std::endl;
        mem_wb_.clear();
        mem_wb_.valid = false;
        return;
    }
    
    //  Now check FP instruction type AFTER we know it's valid
    bool is_fp_instr = is_fp_instruction(ex_mem_.instr);
    
    // Further refinement: For FP conversions to integer, rd is GPR
    if (is_fp_instr) {
        uint8_t funct7 = (ex_mem_.instr >> 25) & 0x7F;
        // FP to int conversions or comparisons write to GPR
        if (funct7 == 0b1100000 || funct7 == 0b1100001 ||
            funct7 == 0b1110000 || funct7 == 0b1110001 ||
            funct7 == 0b1010000 || funct7 == 0b1010001) {
            is_fp_instr = false;  // rd is actually GPR
        }
    }
    
    std::cout << "  [MEM-Entry] ex_mem.valid=" << ex_mem_.valid 
              << " rd=" << (is_fp_instr ? "f" : "x") << (int)ex_mem_.rd 
              << " mem_read=" << ex_mem_.mem_read << std::endl;
    
    int64_t mem_data = 0;
    bool mem_to_reg = false;
    
    if (ex_mem_.mem_read) {
        mem_to_reg = true;
        uint8_t funct3 = ex_mem_.funct3;
        uint64_t addr = ex_mem_.alu_result;
        
        switch (funct3) {
            case 0b000:
                mem_data = static_cast<int8_t>(memory_controller_.ReadByte(addr));
                break;
            case 0b001:
                mem_data = static_cast<int16_t>(memory_controller_.ReadHalfWord(addr));
                break;
            case 0b010:
                mem_data = static_cast<int32_t>(memory_controller_.ReadWord(addr));
                break;
            case 0b011:
                mem_data = memory_controller_.ReadDoubleWord(addr);
                break;
            case 0b100:
                mem_data = static_cast<uint8_t>(memory_controller_.ReadByte(addr));
                break;
            case 0b101:
                mem_data = static_cast<uint16_t>(memory_controller_.ReadHalfWord(addr));
                break;
            case 0b110:
                mem_data = static_cast<uint32_t>(memory_controller_.ReadWord(addr));
                break;
        }
        std::cout << "  [MEM] Read from 0x" << std::hex << addr << " value: 0x" << mem_data << std::dec << std::endl;
    }
    
    if (ex_mem_.mem_write) {
        uint8_t funct3 = ex_mem_.funct3;
        uint64_t addr = ex_mem_.alu_result;
        uint64_t data = ex_mem_.rs2_val;
        
        switch (funct3) {
            case 0b000:
                memory_controller_.WriteByte(addr, data & 0xFF);
                break;
            case 0b001:
                memory_controller_.WriteHalfWord(addr, data & 0xFFFF);
                break;
            case 0b010:
                memory_controller_.WriteWord(addr, data & 0xFFFFFFFF);
                break;
            case 0b011:
                memory_controller_.WriteDoubleWord(addr, data);
                break;
        }
        std::cout << "  [MEM] Write to 0x" << std::hex << addr << " value: 0x" << data << std::dec << std::endl;
    }
    
    mem_wb_.instr = ex_mem_.instr;
    mem_wb_.pc = ex_mem_.pc;
    mem_wb_.alu_result = ex_mem_.alu_result;
    mem_wb_.mem_data = mem_data;
    mem_wb_.rd = ex_mem_.rd;
    mem_wb_.opcode = ex_mem_.opcode;
    mem_wb_.reg_write = ex_mem_.reg_write;
    mem_wb_.fp_write = ex_mem_.fp_write;
    mem_wb_.mem_to_reg = mem_to_reg;
    mem_wb_.valid = true;
    
    std::cout << "  [MEM] Passing to WB: rd=" << (is_fp_instr ? "f" : "x") << (int)mem_wb_.rd 
              << " alu_result=0x" << std::hex << mem_wb_.alu_result 
              << " reg_write=" << mem_wb_.reg_write << std::dec << std::endl;
    
    ex_mem_.valid = false;
}

void RVSSPipelineVM::WB_stage() {
    wb_bypass_data_.valid = false;
    
    if (!mem_wb_.valid) {
        return;
    }

    if (mem_wb_.reg_write) {
        uint8_t opcode = mem_wb_.opcode;
        uint8_t funct7 = (mem_wb_.instr >> 25) & 0x7F;

        bool instr_is_f = instruction_set::isFInstruction(mem_wb_.instr);
        bool instr_is_d = instruction_set::isDInstruction(mem_wb_.instr);  // ✓ Actually check!
        
        // Allow writes to f0, but not x0
        if (mem_wb_.rd == 0 && !instr_is_f && !instr_is_d) {
            mem_wb_.valid = false;
            return;
        }

        uint64_t write_data = 0;

        if (instr_is_f || instr_is_d) {
            bool write_to_gpr = false;
            
            // Check for FP instructions that write to GPR
            if (funct7 == 0b1010000 ||  // FEQ.S, FLT.S, FLE.S
                funct7 == 0b1010001 ||  // FEQ.D, FLT.D, FLE.D
                funct7 == 0b1100000 ||  // FCVT.W.S, FCVT.WU.S, FCVT.L.S, FCVT.LU.S
                funct7 == 0b1100001 ||  // FCVT.W.D, FCVT.WU.D, FCVT.L.D, FCVT.LU.D
                funct7 == 0b1110000 ||  // FMV.X.W, FCLASS.S
                funct7 == 0b1110001) {  // FMV.X.D, FCLASS.D
                write_to_gpr = true;
            }
        
            // Select correct data source
            if (mem_wb_.mem_to_reg) {
                write_data = mem_wb_.mem_data;
            } else {
                write_data = mem_wb_.alu_result;
            }
        
            if (write_to_gpr) {
                registers_.WriteGpr(mem_wb_.rd, write_data);
                std::cout << "  [WB-FP->GPR] Write x" << (int)mem_wb_.rd 
                          << " = 0x" << std::hex << write_data << std::dec << std::endl;
            } else {
                registers_.WriteFpr(mem_wb_.rd, write_data);
                std::cout << "  [WB-FP->FPR] Write f" << (int)mem_wb_.rd 
                          << " = 0x" << std::hex << write_data << std::dec << std::endl;
            }
        
            wb_bypass_data_.valid = true;
            wb_bypass_data_.rd = mem_wb_.rd;
            wb_bypass_data_.value = write_data;
            wb_bypass_data_.instr = mem_wb_.instr;
            
            std::cout << "  [WB-BYPASS-SET] Setting bypass: rd=" 
                      << (write_to_gpr ? "x" : "f") << (int)mem_wb_.rd 
                      << " value=0x" << std::hex << write_data << std::dec << std::endl;
            
        } else {
            // Integer instruction path
            if (mem_wb_.mem_to_reg) {
                write_data = mem_wb_.mem_data;
            } else if (opcode == get_instr_encoding(Instruction::kjal).opcode ||
                       opcode == get_instr_encoding(Instruction::kjalr).opcode) {
                write_data = mem_wb_.pc + 4;
            } else {
                write_data = mem_wb_.alu_result;
            }
        
            registers_.WriteGpr(mem_wb_.rd, write_data);
            std::cout << "  [WB] Write x" << (int)mem_wb_.rd 
                      << " = 0x" << std::hex << write_data << std::dec << std::endl;
            
            wb_bypass_data_.valid = true;
            wb_bypass_data_.rd = mem_wb_.rd;
            wb_bypass_data_.value = write_data;
            wb_bypass_data_.instr = mem_wb_.instr;
            
            std::cout << "  [WB-BYPASS-SET] Setting bypass: rd=x" << (int)mem_wb_.rd 
                      << " value=0x" << std::hex << write_data << std::dec << std::endl;
        }
    }

    mem_wb_.valid = false;
}


void RVSSPipelineVM::Reset() {
    RVSSVM::Reset();
    
    if_id_.clear();
    id_ex_.clear();
    ex_mem_.clear();
    mem_wb_.clear();
    
    paused_at_breakpoint_ = false;
    paused_pc_ = 0;
    resume_cycle_ = 0;
    fetch_pc_ = 0;
    hazard_unit_.Reset();
}

void RVSSPipelineVM::InitializePipeline() {
    program_counter_ = 0;
    if_id_ = IF_ID_t();
    id_ex_ = ID_EX_t();
    ex_mem_ = EX_MEM_t();
    mem_wb_ = MEM_WB_t();
    stop_requested_ = false;
    paused_at_breakpoint_ = false;
    paused_pc_ = 0;
    hazard_unit_.Reset();
    forwarding_unit_.Reset();
    stop_requested_ = false;
}