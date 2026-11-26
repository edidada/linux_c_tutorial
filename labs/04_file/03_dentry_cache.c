/*
 * 03_dentry_cache.c —— 路径解析的成本，以及 dentry cache 命中与未命中
 *
 * 内核视角（fs/namei.c）：
 *   - 每次 stat/open 都要走 link_path_walk() 逐级解析路径。
 *     每一级先在 dentry cache（dcache）里按 (parent, name) 哈希查找。
 *     命中：直接拿到 dentry，再用 d_seq 校验，不用碰磁盘。
 *     未命中：调用 inode 所在文件系统的 lookup()（如 ext4_lookup）读目录块，
 *     把结果实例化成一个 dentry 挂进 dcache，同时分配 inode。
 *   - 查找失败也会缓存：negative dentry（dentry 存在但 inode 为空），
 *     所以反复 stat 一个不存在的文件，第一次之后都不再读盘。
 *   - inode 有引用计数和 LRU（super_block->s_inodes），内存紧张时被回收；
 *     dentry 也一样（dentry->d_lru）。
 *   这解释了为什么「第一次访问某个目录很慢，后面很快」，也解释了容器里挂载很多层时
 *   路径解析为什么贵：每一层 mount 都增加一次 path walk 的节点。
 *
 * 本程序：建 2000 个空文件，分别测
 *   A. 反复 stat 已存在的文件（dcache 命中）
 *   B. 反复 stat 不存在的路径（negative dentry 命中）
 *   C. unlink 之后再 stat（缓存失效，重新 lookup，最终变成 negative）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#define N 2000
#define DIR "/tmp/linux_c_tutorial_dcache"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void)
{
    char path[256];
    mkdir(DIR, 0755);

    printf("创建 %d 个文件...\n", N);
    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof path, DIR "/f%04d", i);
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) { perror("open"); return 1; }
        close(fd);
    }

    struct stat st;
    double t0 = now_ms();
    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof path, DIR "/f%04d", i);
        stat(path, &st);
    }
    double hit = now_ms() - t0;

    t0 = now_ms();
    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof path, DIR "/nope%04d", i);
        stat(path, &st);
    }
    double neg = now_ms() - t0;

    printf("删除全部文件...\n");
    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof path, DIR "/f%04d", i);
        unlink(path);
    }

    t0 = now_ms();
    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof path, DIR "/f%04d", i);
        stat(path, &st);
    }
    double miss = now_ms() - t0;
    rmdir(DIR);

    printf("\n%-38s %10s %12s\n", "场景", "总ms", "单次us");
    printf("%-38s %10.1f %12.2f\n", "stat 已存在（dcache+inode 命中）", hit, hit * 1000 / N);
    printf("%-38s %10.1f %12.2f\n", "stat 不存在（negative dentry 命中）", neg, neg * 1000 / N);
    printf("%-38s %10.1f %12.2f\n", "unlink 后 stat（缓存失效，需 lookup）", miss, miss * 1000 / N);

    printf("\n这组数字只能说明「路径解析在 dcache 命中时是微秒级」：\n");
    printf("三种情况都在同量级，因为 lookup 一旦不碰磁盘，成本主要就是 syscall 本身。\n");
    printf("想看真正的 miss 代价，需要 root 执行 echo 2 > /proc/sys/vm/drop_caches，\n");
    printf("或者挂一个冷目录（新 mount 的 NFS/overlay 层）再 stat 一遍。\n");
    printf("negative dentry 的意义在于：反复探测不存在的路径时不会每次都读目录块，\n");
    printf("这也是很多程序用「先 stat 再 open」不会明显变慢的原因。\n");
    return 0;
}
