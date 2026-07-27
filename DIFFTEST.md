# Difftest 技术文档

本文档说明 `GenSimpoint` 当前支持的两类 Difftest 用法：

- standalone 模式下基于 Spike 的 `--diff`
- 作为上层主模拟器 backend 的 `librefcpu.a`

两条路径共享同一个 `Ref_cpu` 执行核心，但集成方式和目标不同。

---

## 1. 总览

### 1.1 standalone Spike Difftest
- 入口：`main.cpp`
- 运行方式：`./a.out --image your_image.bin --diff`
- 依赖：需要 `SPIKE=1` 构建出的 `a.out`
- 作用：让 `GenSimpoint` 自己作为 DUT，与 Spike 逐条比对

### 1.2 静态库 RefCPU Difftest
- 入口：`include/api/refcpu_api.h`
- 产物：`librefcpu.a`
- 依赖：不要求 Spike
- 作用：让上层主模拟器把 `GenSimpoint` 当作参考模型链接进去

---

## 2. standalone Spike Difftest

### 2.1 基本流程
standalone `--diff` 运行在 `Ref_cpu::exec()` 的主循环里，遵循：

1. `GenSimpoint` 先执行一条指令
2. 普通 RAM 指令由 Spike 执行一条，并比较 PC、GPR、CSR
3. MMIO/外设访问、`wfi` 等待和 DUT 中断切换不让 Spike 自行执行，直接同步 DUT post-state
4. OCSDC DMA 写入 SDRAM/DDR 后，把对应物理地址范围同步到 Spike
5. 发现不一致时立即停止并报错

Spike 普通 step 前会清除自身的异步 `MTIP/MEIP/SEIP` pending，CSR 比较时也屏蔽这些由平台时序决定的位；中断是否发生以及 trap 后状态完全以 DUT 为准。

### 2.2 延迟启动
为绕过早期 boot stub 差异，Spike Difftest 不会从 `pc=0x0` 立即开始，而是在 DUT PC 到达 `0x80000000` 后先同步完整 SDRAM、DDR 和架构状态，再开始逐条比对。

### 2.3 构建与运行
```bash
# 带 Spike 的 standalone 可执行文件
make with_spike

# 启用 standalone Spike difftest
./a.out --image your_image.bin --diff
```

如果当前是 `SPIKE=0` 构建，传 `--diff` 会打印 warning 并自动禁用。

---

## 3. 静态库 RefCPU 接口

### 3.1 设计目标
`librefcpu.a` 的目标不是再套一层 Spike，而是提供一个稳定、可嵌入、可控的参考 CPU 接口，供上层主模拟器直接调用。

这条路径的关键点：
- 不依赖 Spike
- 不走 standalone `main.cpp`
- 默认不打印 UART
- 上层可以直接读写 RAM、IO、架构状态

### 3.2 头文件与产物
- API 头文件：`include/api/refcpu_api.h`
- 静态库：`librefcpu.a`

构建方式：
```bash
make -C GenSimpoint librefcpu.a
```

### 3.3 常用接口
- `refcpu_init(reset_pc, ram_size_bytes)`: 创建上下文并分配 RAM
- `refcpu_destroy(ctx)`: 销毁上下文
- `refcpu_step(ctx, steps)`: 执行若干条指令
- `refcpu_get_state / refcpu_set_state`: 读写架构状态
- `refcpu_sync_ram_from_dut`: 从上层同步 RAM 内容
- `refcpu_load_word / refcpu_store_word`: 直接访问参考模型内存/IO
- `refcpu_set_uart_print(ctx, enable)`: 控制 UART 打印
- `refcpu_set_ref_only(ctx, enable)`: 控制某些参考模型专用行为

---

## 4. 共享执行语义

无论是 standalone 还是静态库，底层都使用同一个 `Ref_cpu`，因此以下行为一致：

- SDRAM 窗口为 `0x40000000 .. 0x4fffffff`（256 MiB）
- DDR 窗口为 `0x80000000 .. 0xffffffff`（2 GiB）
- Boot/UART/XPS INTC/Timer/OCSDC 使用离散 `io_words` 存储
- 物理地址访问采用严格白名单检查
- 遇到 `ebreak` 会退出
- `wfi` 按可退休 NOP 处理

其中物理地址白名单当前为：
- Boot IO: `0x00000000 .. 0x000fffff`
- UART: `0x10000000 .. 0x100000ff`
- XPS INTC: `0x1fb00000 .. 0x1fb00fff`
- Timer: `0x1fd0e000 .. 0x1fd0e0ff`
- OCSDC: `0x1fe10000 .. 0x1fe100ff`
- SDRAM: `0x40000000 .. 0x4fffffff`
- DDR: `0x80000000 .. 0xffffffff`

任何不在白名单内的访存都会直接报错，例如：
```text
[RefCPU] illegal store: addr=0xc03fdea0, size=4 (not in RAM or implemented MMIO)
```

---

## 5. standalone 与静态库的差异

### 5.1 UART 输出
- standalone：默认打开 UART 打印
- 静态库：默认关闭 UART 打印

原因是 standalone 需要可见的控制台输出，而上层主模拟器 difftest 通常要求参考模型保持静默。

### 5.2 入口差异
- standalone 通过 `main.cpp` 做命令行解析，再调用 `Ref_cpu::exec()`
- 静态库由上层自己驱动 `refcpu_*` API，不经过 `main.cpp`

### 5.3 停止条件
- standalone 常见停止条件：`ebreak`、`wfi`、非法访存、超时
- 静态库常见停止条件：上层停止调用 `refcpu_step()`，或参考模型内部触发 `sim_end`

---

## 6. 依赖与构建注意事项

`GenSimpoint/Makefile` 现在为 `.o` 自动生成并包含 `.d` 依赖文件，因此：

- 修改 `include/*.h` 或 `include/api/*.h` 后会自动触发相关目标重编
- `make librefcpu.a` 不再依赖手动删 `.o`

常用命令：
```bash
# 构建 standalone
make -C GenSimpoint

# 构建静态库
make -C GenSimpoint librefcpu.a

# 清理
make -C GenSimpoint clean
```

---

## 7. 推荐使用方式

### 7.1 调试 `GenSimpoint` 自身
使用 standalone：
```bash
./a.out --image your_image.bin
```

如果要对 Spike：
```bash
./a.out --image your_image.bin --diff
```

### 7.2 作为主模拟器参考模型
优先使用静态库：
```bash
make -C GenSimpoint librefcpu.a
make -j4
```

这样主模拟器只维护一套参考模型代码路径，不再维护单独的 in-tree refcpu 副本。
