#ifndef BRANCH_PREDICTOR_H
#define BRANCH_PREDICTOR_H

#include <cstdint>
#include <vector>
#include <iostream>

class BranchPredictor {
public:
    enum Type {
        STATIC_NOT_TAKEN = 0,
        BACKWARD_TAKEN_FORWARD_NOT_TAKEN = 1,
        ONE_BIT = 2,
        TWO_BIT = 3 
    };

    struct Prediction {
        bool predicted_taken;
        uint64_t predicted_target; // valid when predicted_taken == true
    };

    // Create predictor. table_size must be a power of two for ONE_BIT.
    BranchPredictor(Type t = BACKWARD_TAKEN_FORWARD_NOT_TAKEN, size_t table_size = 1024);

    // Predict branch outcome for instruction at PC (instr used to compute target for static predictors)
    Prediction predict(uint64_t pc, uint32_t instr) const;

    // Update predictor state after branch retired/resolved.
    void update(uint64_t pc, uint32_t instr, bool actual_taken);

    // Optional: print statistics (calls to this are safe)
    void PrintStats() const;

    // Expose type and configuration
    Type type() const { return type_; }
    size_t table_size() const { return table_size_; }

private:
    Type type_;
    size_t table_size_;

    // ONE-BIT table (0 = predict not taken, 1 = predict taken)
    std::vector<uint8_t> one_bit_table_; 
    std::vector<uint8_t> two_bit_table_; 

    // Counters for simple stats
    mutable uint64_t total_predictions_ = 0;
    mutable uint64_t correct_predictions_ = 0;

    // Helpers
    static int32_t extractBranchImm(uint32_t instr);
    static int32_t extractJALImm(uint32_t instr);
    inline size_t OneBitIndex(uint64_t pc) const { return (pc >> 2) & (table_size_ - 1); }
};

#endif // BRANCH_PREDICTOR_H
