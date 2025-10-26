/**
 * @file vm_base.h
 * @brief File containing the base class for the virtual machine
 * @author Vishank Singh, https://VishankSingh
 */
#ifndef VM_BASE_H
#define VM_BASE_H

#include "registers.h"
#include "memory_controller.h"
#include "alu.h"
#include "vm_asm_mw.h"

#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

enum SyscallCode {
    SYSCALL_PRINT_INT = 1,
    SYSCALL_PRINT_FLOAT = 2,
    SYSCALL_PRINT_DOUBLE = 3,
    SYSCALL_PRINT_STRING = 4,
    SYSCALL_EXIT = 10,
    SYSCALL_READ = 63,
    SYSCALL_WRITE = 64,
};

class VmBase {
public:
    VmBase() = default;
    virtual ~VmBase() = default;

    AssembledProgram program_;
    std::atomic<bool> stop_requested_ = false;
    std::mutex input_mutex_;
    std::condition_variable input_cv_;
    std::queue<std::string> input_queue_;
    std::vector<uint64_t> breakpoints_;

    uint32_t current_instruction_{};
    uint64_t program_counter_{};

    unsigned int cycle_s_{};
    unsigned int instructions_retired_{};
    float cpi_{};
    float ipc_{};
    unsigned int stall_cycles_{};
    unsigned int branch_mispredictions_{};

    std::string output_status_;

    MemoryController memory_controller_;
    RegisterFile registers_;
    alu::Alu alu_;

    uint64_t program_size_ = 0;

    virtual void Run() = 0;
    virtual void DebugRun() = 0;
    virtual void Step() = 0;
    virtual void Undo() = 0;
    virtual void Redo() = 0;
    virtual void Reset() = 0;

    // Declare functions only, no inline definitions
    virtual void LoadProgram(const AssembledProgram &program);
    virtual void ModifyRegister(const std::string &reg_name, uint64_t value);

    virtual void PushInput(const std::string& input) { 
        std::lock_guard<std::mutex> lock(input_mutex_);
        input_queue_.push(input);
        input_cv_.notify_one();
    }

    virtual void PrintType(){};
    virtual void RequestStop() { stop_requested_ = true; }

    uint64_t GetProgramCounter() const;
    void UpdateProgramCounter(int64_t value);
    int32_t ImmGenerator(uint32_t instruction);

    virtual void AddBreakpoint(uint64_t val, bool is_line = true); 
    virtual void RemoveBreakpoint(uint64_t val, bool is_line = true); 
    bool CheckBreakpoint(uint64_t address);
    void PrintString(uint64_t address);
    void DumpState(const std::filesystem::path &filename); 
};

#endif // VM_BASE_H

