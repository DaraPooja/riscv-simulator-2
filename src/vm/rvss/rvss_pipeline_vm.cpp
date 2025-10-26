#include "vm/rvss/rvss_pipeline_vm.h"
#include "globals.h"
#include "common/instructions.h"
#include "utils.h"  // For DumpRegisters free function
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>

using instruction_set::Instruction;
using instruction_set::get_instr_encoding;

RVSSPipelineVM::RVSSPipelineVM() : RVSSVM() {
    paused_at_breakpoint_ = false;
    paused_pc_ = 0;
    resume_cycle_ = 0;
    fetch_pc_ = 0;
}

RVSSPipelineVM::~RVSSPipelineVM() {}




void RVSSPipelineVM::LoadProgram(const AssembledProgram &program) {
    // Call parent's LoadProgram to set up memory and program state
    RVSSVM::LoadProgram(program);
    
    // Reset pipeline-specific state
    if_id_.clear();
    id_ex_.clear();
    ex_mem_.clear();
    mem_wb_.clear();
    
    paused_at_breakpoint_ = false;
    paused_pc_ = 0;
    resume_cycle_ = 0;
    fetch_pc_ = 0;
    
    std::cout << "Pipeline VM: Program loaded successfully" << std::endl;
}

void RVSSPipelineVM::DumpRegisters() {
    // Inline register dump implementation
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
    stop_requested_ = false;
    int cycle = resume_cycle_;
    
    while (if_id_.valid || id_ex_.valid || ex_mem_.valid || mem_wb_.valid || fetch_pc_ < program_size_) {
        cycle++;
        std::cout << "=== Cycle " << cycle << " === Fetch PC: 0x" << std::hex << fetch_pc_ << std::dec << std::endl;

        if (stop_requested_) {
            std::cout << "Pipeline stopped by external request.\n";
            return false;
        }

        // Check breakpoints BEFORE executing this cycle
        bool hit_bp = false;
        uint64_t bp_pc = 0;

        // Check each pipeline stage for breakpoint (priority: WB > MEM > EX > ID > IF)
        if (mem_wb_.valid && CheckBreakpoint(mem_wb_.pc)) {
            bp_pc = mem_wb_.pc;
            hit_bp = true;
        } else if (ex_mem_.valid && CheckBreakpoint(ex_mem_.pc)) {
            bp_pc = ex_mem_.pc;
            hit_bp = true;
        } else if (id_ex_.valid && CheckBreakpoint(id_ex_.pc)) {
            bp_pc = id_ex_.pc;
            hit_bp = true;
        } else if (if_id_.valid && CheckBreakpoint(if_id_.pc)) {
            bp_pc = if_id_.pc;
            hit_bp = true;
        }

        if (hit_bp) {
            std::cout << "Hit breakpoint at address: 0x" << std::hex << bp_pc << std::dec << std::endl;
            DumpRegisters();
            
            paused_at_breakpoint_ = true;
            paused_pc_ = bp_pc;
            resume_cycle_ = cycle;
            
            std::cout << "Execution paused at breakpoint. Type 'run' to resume.\n";
            return false;
        }

        // Execute one pipeline cycle
        Step();
    }

    std::cout << "Pipeline execution completed.\n";
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

    std::cout << "Resuming execution from breakpoint at PC = 0x"
              << std::hex << paused_pc_ << std::dec << std::endl;

    paused_at_breakpoint_ = false;
    
    return RunPipeline();
}

void RVSSPipelineVM::Step() {
    // Execute stages in reverse order (WB -> IF) to simulate parallel execution
    WB_stage();
    MEM_stage();
    EX_stage();
    ID_stage();
    IF_stage();
}

void RVSSPipelineVM::Run() {
    RunPipeline();
}

void RVSSPipelineVM::PrintType() {
    std::cout << "Running in Pipeline Mode (5-stage, no hazard detection)" << std::endl;
}

void RVSSPipelineVM::IF_stage() {
    // Fetch instruction from memory if within program bounds
    if (fetch_pc_ < program_size_) {
        // Read 4 bytes for instruction (little-endian)
        uint32_t instr = memory_controller_.ReadWord(fetch_pc_);
        
        if_id_.instr = instr;
        if_id_.pc = fetch_pc_;
        if_id_.valid = true;
        
        fetch_pc_ += 4;
        
        std::cout << "  [IF] Fetched instruction at PC=0x" << std::hex << if_id_.pc 
                  << " instr=0x" << instr << std::dec << std::endl;
    } else {
        if_id_.valid = false;
    }
}

