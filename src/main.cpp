#include "main.h"
#include "assembler/assembler.h"
#include "utils.h"
#include "globals.h"
#include "vm/rvss/rvss_vm.h"
#include "vm/rvss/rvss_pipeline_vm.h"
#include "vm/rvss/instruction_scheduler.h"
#include "vm_runner.h"
#include "command_handler.h"
#include "config.h"

#include <iostream>
#include <thread>
#include <bitset>
#include <regex>

int main(int argc, char *argv[]) {
    int vm_mode = 0;  // default mode: 0=single-cycle, 1=pipeline, 2=pipeline+hazard
    bool dump_regs = false;
    bool enable_schedule = false; // for instruction scheduling


    setupVmStateDirectory();
    AssembledProgram program;
    RVSSVM vm;
    RVSSPipelineVM pipeline_vm;

    if (argc <= 1) {
        std::cerr << "No arguments provided. Use --help for usage information.\n";
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --help, -h           Show this help message\n"
                      << "  --assemble <file>    Assemble the specified file\n"
                      << "  --run <file>         Run the specified file\n"
                      << "  --verbose-errors     Enable verbose error printing\n"
                      << "  --start-vm           Start the VM with the default program\n"
                      << "  --start-vm --vm-as-backend  Start VM in backend mode\n"
                      << "  --mode <0|1|2>       Set VM mode (0=single-cycle, 1=basic pipeline, 2=pipeline+hazard)\n"
                      << "  --dump-regs          Dump registers after execution\n"
                      << "  --debug              Enable debug logging to pipeline_debug.log\n"
                      << "  --show-regs          Show register values after execution\n";
            return 0;

        } else if (arg == "--assemble") {
            if (++i >= argc) {
                std::cerr << "Error: No file specified for assembly.\n";
                return 1;
            }
            try {
                AssembledProgram program = assemble(argv[i]);
                std::cout << "Assembled program: " << program.filename << '\n';
                return 0;
            } catch (const std::runtime_error& e) {
                std::cerr << e.what() << '\n';
                return 1;
            }

        } else if (arg == "--run") {
            if (++i >= argc) {
                std::cerr << "Error: No file specified to run.\n";
                return 1;
            }
            try {
                AssembledProgram program = assemble(argv[i]);
                RVSSVM vm;
                vm.LoadProgram(program);
                vm.Run();
                std::cout << "Program running: " << program.filename << '\n';
                return 0;
            } catch (const std::runtime_error& e) {
                std::cerr << e.what() << '\n';
                return 1;
            }

        } else if (arg == "--verbose-errors") {
            globals::verbose_errors_print = true;
            std::cout << "Verbose error printing enabled.\n";

        } else if (arg == "--vm-as-backend") {
            globals::vm_as_backend = true;
            std::cout << "VM backend mode enabled.\n";
            
        } else if (arg == "--start-vm") {
            break;

        } else if (arg == "--mode") {
   if (++i >= argc) {
       std::cerr << "Error: No mode specified.\n";
       return 1;
   }
   try {
       vm_mode = std::stoi(argv[i]);
       if (vm_mode < 0 || vm_mode > 7) {
           std::cerr << "Error: Invalid mode. Use 0–5.\n";
           return 1;
       }

       if (vm_mode == 2) {
    pipeline_vm.enable_hazard_detection_ = true;
    std::cout << "Hazard detection enabled.\n";
} else if (vm_mode == 3) {
    pipeline_vm.enable_hazard_detection_ = true;
    pipeline_vm.EnableForwarding(true);
    std::cout << "Forwarding enabled.\n";
} else if (vm_mode == 4) {
    pipeline_vm.enable_hazard_detection_ = true;
    pipeline_vm.EnableForwarding(true);
    pipeline_vm.enable_branch_prediction_ = true;
    pipeline_vm.SetBranchPredictor(BranchPredictor::BACKWARD_TAKEN_FORWARD_NOT_TAKEN);
    std::cout << "Static branch prediction enabled (Backward Taken / Forward Not Taken).\n";
} else if (vm_mode == 5) {
    pipeline_vm.enable_hazard_detection_ = true;
    pipeline_vm.EnableForwarding(true);
    pipeline_vm.enable_branch_prediction_ = true;
    // Set 1-bit predictor with 1024 entries (we can change size as we like)
    pipeline_vm.SetBranchPredictor(BranchPredictor::ONE_BIT, 1024);
    std::cout << "Dynamic 1-bit branch prediction enabled (table=1024 entries).\n";
}else if (vm_mode == 6) {
    pipeline_vm.enable_hazard_detection_ = true;
    pipeline_vm.EnableForwarding(true);
    pipeline_vm.enable_branch_prediction_ = true;
    pipeline_vm.enable_btb_ = false; // ensure BTB disabled
    pipeline_vm.SetBranchPredictor(BranchPredictor::TWO_BIT, 1024);
    std::cout << "Dynamic 2-bit branch prediction enabled (table=1024 entries).\n";
}
else if (vm_mode == 7) {
    pipeline_vm.enable_hazard_detection_ = true;
    pipeline_vm.EnableForwarding(true);
    pipeline_vm.enable_branch_prediction_ = true;
    pipeline_vm.enable_btb_ = true; // ensure BTB enabled
    pipeline_vm.SetBranchPredictor(BranchPredictor::TWO_BIT, 1024);
    std::cout << "Dynamic 2-bit branch prediction + Branch Target Buffer (BTB) enabled.\n";
}


std::cout << "VM mode set to: "
          << (vm_mode == 0 ? "single-cycle" :
             (vm_mode == 1 ? "basic pipeline" :
             (vm_mode == 2 ? "pipeline with hazard detection" :
             (vm_mode == 3 ? "pipeline with forwarding" :
             (vm_mode == 4 ? "pipeline with static branch prediction" :
             (vm_mode == 5 ? "pipeline with dynamic 1-bit branch prediction" :
             (vm_mode == 6 ? "pipeline with dynamic 2-bit branch prediction" :
                              "pipeline with dynamic 2-bit branch prediction + BTB")))))))
                 << std::endl;


   } catch (...) {
       std::cerr << "Error: Invalid mode value.\n";
       return 1;
   }
}else if (arg == "--schedule") {
    enable_schedule = true;
    pipeline_vm.enable_instruction_scheduling_ = true;
    std::cout << "Instruction scheduling (basic-block optimization) enabled.\n";
}else if (arg == "--debug") {
    pipeline_vm.debug_mode_ = true;
    std::cout << "Debug mode enabled.Cycle details will be logged to pipeline_debug.log\n";
    
} else if (arg == "--show-regs") {
    dump_regs = true;
    std::cout << "Will show registers after execution.\n";
}
else if (arg == "--dump-regs") {
            dump_regs = true;
            std::cout << "Register dump enabled.\n";
            
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            return 1;
        }
    }

    std::cout << "Running in "
    << (vm_mode == 0 ? "single-cycle" :
       (vm_mode == 1 ? "basic pipeline" :
       (vm_mode == 2 ? "pipeline with hazard detection" :
       (vm_mode == 3 ? "pipeline with forwarding" :
       (vm_mode == 4 ? "pipeline with static branch prediction" :
       (vm_mode == 5 ? "pipeline with dynamic 1-bit branch prediction" :
       (vm_mode == 6 ? "pipeline with dynamic 2-bit branch prediction" :
                       "pipeline with dynamic 2-bit branch prediction + BTB")))))))
    << " mode" << std::endl;




    std::thread vm_thread;
    bool vm_running = false;

    auto launch_vm_thread = [&](auto fn) {
        if (vm_thread.joinable()) {
            if (vm_mode == 0) {
                vm.RequestStop();
            } else {
                pipeline_vm.RequestStop();
            }
            vm_thread.join();
        }
        vm_running = true;
        vm_thread = std::thread([&]() {
            fn();
            vm_running = false;
        });
    };

    std::string command_buffer;
    while (true) {
        std::getline(std::cin, command_buffer);
        command_handler::Command command = command_handler::ParseCommand(command_buffer);

        if (command.type == command_handler::CommandType::MODIFY_CONFIG) {
            if (command.args.size() != 3) {
                std::cout << "VM_MODIFY_CONFIG_ERROR" << std::endl;
                continue;
            }
            try {
                vm_config::config.modifyConfig(command.args[0], command.args[1], command.args[2]);
                std::cout << "VM_MODIFY_CONFIG_SUCCESS" << std::endl;
            } catch (const std::exception &e) {
                std::cout << "VM_MODIFY_CONFIG_ERROR" << std::endl;
                std::cerr << e.what() << '\n';
                continue;
            }
            continue;
        }
        
        if (command.type == command_handler::CommandType::LOAD) {
            try {
                program = assemble(command.args[0]);
                std::cout << "VM_PARSE_SUCCESS" << std::endl;
                
                // Load into the appropriate VM based on mode
                if (vm_mode == 0) {
                    vm.LoadProgram(program);
                    vm.output_status_ = "VM_PARSE_SUCCESS";
                    vm.DumpState(globals::vm_state_dump_file_path);
                } else {
                    // LoadProgram will handle scheduling if enable_instruction_scheduling_ is true
                    pipeline_vm.LoadProgram(program);
                    pipeline_vm.output_status_ = "VM_PARSE_SUCCESS";
                    pipeline_vm.DumpState(globals::vm_state_dump_file_path);
                }
                
                std::cout << "Program loaded: " << command.args[0] << std::endl;
                
            } catch (const std::runtime_error &e) {
                std::cout << "VM_PARSE_ERROR" << std::endl;
                if (vm_mode == 0) {
                    vm.output_status_ = "VM_PARSE_ERROR";
                    vm.DumpState(globals::vm_state_dump_file_path);
                } else {
                    pipeline_vm.output_status_ = "VM_PARSE_ERROR";
                    pipeline_vm.DumpState(globals::vm_state_dump_file_path);
                }
                std::cerr << e.what() << '\n';
                continue;
            }
        } else if (command.type == command_handler::CommandType::RUN) {
            // Check if we're resuming from a breakpoint in pipeline mode
            if (vm_mode >= 1 && pipeline_vm.paused_at_breakpoint_ && !vm_running) {
                // Resume directly without launching new thread
                std::cout << "Resuming from breakpoint at PC = 0x"
                          << std::hex << pipeline_vm.paused_pc_ << std::dec << std::endl;
                
                launch_vm_thread([&]() {
                    bool finished = pipeline_vm.ResumePipeline();
                    
                    if (finished) {
                        std::cout << "VM_PROGRAM_END" << std::endl;
                        pipeline_vm.output_status_ = "VM_PROGRAM_END";
                        pipeline_vm.DumpRegisters();
                        pipeline_vm.DumpState(globals::vm_state_dump_file_path);
                    } else if (pipeline_vm.paused_at_breakpoint_) {
                        std::cout << "VM_BREAKPOINT_HIT " << std::hex << pipeline_vm.paused_pc_ << std::dec << std::endl;
                        pipeline_vm.output_status_ = "VM_BREAKPOINT_HIT";
                        pipeline_vm.DumpRegisters();
                        pipeline_vm.DumpState(globals::vm_state_dump_file_path);
                    } else {
                        std::cout << "VM_STOPPED" << std::endl;
                        pipeline_vm.output_status_ = "VM_STOPPED";
                        pipeline_vm.DumpState(globals::vm_state_dump_file_path);
                    }
                });
            } else {
                // Normal run (not resuming from breakpoint)
                launch_vm_thread([&]() {
                    if (vm_mode == 0) {
                        // Single-cycle mode
                        vm.Run();
                    } else {
                        // Pipeline mode 
                        bool finished = pipeline_vm.RunPipeline();

                        if (finished) {
                            std::cout << "VM_PROGRAM_END" << std::endl;
                            pipeline_vm.output_status_ = "VM_PROGRAM_END";
                            pipeline_vm.DumpRegisters();
                            pipeline_vm.DumpState(globals::vm_state_dump_file_path);
                        } else if (pipeline_vm.paused_at_breakpoint_) {
                            std::cout << "VM_BREAKPOINT_HIT " << std::hex << pipeline_vm.paused_pc_ << std::dec << std::endl;
                            pipeline_vm.output_status_ = "VM_BREAKPOINT_HIT";
                            pipeline_vm.DumpRegisters();
                            pipeline_vm.DumpState(globals::vm_state_dump_file_path);
                        } else {
                            std::cout << "VM_STOPPED" << std::endl;
                            pipeline_vm.output_status_ = "VM_STOPPED";
                            pipeline_vm.DumpState(globals::vm_state_dump_file_path);
                        }
                    }
                });
            }

        } else if (command.type == command_handler::CommandType::DEBUG_RUN) {
            if (vm_mode >= 1) {
                std::cout << "DEBUG_RUN not supported in pipeline mode. Use RUN with breakpoints instead." << std::endl;
                continue;
            }
            launch_vm_thread([&]() { vm.DebugRun(); });

        } else if (command.type == command_handler::CommandType::STOP) {
            if (vm_mode == 0) {
                vm.RequestStop();
                vm.output_status_ = "VM_STOPPED";
                vm.DumpState(globals::vm_state_dump_file_path);
            } else {
                pipeline_vm.RequestStop();
                pipeline_vm.output_status_ = "VM_STOPPED";
                pipeline_vm.DumpState(globals::vm_state_dump_file_path);
            }
            std::cout << "VM_STOPPED" << std::endl;

        } else if (command.type == command_handler::CommandType::STEP) {
            if (vm_running) continue;
            
            if (vm_mode >= 1) {
                std::cout << "STEP not supported in pipeline mode. Use RUN with breakpoints instead." << std::endl;
                continue;
            }
            launch_vm_thread([&]() { vm.Step(); });

        } else if (command.type == command_handler::CommandType::UNDO) {
            if (vm_running) continue;
            
            if (vm_mode >= 1) {
                std::cout << "UNDO not supported in pipeline mode." << std::endl;
                continue;
            }
            vm.Undo();

        } else if (command.type == command_handler::CommandType::REDO) {
            if (vm_running) continue;
            
            if (vm_mode >= 1) {
                std::cout << "REDO not supported in pipeline mode." << std::endl;
                continue;
            }
            vm.Redo();

        } else if (command.type == command_handler::CommandType::RESET) {
            if (vm_mode == 0) {
                vm.Reset();
            } else {
                pipeline_vm.Reset();
                if (vm_mode == 3) {
                    pipeline_vm.enable_forwarding_ = true;
                    pipeline_vm.enable_hazard_detection_ = true; 
                }
                
            }

        } else if (command.type == command_handler::CommandType::EXIT) {
            if (vm_mode == 0) {
                vm.RequestStop();
            } else {
                pipeline_vm.RequestStop();
            }
            
            if (vm_thread.joinable()) vm_thread.join();
            
            if (vm_mode == 0) {
                vm.output_status_ = "VM_EXITED";
                vm.DumpState(globals::vm_state_dump_file_path);
            } else {
                pipeline_vm.output_status_ = "VM_EXITED";
                pipeline_vm.DumpState(globals::vm_state_dump_file_path);
            }
            break;

        } else if (command.type == command_handler::CommandType::ADD_BREAKPOINT) {
            unsigned long bp = std::stoul(command.args[0], nullptr, 10);
            if (vm_mode == 0) {
                vm.AddBreakpoint(bp);
            } else {
                pipeline_vm.AddBreakpoint(bp);
            }
            std::cout << "Breakpoint added at line/address " << bp << std::endl;

        } else if (command.type == command_handler::CommandType::REMOVE_BREAKPOINT) {
            unsigned long bp = std::stoul(command.args[0], nullptr, 10);
            if (vm_mode == 0) {
                vm.RemoveBreakpoint(bp);
            } else {
                pipeline_vm.RemoveBreakpoint(bp);
            }

        } else if (command.type == command_handler::CommandType::MODIFY_REGISTER) {
            try {
                if (command.args.size() != 2) {
                    std::cout << "VM_MODIFY_REGISTER_ERROR" << std::endl;
                    continue;
                }
                std::string reg_name = command.args[0];
                uint64_t value = std::stoull(command.args[1], nullptr, 16);
                
                if (vm_mode == 0) {
                    vm.ModifyRegister(reg_name, value);
                    DumpRegisters(globals::registers_dump_file_path, vm.registers_);
                } else {
                    pipeline_vm.ModifyRegister(reg_name, value);
                    DumpRegisters(globals::registers_dump_file_path, pipeline_vm.registers_);
                }
                
                std::cout << "VM_MODIFY_REGISTER_SUCCESS" << std::endl;
            } catch (...) {
                std::cout << "VM_MODIFY_REGISTER_ERROR" << std::endl;
                continue;
            }

        } else if (command.type == command_handler::CommandType::GET_REGISTER) {
            std::string reg_str = command.args[0];
            int idx = 0;
            if (reg_str.size() > 0 && reg_str[0] == 'x') {
                idx = std::stoi(reg_str.substr(1));
            }
            
            uint64_t reg_value = 0;
            if (vm_mode == 0) {
                reg_value = vm.registers_.ReadGpr(idx);
            } else {
                reg_value = pipeline_vm.registers_.ReadGpr(idx);
            }
            
            std::cout << "VM_REGISTER_VAL_START0x" << std::hex << reg_value 
                      << "VM_REGISTER_VAL_END" << std::dec << std::endl;

        } else if (command.type == command_handler::CommandType::MODIFY_MEMORY) {
            if (command.args.size() != 3) {
                std::cout << "VM_MODIFY_MEMORY_ERROR" << std::endl;
                continue;
            }
            try {
                uint64_t address = std::stoull(command.args[0], nullptr, 16);
                std::string type = command.args[1];
                uint64_t value = std::stoull(command.args[2], nullptr, 16);

                auto& mem_ctrl = (vm_mode == 0) ? vm.memory_controller_ : pipeline_vm.memory_controller_;
                
                if (type == "byte") mem_ctrl.WriteByte(address, static_cast<uint8_t>(value));
                else if (type == "half") mem_ctrl.WriteHalfWord(address, static_cast<uint16_t>(value));
                else if (type == "word") mem_ctrl.WriteWord(address, static_cast<uint32_t>(value));
                else if (type == "double") mem_ctrl.WriteDoubleWord(address, value);
                else { 
                    std::cout << "VM_MODIFY_MEMORY_ERROR" << std::endl; 
                    continue; 
                }

                std::cout << "VM_MODIFY_MEMORY_SUCCESS" << std::endl;
            } catch (...) {
                std::cout << "VM_MODIFY_MEMORY_ERROR" << std::endl;
                continue;
            }

        } else if (command.type == command_handler::CommandType::DUMP_MEMORY) {
            try {
                if (vm_mode == 0) {
                    vm.memory_controller_.DumpMemory(command.args);
                } else {
                    pipeline_vm.memory_controller_.DumpMemory(command.args);
                }
            } catch (...) { 
                std::cout << "VM_MEMORY_DUMP_ERROR" << std::endl; 
                continue; 
            }

        } else if (command.type == command_handler::CommandType::PRINT_MEMORY) {
            auto& mem_ctrl = (vm_mode == 0) ? vm.memory_controller_ : pipeline_vm.memory_controller_;
            
            for (size_t i = 0; i < command.args.size(); i += 2) {
                uint64_t address = std::stoull(command.args[i], nullptr, 16);
                uint64_t rows = std::stoull(command.args[i + 1]);
                mem_ctrl.PrintMemory(address, rows);
            }
            std::cout << std::endl;

        } else if (command.type == command_handler::CommandType::GET_MEMORY_POINT) {
            if (command.args.size() != 1) {
                std::cout << "VM_GET_MEMORY_POINT_ERROR" << std::endl;
                continue;
            }
            
            if (vm_mode == 0) {
                vm.memory_controller_.GetMemoryPoint(command.args[0]);
            } else {
                pipeline_vm.memory_controller_.GetMemoryPoint(command.args[0]);
            }

        } else if (command.type == command_handler::CommandType::VM_STDIN) {
            if (vm_mode == 0) {
                vm.PushInput(command.args[0]);
            } else {
                pipeline_vm.PushInput(command.args[0]);
            }

        } else if (command.type == command_handler::CommandType::DUMP_CACHE) {
            std::cout << "Cache dumped." << std::endl;

        } else {
            std::cout << "Invalid command: " << command_buffer << std::endl;
        }
    }

    return 0;
}