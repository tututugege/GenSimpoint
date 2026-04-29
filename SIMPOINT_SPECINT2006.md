# SimPoint 原理与本模拟器生成 SPECint2006 SimPoint 指南

本文介绍：
1. SimPoint 的核心思想
2. 如何用简单模拟器（https://github.com/tututugege/GenSimpoint）为 SPEC CPU2006 INT 基准生成 BBV、SimPoint、Checkpoint

## 1. SimPoint 的原理

SimPoint 的目标是：
- 不跑完整程序的全部阶段，
- 通过少量“代表性区间”近似整体行为，
- 用于微结构仿真/性能评估时显著提速。

标准流程如下：
1. 把程序执行轨迹按固定长度切片（interval）。
2. 每个切片统计 Basic Block Vector（BBV）：即“哪些基本块执行了多少次”。
3. 对所有切片 BBV 做聚类（SimPoint 工具常用 k-means 变体）。
4. 每个簇选一个代表切片（simpoint），并输出权重（weight）。
5. 后续仿真只跑这些代表切片（配合 warmup），再按权重加权汇总结果。

## 2. 模拟器里的关键实现细节

### 2.1 Interval 定义

本模拟器当前 interval 大小为：
- `INTERVAL_SIZE = 100000000`（1e8）
- 定义位置：`include/ref.h`
- 可以自行设置不同的 INTERVAL_SIZE，建议10M-100M

### 2.2 BBV 统计触发点

在 `exec.cpp` 中：
- 仅在 `privilege == U` 且遇到分支边界时累计 `current_bb_len`。
- 达到 interval 边界后在 `--mode bbv` 下 `dump_bbv()` 输出一行 `T ...`。

这意味着 BBV 是“按当前实现定义的基本块/计数口径”生成的，只考虑用户态指令，后续 SimPoint 和 checkpoint 必须使用同一口径，不能混用其他工具生成的 points。

### 2.3 Checkpoint 生成策略

`--mode ckpt` 读取 `.points`（格式：`<interval_id> <simpoint_id>`）后：
- 采用 `target = upcoming_interval + 1` 的 lookahead 逻辑。
- 对 target interval 生成 `warmup1` checkpoint（预热长度 1 个 interval，也就是提一个interval的checkpoint）。
- 文件名类似：
  - `ckpt_sp<sp_id>_target<interval_id>_warmup1.gz`
- 若 target 为 0，则生成：
  - `ckpt_sp<sp_id>_target0_nowarmup.gz`

## 3. 环境准备（SPECint2006）

你需要先准备好 SPECint2006 的 RISC-V 可执行镜像（`.bin`），并确保：
- 每个 benchmark 最终都有一个可被本模拟器直接加载的镜像文件。
- 文件放在 `mem/` 目录（脚本默认输入目录）。

建议命名示例：
- `mem/401.bzip2.bin`
- `mem/429.mcf.bin`
- `mem/456.hmmer.bin`
- ...

## 4. 一次性批量流程（推荐）

### 4.1 编译模拟器

```bash
make -j$(nproc)
```

生成可执行文件：`./a.out`

### 4.2 批量生成 BBV

```bash
bash run_bbv.sh
```

默认行为：
- 输入目录：`mem/`
- 输出 BBV：`bbv_log/<bench>.bbv`
- 日志：`bbv_log/<bench>.log`
- 并行度：`MAX_JOBS=12`（在 `run_bbv.sh` 中可改）

### 4.3 批量跑 SimPoint 并生成 checkpoint

```bash
bash run_ckpt.sh
```

默认行为：
- SimPoint 工具：`./SimPoint/simpoint`
- 读取：`bbv_log/<bench>.bbv`
- 产出 points/weights：
  - `bbv_log/<bench>.points`
  - `bbv_log/<bench>.weights`
- 产出 checkpoint：`ckpts/<bench>/`
- 日志目录：`ckpt_log/`

`run_ckpt.sh` 当前 SimPoint 参数：
- `-maxK 30`
- `-seedproj 0721`
- `-seedkm 11037`
- `-numInitSeeds 5`
- `-iters 1000`

## 5. 单个 benchmark 手工流程（便于调试）

以 `429.mcf.bin` 为例：

### 5.1 生成 BBV

```bash
./a.out --image mem/429.mcf.bin --mode bbv --out-bbv bbv_log/429.mcf.bbv
```

### 5.2 运行 SimPoint 聚类

```bash
./SimPoint/simpoint \
  -loadFVFile bbv_log/429.mcf.bbv \
  -saveSimpoints bbv_log/429.mcf.points \
  -saveSimpointWeights bbv_log/429.mcf.weights \
  -maxK 30 \
  -seedproj 0721 \
  -seedkm 11037 \
  -numInitSeeds 5 \
  -iters 1000
```

### 5.3 生成 checkpoint

```bash
./a.out \
  --image mem/429.mcf.bin \
  --mode ckpt \
  --points bbv_log/429.mcf.points \
  --ckpt-dir ckpts/429.mcf
```

### 5.4 从某个 checkpoint 恢复运行

```bash
./a.out \
  --image mem/429.mcf.bin \
  --mode restore \
  --restore-file ckpts/429.mcf/ckpt_sp0_targetXXXX_warmup1.gz \
  --max-insts 100000000
```

## 6. 结果检查建议

每个 benchmark 至少检查以下文件：
- `bbv_log/<bench>.bbv`：存在且非空
- `bbv_log/<bench>.points`：有多行 `interval simpoint_id`
- `bbv_log/<bench>.weights`：权重和通常接近 1
- `ckpts/<bench>/`：存在一个或多个 `ckpt_sp*.gz`
- `ckpt_log/<bench>.log`：无报错，包含 SimPoint 完成和 checkpoint 完成信息

## 7. 常见问题

1. `SimPoint tool not found`
- 检查 `run_ckpt.sh` 的 `SIMPOINT_TOOL` 路径是否为 `./SimPoint/simpoint`。

2. 只有 BBV，没有 checkpoint
- 常见原因是 `.points` 未生成或为空。
- 先看 `ckpt_log/<bench>.log` 里的 SimPoint 阶段输出。

3. checkpoint 数量与预期不一致
- 先确认 `.points` 实际选择了几个 simpoint。
- 再确认模拟是否提前结束（程序本身很短时，后续 target 可能到不了）。

4. 想改 interval 大小
- 当前需要修改源码 `include/ref.h` 的 `INTERVAL_SIZE` 并重新编译。
- 修改后要重新生成 BBV、points、checkpoint，不能复用旧文件。

## 8. 针对 SPECint2006 的实践建议

1. 先小规模试跑
- 先选 2~3 个 benchmark 走通全流程，确认路径与格式无误，再全量并行，对于ref规模，429.mcf最少，约2900亿条指令，其余大部分都在1w-2w亿条指令量级，最多的是h264ref，五万亿。可以考虑使用test规模检查编译优化效果，最多的约900亿。目前简单模拟器速度约5kw指令/s。

2. 控制并行度
- `run_bbv.sh` 和 `run_ckpt.sh` 的 `MAX_JOBS` 建议根据机器内存调节，避免并发过高导致系统抖动。

3. 固定参数保证可复现
- `seedproj/seedkm` 固定有助于多次实验得到稳定 simpoint。

4. 保留 points/weights 与 checkpoint 一一对应
- 后续做加权统计时必须使用同一版本 points/weights，避免结果错配。