void RVSSPipelineVM::ID_stage() {
    if (!if_id_.valid) {
        id_ex_.valid = false;
        return;
    }
    
    uint32_t instr = if_id_.instr;
    
    // Decode instruction fields
    uint8_t opcode = instr & 0x7F;
    uint8_t rd = (instr >> 7) & 0x1F;
    uint8_t funct3 = (instr >> 12) & 0x7;
    uint8_t rs1 = (instr >> 15) & 0x1F;
    uint8_t rs2 = (instr >> 20) & 0x1F;
    uint8_t funct7 = (instr >> 25) & 0x7F;
    
    // Generate immediate (reuse from single-cycle VM)
    int32_t imm = ImmGenerator(instr);
    
    // Read register values
    uint64_t rs1_val = registers_.ReadGpr(rs1);
    uint64_t rs2_val = registers_.ReadGpr(rs2);
    
    // Set control signals (reuse control unit from RVSSVM)
    control_unit_.SetControlSignals(instr);
    
    // Pass everything to EX stage
    id_ex_.instr = instr;
    id_ex_.pc = if_id_.pc;
    id_ex_.rd = rd;
    id_ex_.rs1 = rs1;
    id_ex_.rs2 = rs2;
    id_ex_.opcode = opcode;
    id_ex_.funct3 = funct3;
    id_ex_.funct7 = funct7;
    id_ex_.imm = imm;
    id_ex_.rs1_val = rs1_val;
    id_ex_.rs2_val = rs2_val;
    
    // Copy control signals
    id_ex_.alu_src = control_unit_.GetAluSrc();
    id_ex_.mem_read = control_unit_.GetMemRead();
    id_ex_.mem_write = control_unit_.GetMemWrite();
    id_ex_.reg_write = control_unit_.GetRegWrite();
    id_ex_.branch = control_unit_.GetBranch();
    id_ex_.alu_op = control_unit_.GetAluOp();
    
    id_ex_.valid = true;
    
    std::cout << "  [ID] Decoded: opcode=0x" << std::hex << (int)opcode 
              << " rd=x" << std::dec << (int)rd 
              << " rs1=x" << (int)rs1 << " rs2=x" << (int)rs2 << std::endl;
    
    if_id_.valid = false;
}

void RVSSPipelineVM::EX_stage() {
    if (!id_ex_.valid) {
        ex_mem_.valid = false;
        return;
    }
    
    uint64_t operand1 = id_ex_.rs1_val;
    uint64_t operand2 = id_ex_.alu_src ? static_cast<uint64_t>(static_cast<int64_t>(id_ex_.imm)) : id_ex_.rs2_val;
    
    // Execute ALU operation (reuse from RVSSVM)
    alu::AluOp alu_operation = control_unit_.GetAluSignal(id_ex_.instr, id_ex_.alu_op);
    bool overflow;
    int64_t alu_result;
    std::tie(alu_result, overflow) = alu_.execute(alu_operation, operand1, operand2);
    
    // Handle branches
    bool branch_taken = false;
    uint64_t branch_target = 0;
    
    if (id_ex_.branch) {
        uint8_t opcode = id_ex_.opcode;
        uint8_t funct3 = id_ex_.funct3;
        
        if (opcode == get_instr_encoding(Instruction::kjalr).opcode || 
            opcode == get_instr_encoding(Instruction::kjal).opcode) {
            branch_taken = true;
            if (opcode == get_instr_encoding(Instruction::kjalr).opcode) {
                branch_target = alu_result & ~1ULL;
            } else {
                branch_target = id_ex_.pc + id_ex_.imm;
            }
        } else if (opcode == get_instr_encoding(Instruction::kbeq).opcode) {
            switch (funct3) {
                case 0b000: branch_taken = (alu_result == 0); break; // BEQ
                case 0b001: branch_taken = (alu_result != 0); break; // BNE
                case 0b100: branch_taken = (alu_result == 1); break; // BLT
                case 0b101: branch_taken = (alu_result == 0); break; // BGE
                case 0b110: branch_taken = (alu_result == 1); break; // BLTU
                case 0b111: branch_taken = (alu_result == 0); break; // BGEU
            }
            if (branch_taken) {
                branch_target = id_ex_.pc + id_ex_.imm;
            }
        }
    }
    
    // Handle AUIPC
    if (id_ex_.opcode == get_instr_encoding(Instruction::kauipc).opcode) {
        alu_result = id_ex_.pc + (id_ex_.imm << 12);
    }
    
    // Pass to MEM stage
    ex_mem_.instr = id_ex_.instr;
    ex_mem_.pc = id_ex_.pc;
    ex_mem_.alu_result = alu_result;
    ex_mem_.rs2_val = id_ex_.rs2_val;
    ex_mem_.rd = id_ex_.rd;
    ex_mem_.opcode = id_ex_.opcode;
    ex_mem_.funct3 = id_ex_.funct3;
    ex_mem_.mem_read = id_ex_.mem_read;
    ex_mem_.mem_write = id_ex_.mem_write;
    ex_mem_.reg_write = id_ex_.reg_write;
    ex_mem_.branch_taken = branch_taken;
    ex_mem_.branch_target = branch_target;
    ex_mem_.valid = true;
    
    std::cout << "  [EX] ALU result: 0x" << std::hex << alu_result << std::dec << std::endl;
    
    // Handle branch/jump (flush pipeline if taken)
    if (branch_taken) {
        std::cout << "  [EX] Branch taken to 0x" << std::hex << branch_target << std::dec << std::endl;
        fetch_pc_ = branch_target;
        if_id_.clear();
        id_ex_.clear();
        // Don't clear id_ex_ here since we're moving it to ex_mem_
    }
    
    id_ex_.valid = false;
}

