#ifndef RVSS_INSTRUCTION_SCHEDULER_H
#define RVSS_INSTRUCTION_SCHEDULER_H

#include <cstdint>
#include <vector>

namespace rvss {

/// Schedule instructions organized as 32-bit words (little endian RISC-V encoding).
/// - Input: instrs: vector of 32-bit machine words (address 0,4,8,...)
/// - Returns: new vector with scheduled instructions (same length, same semantics).
std::vector<uint32_t> ScheduleBasicBlocks(const std::vector<uint32_t>& instrs);
void PrintSchedulingInfo(const std::vector<uint32_t>& original, 
    const std::vector<uint32_t>& scheduled);

} // namespace rvss


#endif // RVSS_INSTRUCTION_SCHEDULER_H
