/*
 * 02_fsync_cost.c —— write 返回不等于落盘：page cache 与 fsync 的代价
 *
 * 内核视角：
 *   - write() 只把数据拷进 page cache（copy_page_from_iter），把页标记 PG_dirty，
 *     然后返回。此时掉电就丢——数据只在内存里。
 *   - 什么时候写磁盘：writeback 线程（flusher）周期性把脏页交给块层；
 *     脏页有 dirty_expire_centisecs（默认 30s）和 dirty_ratio 两个约束。
 *   - fsync() -> vfs_fsync_range -> filemap_write_and_wait_range -> 提交 I/O 并等待完成，
 *     再 -> f_op->fsync 刷文件元数据（ext4 走 jbd2 日志提交）。
 *   - 所以「每条都 fsync」和「攒一批再 fsync」的差距不是磁盘带宽，而是
 *     每次都要等一次日志提交 + 设备 flush cache。
 *
 * 本程序三种写法各 200 条 4KB 记录：
 *   A. 每条 write 后立刻 fsync
 *   B. 每 50 条 write 后 fsync 一次
 *   C. 只 write，最后 fsync 一次
 * 打印总耗时和每条平均耗时。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define RECORDS 200
#define REC_SIZE 4096

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static char rec[REC_SIZE];

/* mode: 1=每条 fsync, 50=每 50 条 fsync, 0=最后一次性 fsync */
static double run(const char *path, int batch)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return -1; }
    double t0 = now_ms();
    for (int i = 0; i < RECORDS; i++) {
        rec[0] = (char)i;
        if (write(fd, rec, REC_SIZE) != REC_SIZE) { perror("write"); break; }
        if (batch > 0 && (i + 1) % batch == 0) fsync(fd);
    }
    fsync(fd);                    /* 保证最后一批也落盘，三种模式口径一致 */
    double ms = now_ms() - t0;
    close(fd);
    return ms;
}

int main(void)
{
    memset(rec, 'R', sizeof rec);

    double a = run("/tmp/linux_c_tutorial_fsync_a.bin", 1);
    double b = run("/tmp/linux_c_tutorial_fsync_b.bin", 50);
    double c = run("/tmp/linux_c_tutorial_fsync_c.bin", 0);

    printf("%d 条 * %d B = %.1f MB\n\n", RECORDS, REC_SIZE,
           (double)RECORDS * REC_SIZE / 1048576.0);
    printf("%-34s %10s %12s\n", "写法", "总耗时ms", "每条us");
    printf("%-34s %10.1f %12.1f\n", "每条 write 后 fsync", a, a * 1000 / RECORDS);
    printf("%-34s %10.1f %12.1f\n", "每 50 条 fsync 一次", b, b * 1000 / RECORDS);
    printf("%-34s %10.1f %12.1f\n", "只 write，最后 fsync 一次", c, c * 1000 / RECORDS);
    printf("\n差距来自「等 I/O 完成 + 等日志提交」的次数，不是数据量。\n");
    printf("真实系统里这就是 WAL/redo 组提交（group commit）存在的理由。\n");
    printf("\n延伸：把 fsync 换成 fdatasync（不刷 mtime 等元数据）看差多少；\n");
    printf("用 `cat /proc/sys/vm/dirty_expire_centisecs` 看 writeback 的节奏。\n");

    unlink("/tmp/linux_c_tutorial_fsync_a.bin");
    unlink("/tmp/linux_c_tutorial_fsync_b.bin");
    unlink("/tmp/linux_c_tutorial_fsync_c.bin");
    return 0;
}
