/*
 * 02_wakeup_latency.c —— 一次唤醒到真正被调度上 CPU，要多久
 *
 * 内核视角：
 *   - 写管道让对端从 read 的睡眠里醒来：wake_up_process -> try_to_wake_up ->
 *     ttwu_queue -> 把任务放进目标 CPU 的 rq，设置 need_resched。
 *   - 如果被唤醒的任务优先级比当前任务高（或当前任务用完 slice），
 *     内核在返回用户态前或抢占点调用 __schedule() 切过去；
 *     否则要等当前任务主动让出或时间片用完。
 *   - 这条路径的长度决定了「唤醒延迟」（wakeup latency），是实时性的核心指标。
 *     普通 CFS/EEVDF 任务通常几十微秒，PREEMPT_RT 可以做到个位数微秒。
 *
 * 本程序：两个线程绑在同一个 CPU 上，用两个管道做 ping-pong 2 万次，
 * 统计每次往返时间，取 p50/p90/p99/max，并对半估算单向唤醒延迟。
 *
 * 注意：WSL2 里内核是微软定制版，且 VM 有额外开销，绝对值只能代表本机；
 * 重点看分布形状（p99 与 p50 的差距）以及不同调度参数下的变化。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#define ROUNDS 20000

static int ab[2], ba[2];

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void *echo_thread(void *arg)
{
    (void)arg;
    char c;
    for (;;) {
        if (read(ab[0], &c, 1) != 1) break;
        if (write(ba[1], &c, 1) != 1) break;
    }
    return NULL;
}

static void pin_to_one_cpu(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);   /* 从允许的 CPU 集合里取一个 */
    int cpu = -1;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &set)) { cpu = i; break; }
    if (cpu < 0) return;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) < 0) perror("setaffinity");
    printf("两个线程都绑到 CPU %d（同一个 runqueue 才测得到唤醒延迟）\n", cpu);
}

int main(void)
{
    if (pipe(ab) < 0 || pipe(ba) < 0) { perror("pipe"); return 1; }
    pin_to_one_cpu();

    pthread_t th;
    pthread_create(&th, NULL, echo_thread, NULL);
    /* 新线程默认继承创建者的 affinity，所以它也在同一个 CPU 上 */

    double *samples = malloc(sizeof(double) * ROUNDS);
    if (!samples) return 1;

    char c = 'x';
    usleep(100000);                    /* 让 echo 线程先阻塞在 read 上 */

    for (int i = 0; i < ROUNDS; i++) {
        double t0 = now_ns();
        if (write(ab[1], &c, 1) != 1) { perror("write"); break; }
        if (read(ba[0], &c, 1) != 1) { perror("read"); break; }
        samples[i] = now_ns() - t0;
    }

    close(ab[1]);                      /* 让 echo 线程退出 */
    pthread_join(th, NULL);

    qsort(samples, ROUNDS, sizeof(double), cmp);
    printf("\n往返时间统计（ns），单向唤醒延迟约为其一半：\n");
    printf("  p50  = %8.0f  (单向 ~%.1f us)\n", samples[ROUNDS / 2], samples[ROUNDS / 2] / 2000.0);
    printf("  p90  = %8.0f  (单向 ~%.1f us)\n", samples[ROUNDS * 9 / 10], samples[ROUNDS * 9 / 10] / 2000.0);
    printf("  p99  = %8.0f  (单向 ~%.1f us)\n", samples[ROUNDS * 99 / 100], samples[ROUNDS * 99 / 100] / 2000.0);
    printf("  max  = %8.0f  (单向 ~%.1f us)\n", samples[ROUNDS - 1], samples[ROUNDS - 1] / 2000.0);
    printf("  min  = %8.0f\n", samples[0]);

    printf("\n尾部延迟（p99/max）远大于 p50，说明偶发的中断、其他任务抢占、\n");
    printf("以及 VM 的 vCPU 调度都会把唤醒推后。这正是实时系统要单独做延迟分析的原因。\n");

    free(samples);
    return 0;
}
