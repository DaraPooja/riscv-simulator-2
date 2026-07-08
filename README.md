# RISC-V simulator

## Building the Project

The code base is written in C++17, to build the project use cmake. (You might want to use 
ninja for faster builds.)

## Usage

To run the simulator, use the following command:

```
./vm --start-vm
```

See [Commands](COMMANDS.md) for a list of commands.

## My Contributions

This project extends the original **single-cycle RISC-V simulator** into a configurable **cycle-accurate 5-stage pipelined processor simulator**.

### Pipeline Architecture
- Implemented a complete **5-stage pipeline** consisting of IF, ID, EX, MEM, and WB stages.
- Added inter-stage pipeline registers (IF/ID, ID/EX, EX/MEM, MEM/WB).
- Added multiple execution modes to compare different processor configurations.

### Hazard Handling
- Implemented RAW hazard detection.
- Added load-use hazard detection with pipeline stalling and bubble insertion.
- Added support for hazard detection in both integer and floating-point pipelines.

### Data Forwarding
- Implemented forwarding paths:
  - EX/MEM → EX
  - MEM/WB → EX
- Extended forwarding to floating-point instructions.
- Reduced unnecessary pipeline stalls by forwarding results whenever possible.

### Branch Prediction
Implemented multiple branch prediction strategies:

- Static Backward-Taken Forward-Not-Taken (BTFNT)
- Dynamic 1-bit branch predictor
- Dynamic 2-bit saturating counter predictor
- Branch Target Buffer (BTB) with JALR-aware prediction

### Floating-Point Pipeline
- Added support for the RISC-V Floating Point (F) Extension.
- Implemented floating-point forwarding.
- Added floating-point hazard detection.
- Supported floating-point arithmetic, comparisons, conversions, and fused multiply-add instructions (FMADD/FMSUB/FNMADD/FNMSUB).

### Instruction Scheduling
- Implemented basic-block instruction scheduling.
- Added dependency analysis.
- Reordered independent instructions to reduce load-use stalls and improve throughput.

### Debugging & Analysis
- Added configurable execution modes (`--mode`).
- Implemented cycle-by-cycle debugging (`--debug`).
- Added breakpoint support.
- Added execution statistics and pipeline tracing.

---

## Supported Execution Modes

| Mode | Description |
|------|-------------|
| 0 | Single-cycle processor |
| 1 | Basic 5-stage pipeline |
| 2 | Pipeline with hazard detection |
| 3 | Pipeline with data forwarding |
| 4 | Pipeline with static branch prediction |
| 5 | Pipeline with dynamic 1-bit branch prediction |
| 6 | Pipeline with dynamic 2-bit branch prediction |
| 7 | Pipeline with dynamic 2-bit branch prediction + Branch Target Buffer (BTB) |

---

## Acknowledgement

This project builds upon the original single-cycle RISC-V simulator provided as part of the Computer Architecture course at IIT Hyderabad.

The original simulator was developed by a senior student and was used with the permission of the course instructor.

The pipeline implementation, hazard handling, forwarding, branch prediction, floating-point support, instruction scheduling, debugging infrastructure, and execution modes described above were implemented as part of this extension.

---



## License
This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for more details.


## References
- [RISC-V Specifications](https://riscv.org/specifications/)
- [Five EmbedDev ISA manual](https://five-embeddev.com/riscv-isa-manual/)