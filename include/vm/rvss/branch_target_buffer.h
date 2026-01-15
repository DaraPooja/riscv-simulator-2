#ifndef BRANCH_TARGET_BUFFER_H
#define BRANCH_TARGET_BUFFER_H

#include <cstdint>
#include <vector>

struct BTBEntry {
    bool valid;
    uint32_t tag;
    uint32_t target;
    bool is_jalr;

    BTBEntry() : valid(false), tag(0), target(0), is_jalr(false) {}
};

class BranchTargetBuffer {
public:
    explicit BranchTargetBuffer(uint32_t size = 256);
    
    // Check if PC has a valid BTB entry (also tracks stats)
    bool hit(uint32_t pc) const;
    
    // Get predicted target address for PC
    uint32_t get_target(uint32_t pc) const;
    
    // Update BTB when branch resolves
    void update(uint32_t pc, uint32_t target, bool is_jalr=false);
    bool is_jalr_entry(uint32_t pc) const;

    
    
    void print_stats() const;

private:
    // Helper: calculate index & tag
    uint32_t get_index(uint32_t pc) const;
    uint32_t get_tag(uint32_t pc) const;

    uint32_t table_size;
    std::vector<BTBEntry> entries;
    
    mutable uint64_t total_accesses = 0;
    mutable uint64_t total_hits = 0;
    mutable uint64_t jalr_accesses = 0;   
    mutable uint64_t jalr_hits = 0; 
    
    
};

#endif