#!/bin/bash

# ================= 配置区域 =================

# 1. 模拟器路径
SIMULATOR="./a.out"

# 2. SimPoint 工具路径 (根据你的环境调整)
# 注意：之前 `ls SimPoint` 显示 simpoint 在 SimPoint/simpoint，而不是 bin/
SIMPOINT_TOOL="./SimPoint/simpoint"

# 3. 输入目录 (存放 benchmark 的二进制文件)
INPUT_DIR="mem"

# 4. BBV 和 Log 目录 (复用之前的输出)
BBV_DIR="bbv_log"
LOG_DIR="ckpt_log"  # Checkpoint 生成日志分开存放

# 5. Checkpoint 输出路径
# 建议改为本地路径，避免 NFS 权限问题
CKPT_BASE_DIR="ckpts"

# 6. SimPoint 参数
INTERVAL_SIZE=100000000
MAX_K=30

# 7. 并行进程数
MAX_JOBS=4

# ===========================================

# 检查必要工具
if [ ! -f "$SIMULATOR" ]; then
    echo "❌ Error: Simulator not found at $SIMULATOR"
    exit 1
fi

if [ ! -f "$SIMPOINT_TOOL" ]; then
    echo "❌ Error: SimPoint tool not found at $SIMPOINT_TOOL"
    echo "   (Checked path: $(realpath -m $SIMPOINT_TOOL))"
    exit 1
fi

# 创建目录
mkdir -p "$CKPT_BASE_DIR"
mkdir -p "$BBV_DIR"
mkdir -p "$LOG_DIR"

echo "=== Checkpoint Generation Config ==="
echo "Simulator : $SIMULATOR"
echo "SimPoint  : $SIMPOINT_TOOL"
echo "Input Dir : $INPUT_DIR"
echo "BBV Dir   : $BBV_DIR"
echo "Ckpt Dir  : $CKPT_BASE_DIR"
echo "Parallel  : $MAX_JOBS jobs"
echo "===================================="

# 定义单个 Benchmark 的处理函数
process_benchmark() {
    local bench_path=$1
    local bench_name=$(basename "$bench_path")
    # 去除后缀 (例如 test_mcf.bin -> test_mcf)
    local basename="${bench_name%.*}"

    # 注意：run_bbv.sh 生成的是 .bbv 后缀
    local bbv_file="$BBV_DIR/${basename}.bbv"
    
    local points_file="$BBV_DIR/${basename}.points"
    local weights_file="$BBV_DIR/${basename}.weights"

    local ckpt_dir="$CKPT_BASE_DIR/${basename}"
    local log_file="$LOG_DIR/${basename}.log"

    # 初始化日志
    echo "=== Start processing $basename at $(date) ===" > "$log_file"
    echo "[Info] Checkpoint Output Dir: $ckpt_dir" >> "$log_file"

    echo "🚀 [Start] $basename (Log: $log_file)"

    # ---------------------------------------------------------
    # 第一步：检查 BBV 是否存在
    # ---------------------------------------------------------
    if [ ! -f "$bbv_file" ]; then
        echo "⚠️  [Skip] BBV file not found: $bbv_file"
        echo "[Error] BBV file missing: $bbv_file" >> "$log_file"
        return 0 # 跳过而不是报错退出
    fi

    # ---------------------------------------------------------
    # 第二步：运行 SimPoint 分析
    # ---------------------------------------------------------
    if [ ! -f "$points_file" ] || [ ! -f "$weights_file" ]; then
        echo "   running SimPoint analysis..."
        echo "" >> "$log_file"
        echo ">>> Stage: SimPoint Analysis" >> "$log_file"

        $SIMPOINT_TOOL \
            -loadFVFile "$bbv_file" \
            -saveSimpoints "$points_file" \
            -saveSimpointWeights "$weights_file" \
            -maxK $MAX_K \
            -seedproj 0721 \
            -seedkm 11037 \
            -numInitSeeds 5 \
            -iters 1000 \
            >> "$log_file" 2>&1

        if [ $? -ne 0 ]; then
            echo "❌ [Error] SimPoint analysis failed for $basename"
            echo "[Failure] SimPoint exited with error" >> "$log_file"
            return 1
        fi
        echo "   SimPoint Analysis finished."
    else
        echo "   Points/Weights already exist."
        echo ">>> Stage: SimPoint Analysis (Skipped)" >> "$log_file"
    fi

    # ---------------------------------------------------------
    # 第三步：生成 Checkpoints
    # ---------------------------------------------------------
    mkdir -p "$ckpt_dir"

    # 如果目录为空则生成
    if [ -z "$(ls -A "$ckpt_dir")" ]; then
        echo "   Generating Checkpoints..."
        echo "" >> "$log_file"
        echo ">>> Stage: Generate Checkpoints" >> "$log_file"

        # 模拟器读取 points 文件并生成 checkpoint
        $SIMULATOR \
            --image "$bench_path" \
            --mode ckpt \
            --points "$points_file" \
            --ckpt-dir "$ckpt_dir" \
            >> "$log_file" 2>&1

        if [ $? -ne 0 ]; then
            echo "❌ [Error] Checkpoint generation failed for $basename"
            echo "[Failure] Simulator exited with error" >> "$log_file"
            return 1
        fi
        echo "✅ [Done] Checkpoints generated for $basename"
    else
        echo "   Checkpoints already exist."
        echo ">>> Stage: Generate Checkpoints (Skipped)" >> "$log_file"
    fi

    echo "=== Finished processing $basename at $(date) ===" >> "$log_file"
}

# ================= 主循环 =================

export -f process_benchmark
export SIMULATOR SIMPOINT_TOOL BBV_DIR CKPT_BASE_DIR LOG_DIR INTERVAL_SIZE MAX_K

echo "Starting parallel jobs..."

# 查找所有 .bin 文件
find "$INPUT_DIR" -name "*.bin" -type f | while read bench_path; do

    # 启动后台任务
    ( process_benchmark "$bench_path" ) &

    # 并行控制
    while [ $(jobs -r -p | wc -l) -ge "$MAX_JOBS" ]; do
        wait -n 2>/dev/null || wait
        sleep 0.1
    done

done

wait
echo "==========================================="
echo "All tasks completed."
