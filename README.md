# RISC-V Full-System Simulator

这是一个轻量级、高性能的 RISC-V 32位全系统模拟器，支持 RV32IMAB + Zfinx 指令集，并集成了两类 Difftest 能力：

- standalone 模式下基于 Spike 的 `--diff`
- 供主模拟器链接使用的 `librefcpu.a` 静态库接口

---

## 🚀 快速开始

### 1. 编译
确保您的系统中已安装 `g++` 和 `zlib` 开发库，然后直接运行：
```bash
make -j$(nproc)
```

默认构建是 **不带 Spike**（`SPIKE=0`，可在没有 `libriscv`/`libfesvr` 的环境下编译）。

如需显式切换：
```bash
# 带 Spike（产物 a.out）
make with_spike

# 不带 Spike（产物 a.out.nospike）
make no_spike
```

说明：
- `a.out.nospike` 下如果传 `--diff`，会打印 warning 并自动禁用 difftest。
- `a.out`（with_spike）才会真正执行 Spike difftest。
- `make -C GenSimpoint librefcpu.a` 会生成给上层主模拟器使用的静态库，不依赖 Spike。

### 1.1 SoftFloat 说明（no_spike 模式）
- `SPIKE=0` 时，默认链接仓库内的 `lib/softfloat/softfloat.a`。
- 如果出现 SoftFloat ABI/行为不兼容（例如链接错误或浮点结果异常），可以自行使用 Berkeley SoftFloat 重新编译静态库并替换该文件。
- 只要保持头文件与库版本一致即可（当前 include 路径为 `lib/softfloat/include`）。

### 2. 基础运行
最简单的运行方式，加载二进制镜像并开始执行：
```bash
./a.out --image path/to/your/image.bin
```

当前 standalone 行为：
- 默认打开 UART 打印。
- 遇到 `ebreak` 会打印退出码并停止。
- 遇到可退休的 `wfi` 会直接停止。
- 访存地址只允许落在 RAM 和已实现 MMIO 白名单中，非法地址会立即报错退出。

---

## 🛠️ 参数说明

| 参数 | 说明 | 示例 |
| :--- | :--- | :--- |
| `--image <file>` | **必选**。指定要加载的二进制镜像文件。 | `--image linux.bin` |
| `--mode <mode>` | 运行模式选择。可选：`normal`, `bbv`, `ckpt`, `restore`。 | `--mode normal` |
| `--diff` | 开启 standalone Spike Difftest（**仅在 normal 模式下可用；仅 `SPIKE=1` 构建有效**）。 | `--diff` |
| `--out-bbv <file>` | 在 `bbv` 模式下，指定产生的 BBV 文件路径。 | `--out-bbv test.bbv` |
| `--points <file>` | 在 `ckpt` 模式下，指定输入的 SimPoint 结果文件。 | `--points test.points` |
| `--ckpt-dir <dir>` | 指定保存 Checkpoint 文件的目录。 | `--ckpt-dir ./checkpoint` |
| `--restore-file <file>`| 指定要恢复的 Checkpoint 文件路径。 | `--restore-file ckpt.gz` |
| `--max-insts <num>` | 运行的最大指令条数（当前主要用于 `restore` 模式采样）。 | `--max-insts 100000000` |

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

## 🧭 地址空间布局（当前实现）

当前实现采用“**256 MiB SDRAM + 2 GiB DDR + 离散 IO word 存储**”的方式，并对物理地址做严格白名单检查：

### 1. RAM（两段独立分配）
- SDRAM：`0x40000000 - 0x4fffffff`（256 MiB）
- DDR：`0x80000000 - 0xffffffff`（2 GiB）
- 两段之间的地址空洞不是 RAM，访问会触发非法访存错误

### 2. IO / Boot（离散存储）
- `0x00000000` 附近：启动 stub（启动阶段写入）
- `0x00001000` 附近：Boot ROM stub（启动阶段写入）
- `0x10000000 - 0x100000ff`：UART（`tree.dts` 对应 `reg size = 0x100`）
- `0x10000004`：OpenSBI 兼容寄存器（初始化写 `0x00006000`）
- `0x1fb00000 - 0x1fb00fff`：XPS INTC
- `0x1fd0e000 - 0x1fd0e0ff`：Timer MMIO 窗口
- `0x1fd0e000`：Timer 低位寄存器（读取返回 `sim_time`）
- `0x1fd0e004`：Timer 高位寄存器（当前返回 0）
- `0x1fe10000 - 0x1fe100ff`：OCSDC

