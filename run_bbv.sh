#!/bin/bash

# === 配置区域 ===
CMD="./a.out"
MODE="--mode bbv"
INPUT_DIR="mem"       # 输入文件目录
OUT_DIR="bbv_log"     # 结果输出目录
LOG_DIR="bbv_log"     # 日志目录
MAX_JOBS=12           # <--- 12 并行
# ===============

# 1. 预检查：确保 a.out 存在且有执行权限
if [ ! -x "$CMD" ]; then
    echo "❌ 错误: 找不到 $CMD 或者没有执行权限！"
    echo "   请尝试运行: chmod +x $CMD"
    exit 1
fi

# 检查并创建目录
mkdir -p "$OUT_DIR"
mkdir -p "$LOG_DIR"

echo "=== 开始批量处理 (并行数: $MAX_JOBS) ==="
echo "工作目录: $(pwd)"
echo "输入目录: $INPUT_DIR"
echo "--------------------------------"

# 使用 find 查找 .bin 文件
while read -r IMAGE_PATH; do
    # 跳过空行
    [ -z "$IMAGE_PATH" ] && continue

    FILENAME=$(basename "$IMAGE_PATH")
    # 去除后缀，生成 .bbv 文件名
    BASENAME="${FILENAME%.*}"
    OUT_FILE="$OUT_DIR/$BASENAME.bbv"
    LOG_FILE="$LOG_DIR/$BASENAME.log"

    # 拼接完整命令（用于日志和执行）
    FULL_CMD="$CMD $MODE --image $IMAGE_PATH --out-bbv $OUT_FILE"

    # 在终端打印，方便看到进度
    echo "🚀 [启动] $FILENAME -> $OUT_FILE"

    # 启动后台任务
    (
        # 重定向标准输出和错误到日志文件
        exec > "$LOG_FILE" 2>&1

        echo "=== Debug Info ==="
        echo "Time: $(date)"
        echo "Exec: $FULL_CMD"
        echo "=================="

        # 执行命令
        $FULL_CMD
        EXIT_CODE=$?

        echo ""
        echo "=== Finished ==="
        echo "Exit Code: $EXIT_CODE"
    ) &

    # --- 并行控制逻辑 ---
    while true; do
        # 统计当前后台运行的作业数
        CURRENT_JOBS=$(jobs -r | wc -l)

        # 如果当前任务数小于 MAX_JOBS，跳出循环继续投递新任务
        if [ "$CURRENT_JOBS" -lt "$MAX_JOBS" ]; then
            break
        fi
        # 否则等待一秒再检查
        sleep 1
    done

done < <(find "$INPUT_DIR" -name "*.bin" -type f)

echo "--------------------------------"
echo "所有任务已分发完毕，正在等待剩余任务结束..."
wait
echo "✅ 全部完成。"