void RVSSPipelineVM::MEM_stage() {
    if (!ex_mem_.valid) {
        mem_wb_.valid = false;
        return;
    }
    
    int64_t mem_data = 0;
    bool mem_to_reg = false;
    
    // Memory read
    if (ex_mem_.mem_read) {
        mem_to_reg = true;
        uint8_t funct3 = ex_mem_.funct3;
        uint64_t addr = ex_mem_.alu_result;
        
        switch (funct3) {
            case 0b000: // LB
                mem_data = static_cast<int8_t>(memory_controller_.ReadByte(addr));
                break;
            case 0b001: // LH
                mem_data = static_cast<int16_t>(memory_controller_.ReadHalfWord(addr));
                break;
            case 0b010: // LW
                mem_data = static_cast<int32_t>(memory_controller_.ReadWord(addr));
                break;
            case 0b011: // LD
                mem_data = memory_controller_.ReadDoubleWord(addr);
                break;
            case 0b100: // LBU
                mem_data = static_cast<uint8_t>(memory_controller_.ReadByte(addr));
                break;
            case 0b101: // LHU
                mem_data = static_cast<uint16_t>(memory_controller_.ReadHalfWord(addr));
                break;
            case 0b110: // LWU
                mem_data = static_cast<uint32_t>(memory_controller_.ReadWord(addr));
                break;
        }
        std::cout << "  [MEM] Read from 0x" << std::hex << addr << " value: 0x" << mem_data << std::dec << std::endl;
    }
    
    // Memory write
    if (ex_mem_.mem_write) {
        uint8_t funct3 = ex_mem_.funct3;
        uint64_t addr = ex_mem_.alu_result;
        uint64_t data = ex_mem_.rs2_val;
        
        switch (funct3) {
            case 0b000: // SB
                memory_controller_.WriteByte(addr, data & 0xFF);
                break;
            case 0b001: // SH
                memory_controller_.WriteHalfWord(addr, data & 0xFFFF);
                break;
            case 0b010: // SW
                memory_controller_.WriteWord(addr, data & 0xFFFFFFFF);
                break;
            case 0b011: // SD
                memory_controller_.WriteDoubleWord(addr, data);
                break;
        }
        std::cout << "  [MEM] Write to 0x" << std::hex << addr << " value: 0x" << data << std::dec << std::endl;
    }
    
    // Pass to WB stage
    mem_wb_.instr = ex_mem_.instr;
    mem_wb_.pc = ex_mem_.pc;
    mem_wb_.alu_result = ex_mem_.alu_result;
    mem_wb_.mem_data = mem_data;
    mem_wb_.rd = ex_mem_.rd;
    mem_wb_.opcode = ex_mem_.opcode;
    mem_wb_.reg_write = ex_mem_.reg_write;
    mem_wb_.mem_to_reg = mem_to_reg;
    mem_wb_.valid = true;
    
    ex_mem_.valid = false;
}

void RVSSPipelineVM::WB_stage() {
    if (!mem_wb_.valid) return;
    
    if (mem_wb_.reg_write && mem_wb_.rd != 0) {
        uint64_t write_data = 0;
        uint8_t opcode = mem_wb_.opcode;
        
        // Determine what to write back
        if (mem_wb_.mem_to_reg) {
            write_data = mem_wb_.mem_data;
        } else if (opcode == get_instr_encoding(Instruction::kjal).opcode || 
                   opcode == get_instr_encoding(Instruction::kjalr).opcode) {
            write_data = mem_wb_.pc + 4;
        } else if (opcode == get_instr_encoding(Instruction::klui).opcode) {
            int32_t imm = ImmGenerator(mem_wb_.instr);
            write_data = imm << 12;
        } else {
            write_data = mem_wb_.alu_result;
        }
        
        registers_.WriteGpr(mem_wb_.rd, write_data);
        std::cout << "  [WB] Write x" << (int)mem_wb_.rd << " = 0x" << std::hex << write_data << std::dec << std::endl;
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
}
