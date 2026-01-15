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

## Project Highlights

- Extended the original **single-cycle RISC-V simulator** to a **multi-stage pipelined simulator**.
- Implemented multiple pipeline configurations including:
  - Basic pipelining
  - Hazard detection
  - Data forwarding
  - Static and dynamic branch prediction
- Added support for **instruction scheduling** and **cycle-accurate debugging**.
- Designed the simulator to be configurable across different execution modes at runtime.



## License
This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for more details.


## References
- [RISC-V Specifications](https://riscv.org/specifications/)
- [Five EmbedDev ISA manual](https://five-embeddev.com/riscv-isa-manual/)