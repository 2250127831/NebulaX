#!/usr/bin/env bash
set -euo pipefail
set +m

PORT="${1:-2250}"
MODE="${2:-}"  # default pipeline, "-r" for pingpong

# 核心分配（P-core 5/6/7，互不重叠）
IO_CORE=6
SEND_CORE=7
CLIENT_CORE=5
if [ "$MODE" = "-r" ]; then
    MODE_FLAG="-r"
    MODE_NAME="pingpong"
else
    MODE_FLAG=""
    MODE_NAME="pipeline"
fi
BASE="$(cd "$(dirname "$0")/.." && pwd)"
PERF_DATA="/tmp/nebulaX_perf.data"
PERF_STAT="/tmp/nebulaX_perf_stat.txt"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/home/qiwang/FlameGraph}"

HAS_SUDO=0
if [ "$EUID" -eq 0 ]; then
    HAS_SUDO=1
fi

GREEN='\033[32m'
CYAN='\033[36m'
RED='\033[31m'
BOLD='\033[1m'
RESET='\033[0m'
CHECK='\xE2\x9C\x93'
ARROW='\xE2\x86\x92'

ok()   { echo -e "  ${GREEN}${CHECK}${RESET} $1"; }
info() { echo -e "  ${CYAN}${ARROW}${RESET} $1"; }
title(){ echo -e "\n${BOLD}$1${RESET}\n=============================="; }

trap "pkill -9 nebulaX &>/dev/null; echo ''; echo '已清理'" EXIT

# 工具检验
check_tool() {
    if ! command -v "$1" &>/dev/null; then
        echo -e "  ${RED}\xE2\x9C\x97${RESET} 未找到 $1，请先安装"
        exit 1
    fi
}
check_tool cmake
check_tool make
check_tool g++
check_tool taskset
check_tool bc
check_tool pidof
check_tool pkill
if [ "$HAS_SUDO" -eq 1 ]; then
    check_tool perf
fi

# ════════════════════════════
title "阶段 1/3 - 编译"
# ════════════════════════════

echo "  [1/2] 编译服务端 ..."
cd "$BASE/build"
cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
make -j4 >/dev/null 2>&1
ok "nebulaX 编译完成"

echo "  [2/2] 编译压测客户端 ..."
g++ -std=c++17 -O2 "$BASE/benchmark/benchmark_client.cpp" \
    -o "$BASE/benchmark/benchmark_client" -lpthread
ok "benchmark_client 编译完成"

# ════════════════════════════
title "阶段 2/3 - 压测"
# ════════════════════════════

pkill -9 nebulaX &>/dev/null || true
sleep 0.5
ok "旧服务端已清理"

if [ "$HAS_SUDO" -eq 1 ]; then
    sysctl -w kernel.kptr_restrict=0 >/dev/null 2>&1 || true
    info "kernel.kptr_restrict=0"
fi

info "启动服务端 nebulaX (IO=core $IO_CORE, Send=core $SEND_CORE, port $PORT) ..."
taskset -c "$IO_CORE,$SEND_CORE" "$BASE/build/nebulaX" "$PORT" \
    --io-core "$IO_CORE" --send-core "$SEND_CORE" > /dev/null 2>&1 &
sleep 2

SERVER_PID=$(pidof nebulaX 2>/dev/null || echo "")
if [ -z "$SERVER_PID" ]; then
    echo -e "  ${RED}\xE2\x9C\x97${RESET} 服务端启动失败，请检查编译"
    exit 1
fi
ok "服务端 PID=$SERVER_PID (core 6, port $PORT)"

echo ""
info "运行 benchmark_client ($MODE_NAME, core 5, 500K x 3) ..."
echo ""

if [ "$HAS_SUDO" -eq 1 ]; then
    perf record -T -F 999 --call-graph dwarf -e cpu-clock -p "$SERVER_PID" \
        -o "$PERF_DATA" 2>/dev/null &
    PERF_RECORD_PID=$!
    perf stat -e context-switches,cycles,instructions,cache-misses,branch-misses,\
L1-dcache-load-misses,L2-load-misses,\
syscalls:sys_enter_sendto,syscalls:sys_enter_recvfrom,syscalls:sys_enter_read \
        -p "$SERVER_PID" -o "$PERF_STAT" 2>/dev/null &
    PERF_STAT_PID=$!
fi

CLIENT_TMP=$(mktemp /tmp/nebulaX_client_out.XXXXXX)
taskset -c "$CLIENT_CORE" "$BASE/benchmark/benchmark_client" 127.0.0.1 "$PORT" $MODE_FLAG > "$CLIENT_TMP" 2>&1
echo ""; cat "$CLIENT_TMP"

read -r B_TOTAL B_AVG B_P50 B_P99 B_P999 B_QPS <<< \
    $(grep '| avg |' "$CLIENT_TMP" | awk -F'|' '{gsub(/ /,"",$3);gsub(/ /,"",$4);gsub(/ /,"",$5);gsub(/ /,"",$6);gsub(/ /,"",$7);gsub(/ /,"",$8); print $3,$4,$5,$6,$7,$8}')
rm -f "$CLIENT_TMP"

if [ "$HAS_SUDO" -eq 1 ]; then
    kill -INT "$PERF_RECORD_PID" 2>/dev/null || true
    kill -INT "$PERF_STAT_PID" 2>/dev/null || true
    sleep 1
    ok "perf 已停止"

    # 裁剪空闲期：取 2.5%~97.5% 样本时间窗口
    TIME_RANGE=$(perf script -i "$PERF_DATA" -F time 2>/dev/null | \
        awk 'NF{t[++n]=$1} END{if(n>20){p5=t[int(n*0.025)]; p95=t[int(n*0.975)]; printf "%f,%f", p5, p95} else printf "0,%f", t[n]}' 2>/dev/null)
