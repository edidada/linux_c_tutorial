/*
 * exec_pipe.c —— fork + exec + pipe：一个 shell 管道的最小实现
 *
 * 内核视角：
 *   execve() -> do_execveat_common -> bprm -> search_binary_handler
 *     -> load_elf_binary（fs/binfmt_elf.c）:
 *        flush_old_exec()      丢掉旧 mm，换新 mm（exec_mmap）
 *        setup_new_exec()      设置 personality、栈上限
 *        elf_map()             按 PT_LOAD 建立 VMA，文件页按需缺页
 *        load_elf_interp()     加载 /lib64/ld-linux-x86-64.so.2
 *        start_thread()        把返回地址设成动态链接器入口
 *     然后返回用户态，从新映像的第一条指令开始执行。
 *   关键点：exec 不创建新进程，pid 不变，只是「换了内存映像和代码」。
 *   文件描述符表保留（除非 O_CLOEXEC / FD_CLOEXEC），所以管道能跨 exec 传递；
 *   exec 时内核在 do_close_on_exec() 里关闭带 CLOEXEC 标记的 fd。
 *
 * 本程序：父进程建管道、打开两个文件（一个带 O_CLOEXEC），fork 后
 *   子进程把 stdout 重定向到管道写端，再 exec `ls -l /proc/self/fd`，
 *   看子进程里还剩哪些 fd；父进程从管道读并等待退出状态。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

int main(void)
{
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }

    int keep = open("/etc/hostname", O_RDONLY);                    /* 会被继承 */
    int cloexec = open("/etc/hostname", O_RDONLY | O_CLOEXEC);     /* exec 时被关掉 */
    printf("父进程: pipe=[%d,%d] keep=%d cloexec=%d\n", fds[0], fds[1], keep, cloexec);
    printf("（O_CLOEXEC 在 open 时就设好了 FD_CLOEXEC，也可以事后 fcntl 设置）\n\n");

    fflush(stdout);   /* fork 会复制 stdio 缓冲区，不 flush 会打印两遍 */

    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }

    if (child == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);         /* 让 ls 的输出进管道 */
        if (fds[1] != STDOUT_FILENO) close(fds[1]);

        printf("子进程 exec 前 pid=%d，接下来 exec ls，pid 不会变\n", getpid());
        fflush(stdout);

        execlp("ls", "ls", "-l", "/proc/self/fd", (char *)NULL);
        perror("execlp");                    /* 只有失败才会到这里 */
        _exit(127);
    }

    close(fds[1]);
    close(keep);                             /* 父进程这边不读，关掉避免管道不结束 */
    close(cloexec);

    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) {
        buf[n] = 0;
        fputs(buf, stdout);                  /* 子进程 ls 的结果 */
    }
    close(fds[0]);

    int status = 0;
    waitpid(child, &status, 0);
    if (WIFEXITED(status))
        printf("\n子进程退出码 = %d（ls 自己的退出码经由 exit_group 传回）\n",
               WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("\n子进程被信号 %d 杀死\n", WTERMSIG(status));

    printf("\n解读：输出里能看到 fd 0/1/2 和那个没带 O_CLOEXEC 的 fd，\n");
    printf("带 O_CLOEXEC 的已经不在表里了。管道能工作，靠的就是 fd 跨 exec 保留 + 引用计数。\n");
    return 0;
}
