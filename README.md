# RISC-V Full-System Simulator

这是一个轻量级、高性能的 RISC-V 32位全系统模拟器，支持 RV32IMAB 指令集，并集成了基于 Spike 的差异化测试 (Difftest) 以及 SimPoint 分析功能。

---

## 🚀 快速开始

### 1. 编译
确保您的系统中已安装 `g++` 和 `zlib` 开发库，然后直接运行：
```bash
make -j$(nproc)
```

### 2. 基础运行
最简单的运行方式，加载二进制镜像并开始执行：
```bash
./a.out --image path/to/your/image.bin
```

---

## 🛠️ 参数说明

| 参数 | 说明 | 示例 |
| :--- | :--- | :--- |
| `--image <file>` | **必选**。指定要加载的二进制镜像文件。 | `--image linux.bin` |
| `--mode <mode>` | 运行模式选择。可选：`normal`, `bbv`, `ckpt`, `restore`。 | `--mode normal` |
| `--diff` | 开启 Difftest 模式。与 Spike 逐条指令对比架构状态（**仅在 normal 模式下可用**）。 | `--diff` |
| `--out-bbv <file>` | 在 `bbv` 模式下，指定产生的 BBV 文件路径。 | `--out-bbv test.bbv` |
| `--points <file>` | 在 `ckpt` 模式下，指定输入的 SimPoint 结果文件。 | `--points test.points` |
| `--ckpt-dir <dir>` | 指定保存 Checkpoint 文件的目录。 | `--ckpt-dir ./checkpoint` |
| `--restore-file <file>`| 指定要恢复的 Checkpoint 文件路径。 | `--restore-file ckpt.gz` |
| `--max-insts <num>` | 运行的最大指令条数（主要用于 `restore` 模式采样）。 | `--max-insts 100000000` |

---

## 💡 使用示例

### 示例 1：开启严格模式 Difftest 运行 Linux
此模式下，模拟器会与 Spike 进行严格的 GPR 和 CSR 校验，任何逻辑偏差都会立即停止。
```bash
./a.out --image ../image/linux.bin --diff
```

### 示例 2：从 Checkpoint 恢复并运行采样
恢复之前保存的状态，并限制运行 1 亿条指令后自动退出并统计。
```bash
./a.out --image linux.bin --mode restore --restore-file checkpoint/ckpt_sp0.gz --max-insts 100000000
```

---

## 🏗️ 核心函数接口说明

### 1. `Ref_cpu` (主模拟器类)
- **`init(reset_pc, image, size)`**: 初始化处理器状态，分配内存并加载 binary 镜像到指定的 DRAM 基地址。
- **`exec(config)`**: 仿真主循环。根据 `SimConfig` 决定运行模式、处理周期计数、触发 Difftest 同步及处理检测点。
- **`RISCV()`**: 取指、译码和分发的核心。处理取指异常，并根据 Opcode 调用不同的子执行函数。
- **`RV32IM() / RV32A() / RV32CSR()`**: 分类处理整数、原子操作及 CSR 相关指令的执行逻辑。
- **`save_checkpoint() / restore_checkpoint()`**: 使用 `zlib` 对 `CPU_state` 以及整个物理内存进行压缩序列化/反序列化。

### 2. `SpikeRef` (金标准比对类)
- **`step(n)`**: 驱动 Spike 内部核心向前执行 `n` 条指令。
- **`reg_check(state, priv)`**: 比对 DUT 传入的状态（PC, GPR, CSR）与 Spike 内部状态。
- **`sync_state(state, priv)`**: 强制将 DUT 的所有架构上下文同步给 Spike（用于启动和中断对齐）。
- **`sync_reg_from_dut(idx, val)`**: 手术级同步。仅同步特定的通用寄存器（通常用于 I/O 读取后）。

---

## 📂 项目结构
- `include/`: 所有的头文件，定义了指令集掩码、CSR 地址及类接口。
- `exec.cpp`: 包含主要的指令执行逻辑以及硬件外设（如 UART/PLIC）的简易模拟。
- `simpoint.cpp`: 负责 BBV 生成及 Checkpoint 的保存与恢复。
- `spike_ref.h`: 处理与 Spike (`libriscv`) 的集成及状态对比。
