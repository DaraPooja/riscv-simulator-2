#include "vm/rvss/branch_predictor.h"
#include <cassert>
#include <iostream>
#include <iomanip>

// Decodes the signed immediate used by B-type branch instructions.
// The immediate is stored in scattered bit positions in the instruction,
// so this reconstructs the 13-bit value and sign-extends it.
int32_t BranchPredictor::extractBranchImm(uint32_t instr) {
    int32_t imm = 0;
    imm |= ((instr >> 31) & 0x1) << 12;
    imm |= ((instr >> 7) & 0x1) << 11;
    imm |= ((instr >> 25) & 0x3F) << 5;
    imm |= ((instr >> 8) & 0xF) << 1;
    if (imm & 0x1000) imm |= 0xFFFFE000;   // sign extension
    return imm;
}

// Decodes the signed immediate used by J-type instructions (JAL).
// J-type immediates are 21 bits with an unusual bit layout.
int32_t BranchPredictor::extractJALImm(uint32_t instr) {
    int32_t imm = 0;
    imm |= ((instr >> 31) & 0x1) << 20;
    imm |= ((instr >> 12) & 0xFF) << 12;
    imm |= ((instr >> 20) & 0x1) << 11;
    imm |= ((instr >> 21) & 0x3FF) << 1;
    if (imm & 0x100000) imm |= 0xFFE00000;   // sign extension
    return imm;
}

BranchPredictor::BranchPredictor(Type t, size_t table_size)
    : type_(t), table_size_(table_size) {

    // For dynamic predictors, ensure table size is a power of two.
    if (type_ == ONE_BIT) {
        assert((table_size_ & (table_size_ - 1)) == 0);
        one_bit_table_.assign(table_size_, 0);   // initial prediction: not taken
    } 
    else if (type_ == TWO_BIT) {
        assert((table_size_ & (table_size_ - 1)) == 0);
        // Initialize with Weakly Not Taken (state 1).
        two_bit_table_.assign(table_size_, 1);
    }

    total_predictions_ = 0;
    correct_predictions_ = 0;
}

// Produces a prediction given the branch instruction and program counter.
// Returns both the taken/not-taken decision and the predicted target.
BranchPredictor::Prediction BranchPredictor::predict(uint64_t pc, uint32_t instr) const {
    Prediction p{false, 0};
    uint8_t opcode = instr & 0x7F;

    // JAL is an unconditional PC-relative jump.
    if (opcode == 0x6F) {
        int32_t imm = extractJALImm(instr);
        p.predicted_taken = true;
        p.predicted_target = pc + imm;
        return p;
    }
    
    // JALR is also an unconditional jump, but its target depends on a register,
    // so the predictor cannot compute the target without the BTB.
    if (opcode == 0x67) {
        p.predicted_taken = true;
        p.predicted_target = 0;   // unknown without BTB
        return p;
    }
    
    // Only conditional branches use dynamic/static prediction.
    if (opcode != 0x63) return p;

    int32_t imm = extractBranchImm(instr);
    uint64_t target = pc + imm;

    // Simple static "always not taken" policy.
    if (type_ == STATIC_NOT_TAKEN) {
        ++total_predictions_;
        p.predicted_taken = false;
        return p;
    }

    // Static policy: backward branches are predicted taken, forward not taken.
    if (type_ == BACKWARD_TAKEN_FORWARD_NOT_TAKEN) {
        ++total_predictions_;
        bool backward = ((int64_t)target < (int64_t)pc);
        if (backward) {
            p.predicted_taken = true;
            p.predicted_target = target;
        }
        return p;
    }

    // One-bit predictor: use the stored bit as the prediction.
    if (type_ == ONE_BIT) {
        size_t idx = OneBitIndex(pc);
        uint8_t bit = one_bit_table_[idx];
        p.predicted_taken = (bit != 0);
        if (p.predicted_taken) {
            p.predicted_target = target;
        }
        ++total_predictions_;
        return p;
    }
    
    // Two-bit predictor: use the 2-bit saturating counter.
    if (type_ == TWO_BIT) {
        size_t idx = OneBitIndex(pc);
        uint8_t state = two_bit_table_[idx];

        // States >= 2 mean predict taken.
        p.predicted_taken = (state >= 2);
        if (p.predicted_taken) {
            p.predicted_target = target;
        }
        
        ++total_predictions_;
        return p;
    }

    return p;
}

// Updates the predictor with the actual outcome of a conditional branch.
// This adjusts tables only for dynamic predictors.
void BranchPredictor::update(uint64_t pc, uint32_t instr, bool actual_taken) {
    uint8_t opcode = instr & 0x7F;
    if (opcode != 0x63) return;   // update only conditional branches

    // For static "not taken", only count correctness.
    if (type_ == STATIC_NOT_TAKEN) {
        if (!actual_taken) {
            ++correct_predictions_;
        }
        return;
    }
    
    // For backward-taken static predictor, compute what the static rule predicted.
    if (type_ == BACKWARD_TAKEN_FORWARD_NOT_TAKEN) {
        int32_t imm = extractBranchImm(instr);
        uint64_t target = pc + imm;
        bool predicted = ((int64_t)target < (int64_t)pc);
        if (predicted == actual_taken) {
            ++correct_predictions_;
        }
        return;
    }

    // One-bit predictor update.
    if (type_ == ONE_BIT) {
        size_t idx = OneBitIndex(pc);
        uint8_t &entry = one_bit_table_[idx];
        
        bool predicted_taken = (entry != 0);
        if (predicted_taken == actual_taken) {
            ++correct_predictions_;
        }
        
        // Overwrite bit with actual outcome.
        entry = actual_taken ? 1 : 0;
        return;
    }
    
    // Two-bit predictor update (saturating counter).
    if (type_ == TWO_BIT) {
        size_t idx = OneBitIndex(pc);
        uint8_t &state = two_bit_table_[idx];
    
        bool predicted_taken = (state >= 2);
        if (predicted_taken == actual_taken) {
            ++correct_predictions_;
        }
    
        // Move the state toward strongly taken or strongly not taken.
        if (actual_taken) {
            if (state < 3) state++;
        } else {
            if (state > 0) state--;
        }
        return;
    }
}

// Prints the accuracy and configuration of the branch predictor.
void BranchPredictor::PrintStats() const {
    std::cout << "[BranchPredictor] ";
    
    switch (type_) {
        case STATIC_NOT_TAKEN: 
            std::cout << "Static Not Taken"; 
            break;
        case BACKWARD_TAKEN_FORWARD_NOT_TAKEN: 
            std::cout << "Static Backward Taken / Forward Not Taken"; 
            break;
        case ONE_BIT: 
            std::cout << "Dynamic 1-bit"; 
            break;
        case TWO_BIT: 
            std::cout << "Dynamic 2-bit"; 
            break;
        default: 
            std::cout << "Unknown"; 
            break;
    }
    
    std::cout << " | Table Size: " << table_size_
              << " | Predictions: " << total_predictions_
              << " | Correct: " << correct_predictions_;

    if (total_predictions_ > 0) {
        double acc = 100.0 * correct_predictions_ / total_predictions_;
        std::cout << " | Accuracy: " << std::fixed 
                  << std::setprecision(2) << acc << "%";
    }

    std::cout << std::endl;
}
