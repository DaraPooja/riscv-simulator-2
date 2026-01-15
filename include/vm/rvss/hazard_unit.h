#ifndef HAZARD_UNIT_H
#define HAZARD_UNIT_H

#include <cstdint>

// Forward declaration
class RVSSPipelineVM;

class HazardUnit {
public:
    explicit HazardUnit(RVSSPipelineVM* vm = nullptr)
        : vm_(vm), stalled_(false), stall_IF_(false), stall_ID_(false) {}

    void SetVM(RVSSPipelineVM* vm) { vm_ = vm; }
    void Reset();
    void Flush();

    // Check hazard
    bool CheckDataHazard();

    // For pipeline IF_stage
    bool ShouldStall() const { return stalled_; }

    // Insert bubble
    void ApplyStall();

private:
    RVSSPipelineVM* vm_;
    bool stalled_;
    bool stall_IF_;
    bool stall_ID_;
};

#endif // HAZARD_UNIT_H