说明：
- 低地址和外设地址不再占用连续大内存，而是按 `word_addr -> word_data` 离散存储。
- 读未初始化的离散 IO word 默认返回 `0`。
- 不在 RAM 或上述 MMIO 白名单内的访存会直接报错，例如：
  `"[RefCPU] illegal store: addr=0x..., size=... (not in RAM or implemented MMIO)"`

---

## 💾 Checkpoint 格式（当前实现）

Checkpoint 使用 `zlib gzip`（写模式 `wb1`，文件后缀通常为 `.gz`），按如下顺序写入：

1. `header`（固定 16 字节）
2. v3 的 `sdram_size`（`uint32_t`，当前为 256 MiB）
3. `CPU_state`（POD 原样二进制）
4. `interval_inst_count`（`uint64_t`）
5. `DDR` 原始字节流（固定 `ram_size` 字节，当前为 2 GiB）
6. v3 checkpoint 的 `SDRAM` 原始字节流（256 MiB）
7. `io_ranges + io_data`（重复 `io_range_count` 次）：
   - `io_range`：两个 `uint32_t`（`base`, `size`）
   - `io_data`：紧随其后的 `size` 字节原始数据

### 当前 checkpoint 版本
- v3 同时保存 DDR 与 SDRAM
- v2 checkpoint 仍可恢复，恢复时新增 SDRAM 清零
- IO 顺序固定为：`BOOT -> UART -> XPS INTC -> TIMER -> OCSDC`

### 兼容性
- 当前恢复逻辑兼容 v2 和 v3；v2 文件没有 SDRAM payload。

---

## 🔄 如何读取与恢复

### 1. 使用模拟器直接恢复（推荐）
```bash
./a.out --image linux.bin --mode restore --restore-file path/to/ckpt.gz --max-insts 100000000
```

恢复流程：
1. 先通过 `init()` 分配 2 GiB DDR 和 256 MiB SDRAM
2. 读取并校验 `header`（magic/version/ram_size/io_range_count）
3. `restore_checkpoint()` 覆盖 `CPU_state` 和 `interval_inst_count`
4. 覆盖 DDR；v3 继续覆盖 SDRAM，v2 将 SDRAM 清零
5. 读取并校验 IO 布局（base+size）是否与当前模拟器配置一致
6. 读取每个 range 的 `io_data` 并恢复 IO 内容

### 2. 离线解析 checkpoint（二次开发）
若需要自己读取文件，按“Checkpoint 格式”中的顺序使用 `gzread` 逐段解析即可。关键点：
- 必须先校验 `magic="Rem\0"`，当前支持 v2 和 v3
- 必须使用与当前程序一致的 `CPU_state` 结构体布局（同编译器/ABI 假设）
- DDR 字节数必须与运行配置一致（当前固定 2 GiB）
- 最后按 `io_ranges` 顺序读取并校验，再读取对应 `io_data`

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

### 3. `refcpu_api`（供主模拟器链接的静态库接口）
- 头文件：`include/api/refcpu_api.h`
- 产物：`librefcpu.a`
- 典型构建：
```bash
make -C GenSimpoint librefcpu.a
```
- 这条路径不依赖 Spike；主模拟器只链接静态库，不会触发 standalone `main.cpp`。
- 作为库使用时，可通过 `refcpu_set_uart_print(ctx, false)` 保持静默。

---

## 📂 项目结构
- `include/`: 所有的头文件，定义了指令集掩码、CSR 地址及类接口。
- `exec.cpp`: 包含主要的指令执行逻辑以及硬件外设（如 UART/XPS INTC/OCSDC）的简易模拟。
- `simpoint.cpp`: 负责 BBV 生成及 Checkpoint 的保存与恢复。
- `spike_ref.h`: 处理与 Spike (`libriscv`) 的集成及状态对比。
