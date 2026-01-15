#include "vm/rvss/branch_target_buffer.h"
#include <cmath>
#include <iostream>

BranchTargetBuffer::BranchTargetBuffer(uint32_t size)
    : table_size(size), entries(size) {
    // Initialize each BTB entry as invalid with zeroed fields.
    for (auto &e : entries) {
        e.valid = false;
        e.tag = 0;
        e.target = 0;
        e.is_jalr = false;
    }
}

uint32_t BranchTargetBuffer::get_index(uint32_t pc) const {
    // Index is based on middle bits of the PC (after removing 2 LSBs for alignment).
    // Bitmasking is used since the table size is assumed to be a power of two.
    return (pc >> 2) & (table_size - 1);
}

uint32_t BranchTargetBuffer::get_tag(uint32_t pc) const {
    // Tag consists of higher PC bits above the index field.
    // The shift accounts for removed low bits and index width.
    uint32_t shift = 2 + static_cast<int>(std::log2(table_size));
    return pc >> shift;
}

bool BranchTargetBuffer::hit(uint32_t pc) const {
    // Every BTB lookup is counted as an access.
    total_accesses++;

    uint32_t index = get_index(pc);
    uint32_t tag = get_tag(pc);
    const BTBEntry &entry = entries[index];
    
    // A hit occurs only if the entry is valid and the tag matches.
    bool is_hit = entry.valid && entry.tag == tag;
    
    if (is_hit) {
        total_hits++;
        // Count JALR hits separately to analyze indirect branch behavior.
        if (entry.is_jalr) {
            jalr_hits++;
        }
    }
    
    return is_hit;
}

uint32_t BranchTargetBuffer::get_target(uint32_t pc) const {
    // Returns the stored target for this PC’s BTB entry.
    uint32_t index = get_index(pc);
    return entries[index].target;
}

bool BranchTargetBuffer::is_jalr_entry(uint32_t pc) const {
    // Checks if the BTB entry corresponds to a JALR (indirect jump).
    uint32_t index = get_index(pc);
    return entries[index].valid && entries[index].is_jalr;
}

void BranchTargetBuffer::update(uint32_t pc, uint32_t target, bool is_jalr) {
    uint32_t index = get_index(pc);
    uint32_t tag = get_tag(pc);

    // Count how many times JALR entries are inserted or updated.
    if (is_jalr) {
        jalr_accesses++;
    }
    
    // Update the BTB entry with new information.
    entries[index].valid = true;
    entries[index].tag = tag;
    entries[index].target = target;
    entries[index].is_jalr = is_jalr;
}

void BranchTargetBuffer::print_stats() const {
    std::cout << "\n=== Branch Target Buffer Stats ===" << std::endl;
    std::cout << "Total BTB accesses: " << total_accesses << std::endl;
    std::cout << "BTB hits:           " << total_hits << std::endl;

    if (total_accesses > 0) {
        double hit_rate = 100.0 * total_hits / total_accesses;
        std::cout << "BTB hit rate:       " << hit_rate << "%" << std::endl;
    } else {
        std::cout << "BTB hit rate:       0%" << std::endl;
    }

    // JALR stats give insight into indirect branch performance.
    if (jalr_accesses > 0) {
        std::cout << "\nJALR-specific stats:" << std::endl;
        std::cout << "JALR accesses:      " << jalr_accesses << std::endl;
        std::cout << "JALR hits:          " << jalr_hits << std::endl;
        double jalr_rate = 100.0 * jalr_hits / jalr_accesses;
        std::cout << "JALR hit rate:      " << jalr_rate << "%" << std::endl;
    }
}
