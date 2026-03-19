#!/usr/bin/env bash
#
# @file run_small_cases.sh
# @brief 小用例测试脚本 - 测试 ss_benchmarks 目录下的所有用例
# @usage: ./run_small_cases.sh [case_list_file] [max_jobs]
#
set -euo pipefail

# 默认参数
LIST_FILE="${1:-./scripts/small_case.list}"
MAX_JOBS="${2:-}"

# 检查用例列表文件
if [[ ! -f "$LIST_FILE" ]]; then
    echo "错误: 用例列表文件不存在: $LIST_FILE" >&2
    echo "用法: $0 [case_list_file] [max_jobs]" >&2
    exit 1
fi

# 获取脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# 设置可执行文件路径
BIN_PATH="${PROJECT_DIR}/build/cons_partitioner"

# 检查可执行文件
if [[ ! -x "$BIN_PATH" ]]; then
    echo "错误: 可执行文件不存在或无权限: $BIN_PATH" >&2
    echo "请先构建项目: mkdir -p build && cd build && cmake .. && make -j" >&2
    exit 1
fi

# 设置并行任务数
if [[ -z "$MAX_JOBS" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        MAX_JOBS="$(nproc)"
    else
        MAX_JOBS="4"
    fi
fi

# 设置输出目录
LOG_DIR="${PROJECT_DIR}/logfiles"
RESULTS_DIR="${PROJECT_DIR}/scripts/results"
ROWS_DIR="${RESULTS_DIR}/rows"
RESULTS_FILE="${RESULTS_DIR}/summary.csv"

# 创建输出目录
mkdir -p "$LOG_DIR" "$ROWS_DIR"

# 写入 CSV 表头
echo "case_name,exit_code,cut_size,total_time_s,log_file" > "$RESULTS_FILE"

echo "========================================"
echo "ConsMLP 小用例测试"
echo "========================================"
echo "可执行文件: $BIN_PATH"
echo "用例列表:   $LIST_FILE"
echo "日志目录:   $LOG_DIR"
echo "并行任务数: $MAX_JOBS"
echo "========================================"

# 运行单个用例的函数
# @param case_path 用例路径（相对于项目根目录）
# @param imbalance_val 不平衡度参数（可选，默认 0.03）
run_case() {
    local case_path="$1"
    local imbalance_val="${2:-0.03}"
    local case_name
    case_name="$(basename "$case_path")"
    local hgr="${PROJECT_DIR}/${case_path}.hgr"
    local insts="${PROJECT_DIR}/${case_path}.insts"
    local log_file="${LOG_DIR}/${case_name}.log"

    echo "[开始] 测试用例: $case_name"

    # 检查 .hgr 文件
    if [[ ! -f "$hgr" ]]; then
        echo "错误: 缺少 .hgr 文件: $hgr" > "$log_file"
        echo "${case_name},1,,,${log_file}" > "${ROWS_DIR}/${case_name}.csv"
        echo "[失败] $case_name: 缺少 .hgr 文件"
        return 0
    fi

    # 检查 .insts 文件
    if [[ ! -f "$insts" ]]; then
        echo "错误: 缺少 .insts 文件: $insts" > "$log_file"
        echo "${case_name},1,,,${log_file}" > "${ROWS_DIR}/${case_name}.csv"
        echo "[失败] $case_name: 缺少 .insts 文件"
        return 0
    fi

    # 运行分区器
    # 使用 direct 模式，k=4，带类型约束
    # 注意：hgr文件作为第一个位置参数，不使用 -f
    set +e
    "$BIN_PATH" "$hgr" \
        -k 4 \
        -imbalance "$imbalance_val" \
        -mode recursive \
        -init all \
        -coarsen cluster \
        -refine gfm \
        -types "$insts" \
        > "$log_file" 2>&1
    local exit_code=$?
    set -e

    # 解析输出结果
    local cut_size
    cut_size="$(awk -F':' '/Cut size:/ {gsub(/ /,"",$2); cut=$2} END {print cut}' "$log_file")"
    local total_time
    total_time="$(awk -F':' '/Total time:/ {gsub(/s/,"",$2); gsub(/ /,"",$2); t=$2} END {print t}' "$log_file")"

    # 写入结果
    echo "${case_name},${exit_code},${cut_size:-},${total_time:-},${log_file}" > "${ROWS_DIR}/${case_name}.csv"

    if [[ "$exit_code" -eq 0 ]]; then
        echo "[成功] $case_name - Cut size: ${cut_size:-N/A}, Time: ${total_time:-N/A}s"
    else
        echo "[失败] $case_name - 退出码: $exit_code"
    fi
}

# 主循环：读取用例列表并并行执行
job_count=0
while read -r case_path imbalance_val || [[ -n "$case_path" ]]; do
    # 跳过空行和注释行
    [[ -z "$case_path" ]] && continue
    [[ "${case_path:0:1}" == "#" ]] && continue

    # 默认不平衡度
    if [[ -z "$imbalance_val" ]]; then
        imbalance_val="0.03"
    fi

    # 后台运行用例
    run_case "$case_path" "$imbalance_val" &
    job_count=$((job_count + 1))

    # 控制并行任务数
    if [[ "$job_count" -ge "$MAX_JOBS" ]]; then
        wait -n 2>/dev/null || true
        job_count=$((job_count - 1))
    fi
done < "$LIST_FILE"

# 等待所有后台任务完成
wait

# 合并所有结果到汇总 CSV
cat "${ROWS_DIR}"/*.csv >> "$RESULTS_FILE"

echo "========================================"
echo "测试完成"
echo "结果汇总: $RESULTS_FILE"
echo "日志文件: $LOG_DIR"
echo "========================================"

# 显示简要统计
total_cases=$(wc -l < "${ROWS_DIR}"/*.csv 2>/dev/null | tail -1 | awk '{print $1}')
success_cases=$(grep -c ',0,' "$RESULTS_FILE" 2>/dev/null || echo "0")
fail_cases=$((total_cases - success_cases))

echo "统计信息:"
echo "  总用例数: $total_cases"
echo "  成功:     $success_cases"
echo "  失败:     $fail_cases"
