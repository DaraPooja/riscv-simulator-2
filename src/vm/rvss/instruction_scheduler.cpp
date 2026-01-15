#include "vm/rvss/instruction_scheduler.h"
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <climits>

namespace rvss {

// These helpers simplify decoding specific fields from RISC-V instructions.
static inline uint8_t opcode_of(uint32_t instr) { return instr & 0x7F; }
static inline uint8_t rd_of(uint32_t instr) { return (instr >> 7) & 0x1F; }
static inline uint8_t funct3_of(uint32_t instr) { return (instr >> 12) & 0x7; }
static inline uint8_t rs1_of(uint32_t instr) { return (instr >> 15) & 0x1F; }
static inline uint8_t rs2_of(uint32_t instr) { return (instr >> 20) & 0x1F; }
static inline uint8_t rs3_of(uint32_t instr) { return (instr >> 27) & 0x1F; }
static inline uint8_t funct7_of(uint32_t instr) { return (instr >> 25) & 0x7F; }

// These functions classify instructions based on their opcode.
static inline bool is_branch_or_jal(uint32_t instr) {
    uint8_t op = opcode_of(instr);
    return op == 0x63 || op == 0x6F || op == 0x67;
}

static inline bool is_load(uint32_t instr) {
    uint8_t op = opcode_of(instr);
    return op == 0x03 || op == 0x07;
}

static inline bool is_store(uint32_t instr) {
    uint8_t op = opcode_of(instr);
    return op == 0x23 || op == 0x27;
}

static inline bool is_memory_op(uint32_t instr) {
    return is_load(instr) || is_store(instr);
}

static inline bool is_control_flow(uint32_t instr) {
    return is_branch_or_jal(instr);
}

// Checks whether an instruction belongs to the floating-point unit.
static inline bool is_fp_instruction(uint32_t instr) {
    uint8_t op = opcode_of(instr);
    return op == 0x07 || op == 0x27 || op == 0x43 || op == 0x47 ||
           op == 0x4B || op == 0x4F || op == 0x53;
}

// Each instruction’s register usage is described using this structure.
// It records which registers are read or written and whether they are integer or FP.
enum class RegType { INTEGER, FLOAT, NONE };

struct RegisterInfo {
    RegType rd_type = RegType::NONE;
    RegType rs1_type = RegType::NONE;
    RegType rs2_type = RegType::NONE;
    RegType rs3_type = RegType::NONE;
    uint8_t rd = 0;
    uint8_t rs1 = 0;
    uint8_t rs2 = 0;
    uint8_t rs3 = 0;
    bool writes_rd = false;
    bool reads_rs1 = false;
    bool reads_rs2 = false;
    bool reads_rs3 = false;
};

// This function determines which registers an instruction reads or writes,
// including special handling for floating-point instructions and conversions.
static RegisterInfo decode_registers(uint32_t instr) {
    RegisterInfo info;
    uint8_t op = opcode_of(instr);
    
    info.rd = rd_of(instr);
    info.rs1 = rs1_of(instr);
    info.rs2 = rs2_of(instr);
    info.rs3 = rs3_of(instr);
    
    // Integer ALU operations.
    if (op == 0x33 || op == 0x13) {
        info.writes_rd = (info.rd != 0);
        info.rd_type = RegType::INTEGER;
        info.reads_rs1 = (info.rs1 != 0);
        info.rs1_type = RegType::INTEGER;
        if (op == 0x33) {
            info.reads_rs2 = (info.rs2 != 0);
            info.rs2_type = RegType::INTEGER;
        }
    }
    else if (op == 0x03) {
        info.writes_rd = (info.rd != 0);
        info.rd_type = RegType::INTEGER;
        info.reads_rs1 = (info.rs1 != 0);
        info.rs1_type = RegType::INTEGER;
    }
    else if (op == 0x23) {
        info.reads_rs1 = (info.rs1 != 0);
        info.rs1_type = RegType::INTEGER;
        info.reads_rs2 = (info.rs2 != 0);
        info.rs2_type = RegType::INTEGER;
    }
    // FP memory operations.
    else if (op == 0x07) {
        info.writes_rd = true;
        info.rd_type = RegType::FLOAT;
        info.reads_rs1 = (info.rs1 != 0);
        info.rs1_type = RegType::INTEGER;
    }
    else if (op == 0x27) {
        info.reads_rs1 = (info.rs1 != 0);
        info.rs1_type = RegType::INTEGER;
        info.reads_rs2 = true;
        info.rs2_type = RegType::FLOAT;
    }
    // FP fused operations (FMADD, etc.).
    else if (op == 0x43 || op == 0x47 || op == 0x4B || op == 0x4F) {
        info.writes_rd = true;
        info.rd_type = RegType::FLOAT;
        info.reads_rs1 = true;
        info.rs1_type = RegType::FLOAT;
        info.reads_rs2 = true;
        info.rs2_type = RegType::FLOAT;
        info.reads_rs3 = true;
        info.rs3_type = RegType::FLOAT;
    }
    // Floating-point ALU.
    else if (op == 0x53) {
        uint8_t funct7 = funct7_of(instr);
        info.writes_rd = true;
        info.rd_type = RegType::FLOAT;
        info.reads_rs1 = true;
        info.rs1_type = RegType::FLOAT;
        
        // These special FP ops have unique input/output register types.
        if ((funct7 & 0x7C) == 0x68 || (funct7 & 0x7C) == 0x78) {
            info.rs1_type = RegType::INTEGER;
            info.reads_rs2 = false;
        }
        else if ((funct7 & 0x7C) == 0x60 || (funct7 & 0x7C) == 0x70) {
            info.rd_type = RegType::INTEGER;
            info.reads_rs2 = false;
        }
        else if (funct7 == 0x70 || funct7 == 0x71) {
            info.rd_type = RegType::INTEGER;
            info.reads_rs2 = false;
        }
        else if (funct7 == 0x78 || funct7 == 0x79) {
            info.rs1_type = RegType::INTEGER;
            info.reads_rs2 = false;
        }
        else if ((funct7 & 0x7C) == 0x2C || funct7 == 0x70) {
            info.reads_rs2 = false;
        }
        else if ((funct7 & 0x7C) == 0x50) {
            info.rd_type = RegType::INTEGER;
            info.reads_rs2 = true;
            info.rs2_type = RegType::FLOAT;
        }
        else {
            info.reads_rs2 = true;
            info.rs2_type = RegType::FLOAT;
        }
    }
    // Branch instructions.
    else if (op == 0x63) {
        info.reads_rs1 = (info.rs1 != 0);
        info.rs1_type = RegType::INTEGER;
        info.reads_rs2 = (info.rs2 != 0);
        info.rs2_type = RegType::INTEGER;
    }
    // JAL writes rd.
    else if (op == 0x6F) {
        info.writes_rd = (info.rd != 0);
        info.rd_type = RegType::INTEGER;
    }
    // JALR writes rd and reads rs1.
    else if (op == 0x67) {
        info.writes_rd = (info.rd != 0);
        info.rd_type = RegType::INTEGER;
        info.reads_rs1 = (info.rs1 != 0);
        info.rs1_type = RegType::INTEGER;
    }
    else if (op == 0x37 || op == 0x17) {
        info.writes_rd = (info.rd != 0);
        info.rd_type = RegType::INTEGER;
    }
    
    return info;
}

// The next two helper functions decode the immediate fields of JAL and branch instructions.
// These are needed to detect backward branches and build basic blocks.
static int32_t extract_jal_immediate(uint32_t instr) {
    int32_t imm = ((int32_t)(instr & 0x80000000) >> 11) |
                  (instr & 0xff000) |
                  ((instr >> 9) & 0x800) |
                  ((instr >> 20) & 0x7fe);
    return imm;
}

static int32_t extract_branch_immediate(uint32_t instr) {
    int32_t imm = ((int32_t)(instr & 0x80000000) >> 19) |
                  ((instr & 0x80) << 4) |
                  ((instr >> 20) & 0x7e0) |
                  ((instr >> 7) & 0x1e);
    return imm;
}

// This scans the full instruction list and identifies all possible branch targets.
// The result is used to break the program into basic blocks for scheduling.
static std::unordered_set<size_t> find_branch_targets(const std::vector<uint32_t>& instrs) {
    std::unordered_set<size_t> targets;
    
    for (size_t i = 0; i < instrs.size(); ++i) {
        uint32_t instr = instrs[i];
        uint8_t op = opcode_of(instr);
        
        if (op == 0x6F) {
            int32_t imm = extract_jal_immediate(instr);
            int64_t target_idx = (int64_t)i + (imm / 4);
            if (target_idx >= 0 && target_idx < (int64_t)instrs.size()) {
                targets.insert((size_t)target_idx);
            }
        }
        else if (op == 0x63) {
            int32_t imm = extract_branch_immediate(instr);
            int64_t target_idx = (int64_t)i + (imm / 4);
            if (target_idx >= 0 && target_idx < (int64_t)instrs.size()) {
                targets.insert((size_t)target_idx);
            }
        }
    }
    
    return targets;
}

// This function estimates how long different operations take.
// It helps the scheduler prioritize instructions so that long-latency FP ops
// can be overlapped with useful independent work.
static int get_execution_latency(uint32_t instr) {
    uint8_t op = opcode_of(instr);

    if (is_load(instr)) return 2;
    if (is_store(instr)) return 1;
    if (is_control_flow(instr)) return 1;
    
    if (op == 0x53) {
        uint8_t funct7 = funct7_of(instr);
        if ((funct7 & 0x7C) == 0x08) return 4;
        if ((funct7 & 0x7C) == 0x0C || (funct7 & 0x7C) == 0x2C) return 8;
        return 3;
    }
    
    if (op == 0x43 || op == 0x47 || op == 0x4B || op == 0x4F) return 4;
    if (op == 0x07) return 2;
    if (op == 0x27) return 1;
    if (op == 0x33 || op == 0x13) return 1;
    if (op == 0x37 || op == 0x17) return 1;

    return 1;
}

// This structure records all register dependencies inside a basic block,
// including separate lists for integer and floating-point registers.
struct DependencyInfo {
    std::unordered_map<uint8_t, std::vector<size_t>> int_writers;
    std::unordered_map<uint8_t, std::vector<size_t>> int_readers;
    std::unordered_map<uint8_t, std::vector<size_t>> fp_writers;
    std::unordered_map<uint8_t, std::vector<size_t>> fp_readers;
    
