/*
 * hello_module.c —— 从内核里看 task_struct：当前进程是谁、系统里有多少进程
 *
 * 这是用户态和内核态之间的第一座桥：
 *   - 用户态读 /proc 得到的 pid、comm、state，在这里是 current 指针直接指到的字段；
 *   - for_each_process 遍历的是 init_task 拉出来的 task 链表（不是 /proc 的遍历逻辑，
 *     但 /proc 的 next_tgid 最终也是走同一份数据结构）。
 *
 * 编译（需要当前内核的 headers）：
 *   make -C /lib/modules/$(uname -r)/build M=$PWD modules
 * 加载：
 *   sudo insmod hello_module.ko who=root
 *   dmesg | tail
 * 卸载：
 *   sudo rmmod hello_module
 *
 * 在 QEMU 实验里（见 qemu/README.md）可以直接 insmod 到 guest，
 * 配合 gdb 断在 hello_init，就能看到 current、init_task 的真实地址。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/jiffies.h>
#include <linux/init.h>
#include <linux/string.h>

static char *who = "anyone";
module_param(who, charp, 0644);
MODULE_PARM_DESC(who, "只打印 comm 匹配这个值的进程");

static int __init hello_init(void)
{
    struct task_struct *p;
    int count = 0, matched = 0;

    pr_info("hello_module: current=%s pid=%d tgid=%d state=%ld prio=%d policy=%d\n",
            current->comm, current->pid, current->tgid,
            (long)current->__state, current->prio, current->policy);
    pr_info("hello_module: jiffies=%lu HZ=%d on_cpu=%d\n",
            jiffies, HZ, current->on_cpu);

    for_each_process(p) {
        count++;
        if (strcmp(p->comm, who) == 0) {
            matched++;
            pr_info("  match: %-16s pid=%-6d state=%ld exec=%llu ns\n",
                    p->comm, p->pid, (long)p->__state,
                    (unsigned long long)p->se.sum_exec_runtime);
        }
    }
    pr_info("hello_module: 共 %d 个 task（含内核线程），匹配 \"%s\" 的 %d 个\n",
            count, who, matched);

    /* 这里可以直接读 mm / se / sched_class，用来对照用户态看到的数字 */
    if (current->mm) {
        pr_info("hello_module: mm->start_code=%#lx start_brk=%#lx brk=%#lx\n",
                current->mm->start_code, current->mm->start_brk, current->mm->brk);
    }
    pr_info("hello_module: se.sum_exec_runtime=%llu ns, nr_switches=%llu\n",
            (unsigned long long)current->se.sum_exec_runtime,
            (unsigned long long)current->nvcsw + current->nivcsw);
    return 0;
}

static void __exit hello_exit(void)
{
    pr_info("hello_module: bye, current=%s pid=%d\n", current->comm, current->pid);
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("linux_c_tutorial");
MODULE_DESCRIPTION("inspect task_struct from inside the kernel");
