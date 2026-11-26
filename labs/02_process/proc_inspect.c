/*
 * proc_inspect.c —— 用 /proc 把 task_struct 的字段读出来
 *
 * 目的：让 /proc 里的每个数字都能对上内核结构体字段，避免「只记命令不懂来源」。
 *
 * 对照表（task_struct / mm_struct / signal_struct）：
 *   /proc/<pid>/status  Name/State/Pid/PPid/Threads/VmRSS/voluntary_ctxt_switches
 *   /proc/<pid>/stat     state, ppid, utime, stime, num_threads, starttime, vsize, rss
 *   /proc/<pid>/statm    total program size / resident / shared / text / lib / data / dt
 *   /proc/<pid>/sched    se.sum_exec_runtime, nr_switches, nr_voluntary_switches,
 *                        clock-delta, policy, prio（CFS 的调度实体直接暴露在这里）
 *   /proc/<pid>/limits   每个 rlimit，对应 signal_struct->rlim[]
 *   /proc/<pid>/maps     每个 VMA：起始-结束 权限 offset dev inode path
 *
 * 读 /proc 本身也走 VFS：procfs 的 inode 在 open 时绑定到 pid，
 * 每个 read 调用 proc_pid_status 之类的函数现场拼字符串，所以它不是磁盘文件。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void dump_status(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) { perror("status"); return; }
    char line[512];
    printf("=== /proc/self/status 摘录 ===\n");
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Name:", 5) || !strncmp(line, "State:", 6) ||
            !strncmp(line, "Tgid:", 5) || !strncmp(line, "Pid:", 4) ||
            !strncmp(line, "PPid:", 5) || !strncmp(line, "Threads:", 8) ||
            !strncmp(line, "VmPeak:", 7) || !strncmp(line, "VmRSS:", 6) ||
            !strncmp(line, "voluntary_ctxt", 14) ||
            !strncmp(line, "nonvoluntary_ctxt", 17))
            fputs(line, stdout);
    }
    fclose(f);
}

static void dump_stat(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) { perror("stat"); return; }
    char buf[1024];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return; }
    fclose(f);

    /* 字段顺序固定，但 comm 里可能有空格，先跳过 "(...)"；之后按空格切分取下标 */
    char *p = strrchr(buf, ')');
    if (!p) return;
    char *tok[64] = {0};
    int ntok = 0;
    for (char *t = strtok(p + 1, " \n"); t && ntok < 64; t = strtok(NULL, " \n"))
        tok[ntok++] = t;
    if (ntok < 22) return;

    /* tok[i] 对应 field = i+3：0=state, 1=ppid, 11=utime, 12=stime, 17=num_threads,
     * 20=vsize, 21=rss */
    printf("\n=== /proc/self/stat 关键字段 ===\n");
    printf("state=%s  ppid=%s  utime=%s ticks  stime=%s ticks\n",
           tok[0], tok[1], tok[11], tok[12]);
    printf("num_threads=%s  vsize=%s bytes  rss=%s pages\n", tok[17], tok[20], tok[21]);
    printf("（utime/stime 单位是 USER_HZ，通常 100，即 1 tick = 10ms；\n");
    printf("  内核在 account_user_time 里把 CPU 时间累加到 signal_struct->utime）\n");
}

static void dump_sched(void)
{
    FILE *f = fopen("/proc/self/sched", "r");
    if (!f) { perror("sched"); return; }
    char line[512];
    printf("\n=== /proc/self/sched 摘录（CFS/EEVDF 调度实体）===\n");
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "se.sum_exec_runtime", 19) ||
            !strncmp(line, "nr_switches", 11) ||
            !strncmp(line, "nr_voluntary_switches", 21) ||
            !strncmp(line, "nr_involuntary_switches", 23) ||
            !strncmp(line, "se.nr_migrations", 16) ||
            !strncmp(line, "clock-delta", 11) ||
            !strncmp(line, "policy", 6) ||
            !strncmp(line, "prio", 4))
            fputs(line, stdout);
    }
    fclose(f);
}

static void dump_maps_head(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { perror("maps"); return; }
    char line[512];
    int n = 0;
    printf("\n=== /proc/self/maps 前 8 个 VMA ===\n");
    while (fgets(line, sizeof line, f) && n < 8) { fputs(line, stdout); n++; }
    fclose(f);
}

int main(void)
{
    printf("本进程 pid=%d，下面所有内容都来自内核现场构造的 procfs 文件\n\n", getpid());
    dump_status();
    dump_stat();
    dump_sched();
    dump_maps_head();

    printf("\n=== 对照 task_struct（include/linux/sched.h）===\n");
    printf("  pid/tgid        -> status 的 Pid/Tgid\n");
    printf("  __state        -> status 的 State（R/S/D/Z...）\n");
    printf("  mm->start_brk  -> maps 里 [heap] 的起点\n");
    printf("  mm->mmap       -> maps 的第一行（VMA 链表头）\n");
    printf("  se.sum_exec_runtime -> sched 里的同名字段\n");
    printf("  signal->rlim[] -> limits\n");
    return 0;
}
