/*
 * niceload.c —— 设置自己的 nice 后忙循环。
 *
 * 为什么需要它：QEMU guest 里只有 busybox，没有 nice / renice applet，
 * 也没法从 shell 改调度参数。静态编译后塞进 initramfs：
 *
 *   gcc -O2 -static -o niceload niceload.c
 *
 * guest 里：
 *   /bin/niceload 10 &      # nice 10，权重 110 * 1024 = 112640
 *   /bin/niceload 0 &       # nice 0，权重 1024 * 1024 = 1048576
 *
 * 配合 labs/07_sched_kernel/fair_pick_dbg.patch 的内核插桩，
 * dmesg 里能看到 weight 字段，/proc/<pid>/stat 能看到 utime。
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/resource.h>

int main(int argc, char **argv)
{
	int nice = argc > 1 ? atoi(argv[1]) : 0;

	setpriority(PRIO_PROCESS, 0, nice);
	printf("niceload pid=%d nice=%d\n", getpid(), getpriority(PRIO_PROCESS, 0));
	fflush(stdout);

	volatile unsigned long x = 0;
	for (;;)
		x++;

	return 0;
}
