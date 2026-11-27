/*
 * touchmem.c —— 让缺页次数可以被数出来：4MB = 1024 页 = 1024 次 minflt
 *
 * 为什么单独做这个：RSS 实验能看到「RSS 涨了多少」，但看不到「涨的过程分了
 * 多少次」。这里逐页写 4MB（每页一次匿名缺页），把计数和 QEMU 里
 * do_anonymous_page 的命中次数对一遍（见 qemu/fault.gdb）。
 *
 * 两个来源对账：
 *   getrusage() 的 ru_minflt —— 本进程视角，内核在缺页处理里加一
 *   /proc/self/stat 的第 10 字段 minflt —— 同一本账的另一种读法
 *
 * 两个坑写在代码里：
 *   1. 用 mmap 而不是 malloc：malloc 的 chunk 头和首元素同页，会少一次缺页；
 *   2. 写操作必须走 volatile：非 volatile 的整段写入会被 -O2 直接优化掉。
 *
 * 用法： ./touchmem
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>

#define MB (1UL << 20)
#define N  (4UL * MB)

static long proc_stat_minflt(void)
{
	FILE *f = fopen("/proc/self/stat", "r");
	long dummy, minflt = -1, majflt = -1;
	char comm[64], state;

	if (!f)
		return -1;
	/* 字段：pid comm state ppid ... minflt(10) ... majflt(12) */
	if (fscanf(f, "%ld %63s %c %ld %ld %ld %ld %ld %ld %ld %ld %ld",
		   &dummy, comm, &state, &dummy, &dummy, &dummy, &dummy,
		   &dummy, &dummy, &minflt, &dummy, &majflt) != 12)
		minflt = -1;
	fclose(f);
	(void)majflt;
	return minflt;
}

static unsigned long minflt_now(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_minflt;
}

int main(void)
{
	char *p;
	volatile char *vp;
	unsigned long i, t0, t1, t2;
	long p0, p1, p2;

	/* 直接 mmap 一块匿名内存：VMA 干净，正好 N/4K 页，不受 malloc 头影响 */
	p = mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	vp = p;

	printf("touchmem: 映射 %lu MB = %lu 页\n", N / MB, N / 4096);

	p0 = proc_stat_minflt();
	t0 = minflt_now();
	for (i = 0; i < N; i += 4096)
		vp[i] = 0x5a;        /* 第一次触碰：每页一次匿名缺页 */
	t1 = minflt_now();
	p1 = proc_stat_minflt();

	printf("touchmem: 首轮 getrusage minflt = %lu（每页 %.2f 次）\n",
	       t1 - t0, (double)(t1 - t0) / (N / 4096));
	printf("touchmem: 首轮 /proc/self/stat 增量 = %ld（同一本账的另一种读法）\n",
	       p1 - p0);

	/* 再逐页写一遍：页已经 resident，不应该再产生缺页 */
	for (i = 0; i < N; i += 4096)
		vp[i] = (char)i;
	t2 = minflt_now();
	p2 = proc_stat_minflt();
	printf("touchmem: 第二轮 getrusage = %lu, /proc/stat 增量 = %ld（页已驻留）\n",
	       t2 - t1, p2 - p1);
	printf("touchmem: 第二轮剩的几次是 printf 自己申请缓冲造成的，与这 4MB 无关\n");

	return 0;
}