    std::vector<size_t> stores;
    std::vector<size_t> loads;
    std::vector<size_t> control_flow;
    std::vector<size_t> fp_operations;
};

// This analyzes an entire block and fills in the dependency tables.
static DependencyInfo analyze_dependencies(const std::vector<uint32_t>& block) {
    DependencyInfo deps;
    
    for (size_t i = 0; i < block.size(); ++i) {
        uint32_t instr = block[i];
        RegisterInfo info = decode_registers(instr);
        
        // Writers.
        if (info.writes_rd) {
            if (info.rd_type == RegType::INTEGER && info.rd != 0) {
                deps.int_writers[info.rd].push_back(i);
            } else if (info.rd_type == RegType::FLOAT) {
                deps.fp_writers[info.rd].push_back(i);
            }
        }
        // Readers.
        if (info.reads_rs1) {
            if (info.rs1_type == RegType::INTEGER && info.rs1 != 0) {
                deps.int_readers[info.rs1].push_back(i);
            } else if (info.rs1_type == RegType::FLOAT) {
                deps.fp_readers[info.rs1].push_back(i);
            }
        }
        if (info.reads_rs2) {
            if (info.rs2_type == RegType::INTEGER && info.rs2 != 0) {
                deps.int_readers[info.rs2].push_back(i);
            } else if (info.rs2_type == RegType::FLOAT) {
                deps.fp_readers[info.rs2].push_back(i);
            }
        }
        if (info.reads_rs3 && info.rs3_type == RegType::FLOAT) {
            deps.fp_readers[info.rs3].push_back(i);
        }
        
        if (is_store(instr)) deps.stores.push_back(i);
        if (is_load(instr)) deps.loads.push_back(i);
        if (is_control_flow(instr)) deps.control_flow.push_back(i);
        if (is_fp_instruction(instr)) deps.fp_operations.push_back(i);
    }
    
    return deps;
}

// This is the core scheduler for a single basic block.
// It attempts to reorder instructions only when it is safe and beneficial,
// such as hiding latency of FP operations or grouping independent instructions.
static std::vector<uint32_t> schedule_block(const std::vector<uint32_t>& block) {
    size_t n = block.size();
    if (n <= 1) return block;

    // If the block contains backward branches or computed jumps, 
    // reordering is unsafe and we keep the original order.
    for (const auto& instr : block) {
        uint8_t opcode = opcode_of(instr);
        
        if (opcode == 0x63) {
            int32_t imm = extract_branch_immediate(instr);
            if (imm < 0) {
                return block;
            }
        }
        if (opcode == 0x67) {
            return block;
        }
    }
    
    DependencyInfo deps = analyze_dependencies(block);
    
    std::vector<uint32_t> out;
    out.reserve(n);
    std::vector<bool> scheduled(n, false);
    
    // Decides whether it is safe to schedule the instruction at index i.
    auto can_schedule = [&](size_t i) -> bool {
        RegisterInfo info_i = decode_registers(block[i]);
        bool is_ctrl_i = is_control_flow(block[i]);
        bool is_load_i = is_load(block[i]);
        bool is_store_i = is_store(block[i]);
        
        // Control flow must be scheduled last.
        if (is_ctrl_i) {
            for (size_t j = 0; j < n; ++j) {
                if (!scheduled[j] && j != i) return false;
            }
            return true;
        }
        
        // Do not move instructions above unresolved control flow.
        for (size_t j : deps.control_flow) {
            if (j < i && !scheduled[j]) {
                return false;
            }
        }
        
        // RAW dependency check.
        if (info_i.reads_rs1) {
            auto& writers = (info_i.rs1_type == RegType::INTEGER)
                                ? deps.int_writers
                                : deps.fp_writers;
            if (writers.count(info_i.rs1)) {
                for (size_t writer : writers.at(info_i.rs1)) {
                    if (writer < i && !scheduled[writer]) return false;
                }
            }
        }
        if (info_i.reads_rs2) {
            auto& writers = (info_i.rs2_type == RegType::INTEGER)
                                ? deps.int_writers
                                : deps.fp_writers;
            if (writers.count(info_i.rs2)) {
                for (size_t writer : writers.at(info_i.rs2)) {
                    if (writer < i && !scheduled[writer]) return false;
                }
            }
        }
        if (info_i.reads_rs3 && info_i.rs3_type == RegType::FLOAT) {
            if (deps.fp_writers.count(info_i.rs3)) {
                for (size_t writer : deps.fp_writers.at(info_i.rs3)) {
                    if (writer < i && !scheduled[writer]) return false;
                }
            }
        }
        
        // WAW check.
        if (info_i.writes_rd) {
            auto& writers = (info_i.rd_type == RegType::INTEGER)
                                ? deps.int_writers
                                : deps.fp_writers;
            if (writers.count(info_i.rd)) {
                for (size_t other_writer : writers.at(info_i.rd)) {
                    if (other_writer < i && !scheduled[other_writer]) return false;
                }
            }
        }
        
        // WAR check.
        if (info_i.writes_rd) {
            auto& readers = (info_i.rd_type == RegType::INTEGER)
                                ? deps.int_readers
                                : deps.fp_readers;
            if (readers.count(info_i.rd)) {
                for (size_t reader : readers.at(info_i.rd)) {
                    if (reader < i && !scheduled[reader]) return false;
                }
            }
        }
        
        // Stores cannot move above earlier stores or unscheduled loads.
        if (is_store_i) {
            for (size_t other_store : deps.stores) {
                if (other_store < i && !scheduled[other_store]) return false;
            }
            for (size_t load : deps.loads) {
                if (load < i && scheduled[load]) return false;
            }
        }
        
        // Loads cannot move above earlier stores.
        if (is_load_i) {
            for (size_t store : deps.stores) {
                if (store < i && !scheduled[store]) return false;
            }
        }
        
        return true;
    };
    
    // This loop chooses instructions one by one based on dependency rules
    // and a scoring system that prefers independent and latency-hiding scheduling.
    size_t scheduled_count = 0;
    int current_cycle = 0;
    std::vector<int> ready_cycle(n, 0);

    while (scheduled_count < n) {
        size_t best_candidate = n;
        int best_score = -100000;
        int earliest_ready = INT_MAX;
        
        for (size_t i = 0; i < n; ++i) {
            if (!scheduled[i] && can_schedule(i)) {
                earliest_ready = std::min(earliest_ready, ready_cycle[i]);
            }
        }
        
        if (earliest_ready > current_cycle) {
            current_cycle = earliest_ready;
        }
        
        for (size_t i = 0; i < n; ++i) {
            if (scheduled[i] || !can_schedule(i)) continue;
            if (ready_cycle[i] > current_cycle) continue;
            
            int score = 0;
            RegisterInfo info = decode_registers(block[i]);
            
            if (out.size() > 0) {
                RegisterInfo prev_info = decode_registers(out.back());
                bool depends_on_prev = false;
                
                if (prev_info.writes_rd) {
                    if (info.reads_rs1 && info.rs1 == prev_info.rd && info.rs1_type == prev_info.rd_type)
                        depends_on_prev = true;
                    if (info.reads_rs2 && info.rs2 == prev_info.rd && info.rs2_type == prev_info.rd_type)
                        depends_on_prev = true;
                    if (info.reads_rs3 && info.rs3 == prev_info.rd && info.rs3_type == prev_info.rd_type)
                        depends_on_prev = true;
                }
                
                if (depends_on_prev) score -= 10;
                else score += 5;
            }
            
            if (out.size() > 0 && is_fp_instruction(out.back()) && !is_fp_instruction(block[i])) {
                score += 8;
            }
            
            score += (current_cycle - ready_cycle[i]) * 2;
            score -= (int)i;
            
            if (score > best_score) {
                best_score = score;
                best_candidate = i;
            }
        }
        
        if (best_candidate < n) {
            scheduled[best_candidate] = true;
            out.push_back(block[best_candidate]);
            scheduled_count++;
            
            int latency = get_execution_latency(block[best_candidate]);
            int completion_cycle = current_cycle + latency;
            
            RegisterInfo info = decode_registers(block[best_candidate]);
            
            if (info.writes_rd) {
                for (size_t j = 0; j < n; ++j) {
                    if (scheduled[j]) continue;
                    
                    RegisterInfo dep_info = decode_registers(block[j]);
                    bool is_dependent = false;
                    
                    if (dep_info.reads_rs1 && dep_info.rs1 == info.rd && dep_info.rs1_type == info.rd_type)
                        is_dependent = true;
                    if (dep_info.reads_rs2 && dep_info.rs2 == info.rd && dep_info.rs2_type == info.rd_type)
                        is_dependent = true;
                    if (dep_info.reads_rs3 && dep_info.rs3 == info.rd && dep_info.rs3_type == info.rd_type)
                        is_dependent = true;
                    
                    if (is_dependent) {
                        int forward_bonus = (is_load(block[best_candidate])) ? 0 : 1;
                        ready_cycle[j] = std::max(ready_cycle[j], completion_cycle - forward_bonus);
                    }
                }
            }
            
            current_cycle++;
        } 
        else {
            std::cerr << "ERROR: No schedulable instruction found!" << std::endl;
            break;
        }
    }
    
    return out;
}

// This divides the entire program into basic blocks and applies scheduling to each block.
// Blocks are formed based on control flow and static branch-target detection.
std::vector<uint32_t> ScheduleBasicBlocks(const std::vector<uint32_t>& instrs) {
    if (instrs.empty()) return {};
    
    std::unordered_set<size_t> branch_targets = find_branch_targets(instrs);
    
    std::vector<uint32_t> out;
    out.reserve(instrs.size());
    
    size_t i = 0;
    size_t N = instrs.size();
    
    while (i < N) {
        size_t start = i;
        size_t end = start;
        
        while (end < N) {
            if (end > start && branch_targets.count(end)) {
                break;
            }
            uint32_t instr = instrs[end];
            end++;
            if (is_control_flow(instr)) {
                break;
            }
        }
        
        if (start >= N || end > N || start >= end) {
            std::cerr << "ERROR: Invalid block bounds [" << start << ", " << end 
                      << ") for instruction count " << N << std::endl;
            i = end;
            continue;
        }
        
        std::vector<uint32_t> block;
        block.reserve(end - start);
        
        for (size_t idx = start; idx < end && idx < N; ++idx) {
            block.push_back(instrs[idx]);
        }
        
        if (block.empty()) {
            i = end;
            continue;
        }
        
        std::vector<uint32_t> scheduled_block = schedule_block(block);
        
        if (scheduled_block.size() != block.size()) {
            std::cerr << "ERROR: Scheduling changed instruction count (original=" 
                      << block.size() << ", scheduled=" << scheduled_block.size() << ")" << std::endl;
            out.insert(out.end(), block.begin(), block.end());
        } else {
            out.insert(out.end(), scheduled_block.begin(), scheduled_block.end());
        }
        
        i = end;
    }
    
    if (out.size() != instrs.size()) {
        std::cerr << "CRITICAL ERROR: Instruction count mismatch (original=" 
                  << instrs.size() << ", scheduled=" << out.size() << ")" << std::endl;
        return instrs;
    }
    
    return out;
}

// This prints a summary comparing original and scheduled code,
// including cycle estimates and number of reordered instructions.
void PrintSchedulingInfo(const std::vector<uint32_t>& original,
                        const std::vector<uint32_t>& scheduled) {

    std::cout << "\n=== Instruction Scheduling Results ===" << std::endl;
    std::cout << "Original instruction count: " << original.size() << std::endl;
    std::cout << "Scheduled instruction count: " << scheduled.size() << std::endl;
    
    size_t reordered = 0;
    size_t fp_instrs = 0;
    int original_cycles = 0;
    int scheduled_cycles = 0;
    
    for (size_t i = 0; i < std::min(original.size(), scheduled.size()); ++i) {
        if (original[i] != scheduled[i]) reordered++;
        if (is_fp_instruction(original[i])) fp_instrs++;
        original_cycles += get_execution_latency(original[i]);
        scheduled_cycles += get_execution_latency(scheduled[i]);
    }
    
    std::cout << "Instructions reordered: " << reordered << std::endl;
    std::cout << "Reorder percentage: "
              << (original.size() > 0 ? (100.0 * reordered / original.size()) : 0.0)
              << "%" << std::endl;
    std::cout << "Floating-point instructions: " << fp_instrs << std::endl;
    /*std::cout << "Estimated cycles (original): " << original_cycles << std::endl;
    std::cout << "Estimated cycles (scheduled): " << scheduled_cycles << std::endl;
    
    if (scheduled_cycles > 0 && original_cycles > scheduled_cycles) {
        std::cout << "Estimated speedup: "
                  << (100.0 * (original_cycles - scheduled_cycles) / original_cycles)
                  << "%" << std::endl;
    } 
    else if (scheduled_cycles > original_cycles) {
        std::cout << "Estimated slowdown: "
                  << (100.0 * (scheduled_cycles - original_cycles) / original_cycles)
                  << "%" << std::endl;
    } 
    else {
        std::cout << "No change in estimated cycles" << std::endl;
    }*/
}

} // namespace rvss
