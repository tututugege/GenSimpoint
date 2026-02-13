# Difftest 技术文档 (Strict Mode)

本文档详细介绍了模拟器中 **Difftest (差异化测试)** 系统的架构设计、实现细节及使用方法。当前系统已升级至 **Strict Mode (严格模式)**，以确保仿真行为与金标准 (Spike) 的绝对一致。

---

## 1. 基本原理与机制

### 1.1 Step-and-Check 机制
Difftest 运行在主仿真循环中，遵循 **"先执行，后比对"** 的原则：
1. **DUT 执行**：模拟器 (DUT) 执行一条指令，更新内部寄存器和内存。
2. **Ref 同步执行**：金标准模拟器 (Spike) 通过 `step(1)` 执行相同数量的指令。
3. **状态比对**：提取两者的架构状态（Architectural State），包括 PC、通用寄存器 (GPR) 和关键控制状态寄存器 (CSR)。
4. **异常处理**：若比对失败，立即停止仿真并输出详尽的差异报告。

### 1.2 延迟启动策略
为了避开自定义 Bootloader 段（通常涉及大量机器相关的非标准 I/O 初始化），Difftest 仅在 PC 到达 Payload 入口点（`0x80000000`）时才启动。启动时，DUT 会将其当前的寄存器状态全量同步给 Spike。

---

## 2. 内存空间与隔离架构

为了保证 DUT 的稳定性和 Spike 的纯净性，系统采用了 **物理内存隔离** 方案：

*   **隔离设计**：Spike 拥有自己独立的 DRAM 缓冲区（`SpikeMem`），与 DUT 的内存互不干涉。
*   **防止污染**：Spike 在执行过程中由于缺乏外设逻辑，可能会对 I/O 区域或某些特定地址进行写回。通过隔离，Spike 的任何写操作都不会破坏 DUT 的硬件外设状态或内存。
*   **同步加载**：在初始化阶段，相同的 `bin` 镜像会被分别加载到 DUT 和 Spike 的独立内存空间中。

| 内存区域 | 基地址 | 长度 | 说明 |
| :--- | :--- | :--- | :--- |
| **DRAM (Isolated)** | `0x80000000` | 1GB+ | 每个模拟器各有一份拷贝 |
| **Boot ROM** | `0x10000` | 4KB | Spike 内部构造的 Boot 段 |
| **PLIC/UART/Timer** | *多个* | *多个* | 仅 DUT 拥有真实外设状态 |

---

## 3. 同步机制：手术级 (Surgical) vs. 全量 (Full)

在“严格模式”下，我们最大限度地减少了状态同步，以暴露 DUT 可能存在的逻辑缺陷：

### 3.1 寄存器手术同步 (Surgical Sync)
针对 **I/O 读取指令**（如串口读取、计时器值读取）：
- Spike 并没有真实的外设。当 DUT 读到真实 I/O 数据时，Difftest 会调用 `sync_reg_from_dut`。
- **操作**：仅将该指令的目标通用寄存器（RD）值从 DUT 拷贝给 Spike。
- **意义**：允许 Spike 获得正确的外部输入数据，继续后续计算，而无需同步其他状态。

### 3.2 中断与特权级同步 (Full State Sync)
针对 **中断触发** 或 **CSR 写入**：
- 当发生外部中断或 DUT 主动修改 `mip/sip` 等关键寄存器时，系统调用 `sync_state`。
- **操作**：将 DUT 的全部架构寄存器（GPR + 21 个 CSR）全量覆盖到 Spike。
- **意义**：对齐异常处理的起点，确保两者在处理异步事件时进入相同的特权级。

---

## 4. 严格校验：GPR + 21 个 CSR

为了捕捉最隐蔽的 Bug，校验逻辑不仅覆盖了通用寄存器，还包含了 DUT 实现的所有 CSR。

### 校验范围
1. **PC**: 指令流的绝对对齐。
2. **GPR (x0-x31)**: 计算结果的校验。
3. **CSR (21个)**: 
   - **Machine 层**: `mtvec`, `mepc`, `mcause`, `mie`, `mip`, `mtval`, `mscratch`, `mstatus`, `mideleg`, `medeleg`, `mhartid`, `misa`
   - **Supervisor 层**: `sepc`, `stvec`, `scause`, `sscratch`, `stval`, `sstatus`, `sie`, `sip`, `satp`

### 报错示例 (Bug 追踪)
系统增加了高亮显示，当检测到差异时会输出：
```text
[Difftest Mismatch Detected!]
CSR Name  Address   Reference (Spike)   DUT (Simulator)
sstatus   0x100     0x102               0x40102          <-- MISMATCH
```
*这曾成功帮助定位了 Spike 内部 `mstatus.SUM` 位无法写入的问题。*

---

## 5. SpikeRef 类成员说明 (`include/spike_ref.h`)

### 5.1 主要成员变量
- `std::unique_ptr<sim_t> sim`: Spike 核心仿真器对象。
- `std::unique_ptr<cfg_t> cfg`: Spike 配置对象（ISA、内存分布等）。
- `SpikeMem* main_mem_ptr`: 指向 Spike 独立主存的指针。

### 5.2 核心成员函数
- **`SpikeRef(...)`**: 构造函数。负责初始化 Spike，配置 ISA (如 `RV32IMAB`)，并加载 Binary 镜像。
- **`step(n)`**: 让 Spike 执行 `n` 条指令。
- **`reg_check(state, priv)`**: 核心校验函数。对比 DUT 传入的状态与 Spike 当前状态。返回 `false` 时直接退出仿真。
- **`sync_state(state, priv)`**: 将 DUT 的完整硬件上下文（GPR + 21 CSRs）强行同步到 Spike。
- **`sync_reg_from_dut(idx, val)`**: 将 DUT 的第 `idx` 个通用寄存器值设置为 `val`。

---

## 6. 使用与调试

1. **编译**: 确保 `libriscv` 已正确安装，直接 `make`。
2. **启动**: 使用 `--diff` 参数开启。
   ```bash
   ./a.out --image your_image.bin --diff
   ```
3. **拦截**: 仿真一旦报错（Exit code 1），请根据输出的红色 `MISMATCH` 标签定位出错位置。
   - **通用寄存器错**：通常是上条指令计算逻辑 (ALU/Memory) 错误。
   - **PC 错**：通常是分支预测、跳转指令或异常跳转逻辑错误。
   - **CSR 错**：通常是特权指令、掩码 (Mask) 处理或中断屏蔽逻辑错误。

---
*文档版本: v2.0 (Strict Sync Support)*
