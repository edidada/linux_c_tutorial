/*
 * 01_nice_effect.c —— nice 值如何换算成 CPU 份额
 *
 * 内核视角（kernel/sched/fair.c）：
 *   - 每个任务有一个权重 sched_prio_to_weight[nice+20]，nice 0 = 1024，
 *     每降一级 nice（更优先）权重约 ×1.25，每升一级约 ÷1.25。
 *   - 老 CFS：vruntime += 实际运行时间 * (NICE_0_LOAD / 权重)，权重小的任务
 *     vruntime 涨得快，很快被排到红黑树右边，于是分到的时间少。
 *   - 6.6 之后换成 EEVDF：仍然用权重算 vruntime，但额外给每个实体一个
 *     slice（时间片）和 deadline（= vruntime + slice），调度时选 eligible 且
 *     deadline 最早的。宏观效果仍是按权重分配，只是延迟控制更精细。
 *   - 结论：3 个一直跑满的任务，nice 0/5/10 的 CPU 时间比就是权重比 1024:335:110。
 *
 * 本程序：起 3 个 CPU 密集型子进程（nice 0/5/10），跑 3 秒后读
 * /proc/<pid>/stat 的 utime+stime，算实际份额，与理论值对比。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <string.h>
#include <sched.h>

static const int NICE[3] = { 0, 5, 10 };
/* sched_prio_to_weight 的几个常用点：nice 0/5/10/15/19 */
static const long WEIGHT[3] = { 1024, 335, 110 };

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* 读 /proc/<pid>/stat 的 utime + stime（单位 tick，USER_HZ=100） */
static long cpu_ticks(pid_t pid)
{
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return -1; }
    fclose(f);
    char *p = strrchr(buf, ')');
    if (!p) return -1;

    char *tok[64] = {0};
    int n = 0;
    for (char *t = strtok(p + 1, " \n"); t && n < 64; t = strtok(NULL, " \n"))
        tok[n++] = t;
    if (n < 15) return -1;
    return atol(tok[11]) + atol(tok[12]);      /* field 14,15 */
}

/* 关键：nice 只有在同一个 CPU 上抢同一份算力时才起作用。
 * 16 核机器上三个进程各占一个核，权重比根本不会体现出来。 */
static void pin_to_one_cpu(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);
    int cpu = -1;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &set)) { cpu = i; break; }
    if (cpu < 0) return;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof set, &set);
}

int main(void)
{
    pid_t pids[3] = {0};
    printf("起 3 个忙循环子进程，nice 分别 0 / 5 / 10，跑 3 秒...\n");

    for (int i = 0; i < 3; i++) {
        pid_t p = fork();
        if (p == 0) {
            pin_to_one_cpu();                      /* 三个都挤到同一个核 */
            setpriority(PRIO_PROCESS, 0, NICE[i]);
            volatile unsigned long x = 1;
            for (;;) x = x * 1103515245UL + 12345UL;   /* 纯用户态计算 */
            _exit(0);
        }
        pids[i] = p;
    }

    double t0 = now_ms();
    while (now_ms() - t0 < 3000) usleep(50000);

    long t[3];
    for (int i = 0; i < 3; i++) t[i] = cpu_ticks(pids[i]);

    for (int i = 0; i < 3; i++) kill(pids[i], SIGKILL);
    for (int i = 0; i < 3; i++) waitpid(pids[i], NULL, 0);

    double total = (double)(t[0] + t[1] + t[2]);
    printf("\n%-8s %10s %12s %12s\n", "nice", "cpu ticks", "实际份额", "理论份额");
    for (int i = 0; i < 3; i++) {
        double actual = total > 0 ? t[i] / total * 100.0 : 0;
        double theory = (double)WEIGHT[i] / (WEIGHT[0] + WEIGHT[1] + WEIGHT[2]) * 100.0;
        printf("%-8d %10ld %11.1f%% %11.1f%%\n", NICE[i], t[i], actual, theory);
    }
    printf("\n三个子进程都被绑到同一个 CPU，所以才能看到权重比；\n");
    printf("如果不绑核（比如 16 核机器上各占一核），三个都是 100%，nice 完全不起作用。\n");
    printf("实测值与 1024:335:110 的偏差通常在 1 个百分点以内，超出就说明窗口里有别的负载。\n");

    printf("\n自己看内核怎么看这件事：\n");
    printf("  cat /proc/sched_debug | head -50      # 每个 CPU 的 cfs_rq 和 se 列表\n");
    printf("  cat /proc/<pid>/sched                 # se.sum_exec_runtime / nr_switches\n");
    printf("  cat /proc/sys/kernel/sched_*          # 调度器的可调参数\n");
    return 0;
}
