/**
 * @file vm_base.cpp
 * @brief File containing the base class for the virtual machine
 * @author Vishank Singh
 */

 #include "vm/vm_base.h"
 #include "globals.h"
 #include "config.h"
 
 #include <cstdint>
 #include <iostream>
 #include <iomanip>
 #include <fstream>
 #include <filesystem>
 #include <algorithm>
 #include <cstring>
 #include <thread>
 
 void VmBase::LoadProgram(const AssembledProgram &program) {
     program_ = program;
     unsigned int counter = 0;
     for (const auto &instruction: program.text_buffer) {
         memory_controller_.WriteWord(counter, instruction);
         counter += 4;
     }
     program_size_ = counter;
     AddBreakpoint(program_size_, false);
 
     unsigned int data_counter = 0;
     uint64_t base_data_address = vm_config::config.getDataSectionStart();
     auto align = [&](unsigned int alignment) {
         if (data_counter % alignment != 0)
             data_counter += alignment - (data_counter % alignment);
     };
     for (const auto& data : program.data_buffer) {
         std::visit([&](auto&& value) {
             using T = std::decay_t<decltype(value)>; 
             if constexpr (std::is_same_v<T, uint8_t>) {
                 align(1);
                 memory_controller_.WriteByte(base_data_address + data_counter, value);
                 data_counter += 1;
             } else if constexpr (std::is_same_v<T, uint16_t>) {
                 align(2);
                 memory_controller_.WriteHalfWord(base_data_address + data_counter, value);
                 data_counter += 2;
             } else if constexpr (std::is_same_v<T, uint32_t>) {
                 align(4);
                 memory_controller_.WriteWord(base_data_address + data_counter, value);
                 data_counter += 4;
             } else if constexpr (std::is_same_v<T, uint64_t>) {
                 align(8);
                 memory_controller_.WriteDoubleWord(base_data_address + data_counter, value);
                 data_counter += 8;
             } else if constexpr (std::is_same_v<T, float>) {
                 align(4);
                 uint32_t float_as_int;
                 std::memcpy(&float_as_int, &value, sizeof(float));
                 memory_controller_.WriteWord(base_data_address + data_counter, float_as_int);
                 data_counter += 4;
             } else if constexpr (std::is_same_v<T, double>) {
                 align(8);
                 uint64_t double_as_int;
                 std::memcpy(&double_as_int, &value, sizeof(double));
                 memory_controller_.WriteDoubleWord(base_data_address + data_counter, double_as_int);
                 data_counter += 8;
             } else if constexpr (std::is_same_v<T, std::string>) {
                 align(1);
                 for (size_t i = 0; i < value.size(); i++) {
                     memory_controller_.WriteByte(base_data_address + data_counter, static_cast<uint8_t>(value[i]));
                     data_counter += 1;
                 }
             }
         }, data);
     }
 
     std::cout << "VM_PROGRAM_LOADED" << std::endl;
     output_status_ = "VM_PROGRAM_LOADED";
 
     DumpState(globals::vm_state_dump_file_path);
 }
 
 uint64_t VmBase::GetProgramCounter() const {
     return program_counter_;
 }
 
 void VmBase::UpdateProgramCounter(int64_t value) {
     program_counter_ = static_cast<uint64_t>(static_cast<int64_t>(program_counter_) + value);
 }
 
 auto sign_extend = [](uint32_t value, unsigned int bits) -> int32_t {
     int32_t mask = 1 << (bits - 1);
     return (value ^ mask) - mask;
 };
 
 int32_t VmBase::ImmGenerator(uint32_t instruction) {
     int32_t imm = 0;
     uint8_t opcode = instruction & 0b1111111;
     switch (opcode) {
         case 0b0010011: case 0b0000011: case 0b1100111: case 0b0001111: case 0b0000111:
             imm = (instruction >> 20) & 0xFFF;
             imm = sign_extend(imm, 12);
             break;
         case 0b0100011: case 0b0100111:
             imm = ((instruction >> 7) & 0x1F) | ((instruction >> 25) & 0x7F) << 5;
             imm = sign_extend(imm, 12);
             break;
         case 0b1100011:
             imm = ((instruction >> 8) & 0xF) | ((instruction >> 25) & 0x3F) << 4 | ((instruction >> 7) & 0x1) << 10 | ((instruction >> 31) & 0x1) << 11;
             imm <<= 1;
             imm = sign_extend(imm, 13);
             break;
         case 0b0110111: case 0b0010111:
             imm = (instruction & 0xFFFFF000) >> 12;
             break;
         case 0b1101111:
             imm = ((instruction >> 21) & 0x3FF) | ((instruction >> 20) & 0x1) << 10 | ((instruction >> 12) & 0xFF) << 11 | ((instruction >> 31) & 0x1) << 19;
             imm <<= 1;
             imm = sign_extend(imm, 21);
             break;
         case 0b0110011: case 0b1010011:
             imm = 0;
             break;
         default:
             imm = 0;
             break;
     }
     return imm;
 }
 
 void VmBase::AddBreakpoint(uint64_t val, bool is_line) {
     uint64_t address = 0;
 
     if (is_line) {
         // 🔹 Convert line number to instruction number
         auto it = program_.line_number_instruction_number_mapping.find(val);
         if (it == program_.line_number_instruction_number_mapping.end()) {
             std::cerr << "Invalid line number: " << val << std::endl;
             return;
         }
 
         unsigned int instr_num = it->second;
 
         // 🔹 Each instruction is 4 bytes wide
         address = instr_num * 4;
     } else {
         // Direct address given
         address = val;
     }
 
     if (std::find(breakpoints_.begin(), breakpoints_.end(), address) == breakpoints_.end()) {
         breakpoints_.push_back(address);
         std::cout << "Breakpoint added at address 0x" << std::hex << address << std::dec << std::endl;
     } else {
         std::cout << "Breakpoint already exists at address 0x" << std::hex << address << std::dec << std::endl;
     }
 }
 
 
 void VmBase::RemoveBreakpoint(uint64_t val, bool is_line) {
     uint64_t address = 0;
 
     if (is_line) {
         auto it = program_.line_number_instruction_number_mapping.find(val);
         if (it == program_.line_number_instruction_number_mapping.end()) {
             std::cerr << "Invalid line number: " << val << std::endl;
             return;
         }
 
         unsigned int instr_num = it->second;
         address = instr_num * 4;
     } else {
         address = val;
     }
 
     auto it = std::find(breakpoints_.begin(), breakpoints_.end(), address);
     if (it != breakpoints_.end()) {
         breakpoints_.erase(it);
         std::cout << "Breakpoint removed at address 0x" << std::hex << address << std::dec << std::endl;
     } else {
         std::cout << "No breakpoint found at address 0x" << std::hex << address << std::dec << std::endl;
     }
 }
 
 
 
 bool VmBase::CheckBreakpoint(uint64_t address) {
     return std::find(breakpoints_.begin(), breakpoints_.end(), address) != breakpoints_.end();
 }
 
 void VmBase::PrintString(uint64_t address) {
     while (true) {
         char c = memory_controller_.ReadByte(address);
         if (c == '\0') break;
         std::cout << c;
         address++;
     }
 }
 
 void VmBase::DumpState(const std::filesystem::path &filename) {
     std::ofstream file(filename);
     if (!file.is_open()) { std::cerr << "Error opening file: " << filename.string() << std::endl; return; }
 
     unsigned int instruction_number = program_counter_ / 4;
     unsigned int current_line = program_.instruction_number_line_number_mapping[instruction_number];
 
     file << "{\n";
     file << "    \"program_counter\": \"" << std::hex << std::setw(8) << std::setfill('0') << program_counter_ << "\",\n";
     file << "    \"current_line\": " << current_line << ",\n";
     file << "    \"current_instruction\": \"" << std::hex << std::setw(8) << std::setfill('0') << current_instruction_ << "\",\n";
     file << "    \"disassembly_line_number\": " << program_.instruction_number_disassembly_mapping[instruction_number] << ",\n";
     file << "    \"cycle_count\": " << cycle_s_ << ",\n";
     file << "    \"instructions_retired\": " << instructions_retired_ << ",\n";
     file << "    \"cpi\": " << cpi_ << ",\n";
     file << "    \"ipc\": " << ipc_ << ",\n";
     file << "    \"stall_cycles\": " << stall_cycles_ << ",\n";
     file << "    \"branch_mispredictions\": " << branch_mispredictions_ << ",\n";
     file << "    \"breakpoints\": [";
     for (size_t i = 1; i < breakpoints_.size(); ++i) {
         file << program_.instruction_number_line_number_mapping[breakpoints_[i] / 4];
         if (i < breakpoints_.size() - 1) file << ", ";
     }
     file << "],\n";
     file << "    \"output_status\": \"" << output_status_ << "\"\n";
     file << "}\n";
     file.close();
 }
 
 void VmBase::ModifyRegister(const std::string &reg_name, uint64_t value) {
     registers_.ModifyRegister(reg_name, value);
 }
 
 