#!/usr/bin/env bash
# snapshot.sh —— 给一个进程拍一张内核视角的快照
#
# 用法：
#   ./scripts/snapshot.sh <pid>
#   ./scripts/snapshot.sh <pid> 0.5     # 每 0.5s 刷一次，Ctrl-C 退出
#
# 输出对应内核里的哪些字段，见 docs/03-process.md 的对照表。
set -euo pipefail

pid=${1:?用法: snapshot.sh <pid> [interval]}
interval=${2:-}

one() {
    echo "===== $(date +%H:%M:%S) pid=$pid ====="
    if [ ! -d "/proc/$pid" ]; then echo "进程不存在"; return 1; fi
    grep -E '^(Name|State|Tgid|Pid|PPid|Threads|VmPeak|VmRSS|voluntary_ctxt|nonvoluntary_ctxt)' \
        "/proc/$pid/status" || true
    echo "--- sched ---"
    grep -E '^(se.sum_exec_runtime|nr_switches|nr_voluntary_switches|nr_involuntary_switches|se.nr_migrations|policy|prio)' \
        "/proc/$pid/sched" || true
    echo "--- smaps_rollup ---"
    grep -E '^(Rss|Pss|Shared_Clean|Shared_Dirty|Private_Clean|Private_Dirty|Anonymous|Swap)' \
        "/proc/$pid/smaps_rollup" 2>/dev/null || true
    echo
}

if [ -z "$interval" ]; then
    one
else
    while :; do
        one || exit 1
        sleep "$interval"
    done
fi
