/*
 * 04_inotify.c —— 从 inotify 事件反推 VFS 的钩子位置
 *
 * 内核视角（fs/notify）：
 *   - inotify_add_watch 在内核里创建一个 inotify_mark，挂到目标 inode 的
 *     i_fsnotify_marks 上。
 *   - VFS 在关键动作里调用 fsnotify()：
 *       vfs_create/vfs_unlink/vfs_rename  -> FS_CREATE/FS_DELETE/FS_MOVE
 *       vfs_write -> file_modified -> fsnotify_modify
 *       chmod/chown 走 notify_change -> fsnotify_attrib
 *   - 事件先进入 inotify 的 event queue，read(inotify_fd) 从队列里取，
 *     所以 inotify 的事件也是「普通文件描述符 + read」的模型，可以用 poll/epoll 等待。
 *   - 它监听的是 inode 上的操作，不是路径字符串；文件被 rename 之后 watch 仍然有效。
 *
 * 本程序：监听一个临时目录，子进程依次 create/write/chmod/rename/unlink，
 * 父进程把事件打印出来（mask 展开成可读名字）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define DIR "/tmp/linux_c_tutorial_inotify"

static void print_mask(unsigned int m)
{
    struct { unsigned int bit; const char *name; } tab[] = {
        { IN_ACCESS, "ACCESS" }, { IN_MODIFY, "MODIFY" }, { IN_ATTRIB, "ATTRIB" },
        { IN_CLOSE_WRITE, "CLOSE_WRITE" }, { IN_CLOSE_NOWRITE, "CLOSE_NOWRITE" },
        { IN_OPEN, "OPEN" }, { IN_MOVED_FROM, "MOVED_FROM" }, { IN_MOVED_TO, "MOVED_TO" },
        { IN_CREATE, "CREATE" }, { IN_DELETE, "DELETE" }, { IN_DELETE_SELF, "DELETE_SELF" },
        { IN_MOVE_SELF, "MOVE_SELF" }, { IN_ISDIR, "ISDIR" },
    };
    int first = 1;
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (m & tab[i].bit) { printf("%s%s", first ? "" : "|", tab[i].name); first = 0; }
    if (first) printf("0x%x", m);
}

int main(void)
{
    mkdir(DIR, 0755);
    int ifd = inotify_init1(IN_CLOEXEC);
    if (ifd < 0) { perror("inotify_init1"); return 1; }
    int wd = inotify_add_watch(ifd, DIR, IN_ALL_EVENTS);
    if (wd < 0) { perror("inotify_add_watch"); return 1; }

    pid_t child = fork();
    if (child == 0) {
        usleep(100000);
        const char *f1 = DIR "/a.txt";
        const char *f2 = DIR "/b.txt";
        int fd = open(f1, O_CREAT | O_WRONLY, 0644);     /* CREATE + OPEN */
        if (write(fd, "hello", 5) < 0) perror("write");     /* MODIFY */
        close(fd);                                        /* CLOSE_WRITE */
        chmod(f1, 0600);                                  /* ATTRIB */
        rename(f1, f2);                                   /* MOVED_FROM + MOVED_TO */
        unlink(f2);                                       /* DELETE */
        _exit(0);
    }

    waitpid(child, NULL, 0);       /* 子进程先做完，事件都进了队列 */
    usleep(100000);                /* 等内核把剩下的通知投递完 */

    printf("=== inotify 收到的事件序列 ===\n");
    char buf[4096] __attribute__((aligned(8)));
    ssize_t n;
    while ((n = read(ifd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *e = (struct inotify_event *)p;
            printf("  wd=%d mask=", e->wd);
            print_mask(e->mask);
            if (e->len) printf("  name=%s", e->name);
            printf("\n");
            p += sizeof(struct inotify_event) + e->len;
        }
        if (n < (ssize_t)sizeof(buf)) break;
    }

    inotify_rm_watch(ifd, wd);
    close(ifd);
    rmdir(DIR);

    printf("\n对照内核：每个事件背后都是一次 VFS 操作里的 fsnotify 调用，\n");
    printf("所以用 inotify 可以「看见」VFS 在哪些代码路径上挂了钩子。\n");
    printf("延伸：把 watch 加到文件而不是目录，再 rename 它，验证 watch 跟着 inode 走。\n");
    return 0;
}