fi

# ════════════════════════════
title "阶段 3/3 - 分析结果"
# ════════════════════════════

# 基准汇总（无论有无 sudo 都输出）
echo ""
echo -e "${BOLD}  压测汇总${RESET}"
echo "    avg=${B_AVG}us  P50=${B_P50}us  P99=${B_P99}us  P999=${B_P999}us  QPS=${B_QPS}"

if [ "$HAS_SUDO" -eq 1 ]; then
    echo ""
    echo -e "${BOLD}  上下文切换${RESET}"
    grep -v "<not counted>" "$PERF_STAT" 2>/dev/null \
        | grep -v "cpu_atom/" \
        | grep -v "^#" \
        | grep -v "^$" \
        | grep -v "Performance counter" \
        | sed 's/  #.*$//' \
        | sed 's/^/    /'

    CTX=$(awk '/context-switches/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null)
    SEC=$(awk '/seconds time elapsed/{print $1; exit}' "$PERF_STAT" 2>/dev/null)
    CYCLES=$(awk '/cpu_core\/cycles\//{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null)
    INST=$(awk '/cpu_core\/instructions\//{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null)
    CMISS=$(awk '/cpu_core\/cache-misses\//{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null)
    BMISS=$(awk '/cpu_core\/branch-misses\//{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null)

    if [ -z "$CYCLES" ]; then CYCLES=$(awk '/cycles/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi
    if [ -z "$INST" ]; then INST=$(awk '/instructions/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi
    if [ -z "$CMISS" ]; then CMISS=$(awk '/cache-misses/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi
    if [ -z "$BMISS" ]; then BMISS=$(awk '/branch-misses/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi
    L1M=""
    L2M=""
    SEND_S=""
    RECV_S=""
    READ_S=""
    if [ -z "$L1M" ]; then L1M=$(awk '/L1-dcache-load-misses/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi
    if [ -z "$L2M" ]; then L2M=$(awk '/L2-load-misses/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi
    if [ -z "$SEND_S" ]; then SEND_S=$(awk '/syscalls:sys_enter_sendto/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi
    if [ -z "$RECV_S" ]; then RECV_S=$(awk '/syscalls:sys_enter_recvfrom/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi
    if [ -z "$READ_S" ]; then READ_S=$(awk '/syscalls:sys_enter_read/{gsub(/,/,"",$1); print $1; exit}' "$PERF_STAT" 2>/dev/null); fi

    CTX_S="?"
    if [ -n "$CTX" ] && [ -n "$SEC" ] && [ "$SEC" != "0" ]; then
        CTX_S=$(awk "BEGIN{printf \"%.0f\", $CTX/$SEC}")
        echo ""
        echo "    ctx/s: $CTX_S"
    fi

    IPC="?"
    if [ -n "$INST" ] && [ -n "$CYCLES" ] && [ "$(echo "$INST > 0" | bc -l 2>/dev/null)" -eq 1 ] 2>/dev/null; then
        IPC=$(awk "BEGIN{printf \"%.2f\", $INST/$CYCLES}" 2>/dev/null)
        echo ""
        echo -e "${BOLD}  硬件事件${RESET}"
        echo "    IPC:           $IPC"
        echo "    cache-misses:  $CMISS"
        echo "    branch-misses: $BMISS"
    fi

    TIME_OPTS=()
    if [ -n "$TIME_RANGE" ]; then TIME_OPTS=(--time "$TIME_RANGE"); fi

    echo ""
    echo -e "${BOLD}  CPU 热点 Top 20${RESET}"
    echo ""
    perf report -i "$PERF_DATA" "${TIME_OPTS[@]}" --stdio --no-header 2>/dev/null | grep -v '^ *#' | head -25 | sed 's/^/    /' || true

    SEND=$(perf report -i "$PERF_DATA" "${TIME_OPTS[@]}" --stdio --no-header 2>/dev/null | \
        grep -E "\|--[0-9.]+%--.*send" | head -1 | sed -n 's/.*|--\([0-9.]*\)%--.*/\1/p' || echo "")
    SEND="${SEND:-100.00}"
    echo ""
    echo "    send(): ${SEND}% CPU"

    if [ -f "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ] && \
       [ -f "$FLAMEGRAPH_DIR/flamegraph.pl" ]; then
        mkdir -p "$BASE/profiling"
        FLAME_SVG="$BASE/profiling/nebulaX_flame_${MODE_NAME}.svg"
        echo ""
        echo -e "${BOLD}  火焰图${RESET}"
        if [ -n "$TIME_RANGE" ]; then
            perf script -i "$PERF_DATA" --time "$TIME_RANGE" 2>/dev/null
        else
            perf script -i "$PERF_DATA" 2>/dev/null
        fi | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" 2>/dev/null \
            | "$FLAMEGRAPH_DIR/flamegraph.pl" > "$FLAME_SVG" 2>/dev/null || true
        if [ -s "$FLAME_SVG" ]; then
            ok "火焰图: $FLAME_SVG"
        fi
    fi

    echo ""
    echo -e "${BOLD}  == 汇总 ==${RESET}"
    echo "    avg=${B_AVG}us  P50=${B_P50}us  P99=${B_P99}us  P999=${B_P999}us  QPS=${B_QPS}  ctx/s=${CTX_S}  IPC=${IPC}  send=${SEND}%"
fi

echo ""
echo "=============================="
echo -e "${GREEN}  压测完成${RESET}"
echo "=============================="
