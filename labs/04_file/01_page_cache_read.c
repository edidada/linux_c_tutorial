/*
 * 01_page_cache_read.c —— 同一次 read，命中 page cache 与真正读磁盘差多少
 *
 * 内核视角（fs/read_write.c -> vfs_read -> filemap_read）：
 *   - 第一次读某页：page cache 里没有 -> filemap_fault / read_pages 触发磁盘 I/O，
 *     把页放进 inode->i_mapping 的 xarray，然后拷贝到用户缓冲区。
 *   - 再读同一页：直接 copy_page_to_iter，不产生 I/O，也不产生 major fault。
 *   - posix_fadvise(POSIX_FADV_DONTNEED)：让内核丢弃这段 range 的 clean page，
 *     不需要 root 权限，也不需要动全局 drop_caches，是做冷/热对比最干净的办法。
 *   - 顺序读会触发 readahead（默认 128KB 窗口），所以实际 I/O 是按批提交的，
 *     单次 read 的延迟被摊薄，这也是顺序读比随机读快得多的原因之一。
 *
 * 本程序：造一个 128MB 文件，测三次顺序读的吞吐：写完后立刻读（可能部分命中）、
 * 丢缓存后读（冷）、再读一次（热）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#define SIZE_MB 128

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* 顺序读一遍，返回 MB/s */
static double read_pass(int fd, size_t total)
{
    char *buf = malloc(1 << 20);
    if (!buf) return -1;
    lseek(fd, 0, SEEK_SET);
    size_t done = 0;
    double t0 = now_ms();
    while (done < total) {
        ssize_t n = read(fd, buf, 1 << 20);
        if (n <= 0) break;
        done += n;
    }
    double ms = now_ms() - t0;
    free(buf);
    lseek(fd, 0, SEEK_SET);
    return ms > 0 ? (double)done / (1024.0 * 1024.0) / (ms / 1000.0) : -1;
}

int main(void)
{
    const char *path = "/tmp/linux_c_tutorial_pc.bin";
    size_t total = (size_t)SIZE_MB << 20;

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }

    printf("造文件中（%d MB）...\n", SIZE_MB);
    char *blk = malloc(1 << 20);
    memset(blk, 'A', 1 << 20);
    for (size_t off = 0; off < total; off += (1 << 20))
        if (write(fd, blk, 1 << 20) < 0) { perror("write"); return 1; }
    fsync(fd);
    free(blk);

    double r1 = read_pass(fd, total);

    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    double r2 = read_pass(fd, total);

    double r3 = read_pass(fd, total);

    printf("\n%-30s %10s\n", "场景", "MB/s");
    printf("%-30s %10.1f\n", "写完后立刻读（部分页还在 cache）", r1);
    printf("%-30s %10.1f\n", "fadvise DONTNEED 后读（冷，真的读盘）", r2);
    printf("%-30s %10.1f\n", "紧接着再读（热，全程 page cache）", r3);
    printf("\n倍率：热读是冷读的 %.1f 倍\n", r3 / r2);
    printf("\n可继续做的实验：换 O_DIRECT 打开再读一遍（绕过 page cache），\n");
    printf("对比它和 buffered 读的吞吐与 CPU 占用（O_DIRECT 要求缓冲区按 512B/4KB 对齐）。\n");

    close(fd);
    unlink(path);
    return 0;
}
