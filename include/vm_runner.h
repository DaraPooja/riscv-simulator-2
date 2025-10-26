/**
 * @file vm_runner.h
 * @brief This file contains the declaration of the VMRunner class
 * @author Vishank Singh, https://github.com/VishankSingh
 */

#ifndef VM_RUNNER_H
#define VM_RUNNER_H

#include "vm/vm_base.h"
#include "vm/rvss/rvss_vm.h"
#include "vm/rvss/rvss_pipeline_vm.h"
#include "config.h"
#include "vm_asm_mw.h"

#include <memory>
#include <stdexcept>

/**
 * Create VM instance based on vm_config::config.getVmType()
 */
inline std::unique_ptr<VmBase> createVM(vm_config::VmTypes vmType) {
    if (vmType == vm_config::VmTypes::SINGLE_STAGE) {
        return std::make_unique<RVSSVM>();
    }
    // config.h uses MULTI_STAGE for pipelined mode
    if (vmType == vm_config::VmTypes::MULTI_STAGE) {
        return std::make_unique<RVSSPipelineVM>();
    }
    return nullptr;
}

class VMRunner {
public:
    std::unique_ptr<VmBase> vm_;

    VMRunner() {
        // Use the global config instance defined in config.h
        vm_config::VmTypes vmType = vm_config::config.getVmType();
        vm_ = createVM(vmType);
        if (!vm_) throw std::runtime_error("Failed to create VM instance");
    }

    ~VMRunner() = default;

    // Provide the VM pointer to callers (main.cpp expects GetVM())
    VmBase* GetVM() {
        return vm_.get();
    }

    void LoadProgram(const AssembledProgram &program) { vm_->LoadProgram(program); }

    void Custom() {
        if (auto vmInstance = dynamic_cast<RVSSVM *>(vm_.get())) vmInstance->PrintType();
        else if (auto vmPipeline = dynamic_cast<RVSSPipelineVM *>(vm_.get())) vmPipeline->PrintType();
        else throw std::runtime_error("VM not initialized.");
    }

    void Step() { vm_->Step(); }

    
};

#endif // VM_RUNNER_H

