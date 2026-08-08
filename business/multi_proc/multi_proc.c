/*
 * =============================================================================
 * EdgeSim 多进程管道模块实现文件  multi_proc.c
 * =============================================================================
 * 【文件作用】
 *   实现 multi_proc.h 中声明的全部接口。本文件是 EdgeSim 的核心底层代码，
 *   实现 4 进程隔离架构与双向匿名管道 IPC。
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.2 节
 *
 * 【核心机制】
 *   1. fork() 创建子进程：子进程继承父进程内存（含全局变量副本）
 *   2. pipe() 创建匿名管道：单方向，双向通信需 2 根管道
 *   3. select() 实现非阻塞读写 + 超时：避免死锁
 *   4. SIGCHLD 信号通知子进程退出：父进程用 waitpid 回收僵尸
 *   5. WIFSIGNALED 判断崩溃：被信号杀死则 restart_child() 重 fork
 *
 * 【死锁避免要点（新手重点）】
 *   ① 所有管道 fd 设 O_NONBLOCK，write 不会永久阻塞
 *   ② proc_send/recv 用 select 设超时，超时即返回不死等
 *   ③ 信号处理器只设标志位（sig_atomic_t），不做复杂操作
 *   ④ select 被 SIGCHLD 中断时(EINTR)按超时处理，不重试避免卡死
 *   ⑤ 父进程 kill 子进程后用 waitpid 等待，不残留僵尸
 * =============================================================================
 */


/* =============================================================================
 * 一、头文件包含区（逐条说明作用）
 * ============================================================================= */
#include "multi_proc.h"     /* 本模块公共接口 */

#include <stdio.h>          /* printf/perror（调试日志） */
#include <stdlib.h>         /* exit/abort */
#include <string.h>         /* memset/memcpy */
#include <errno.h>          /* errno/EINTR/EPIPE/EAGAIN */

#include <unistd.h>         /* fork/pipe/close/read/write/getpid/getppid/kill */
#include <fcntl.h>          /* fcntl/F_GETFL/F_SETFL/O_NONBLOCK */
#include <sys/select.h>     /* select/FD_ZERO/FD_SET/struct timeval */
#include <sys/wait.h>       /* waitpid/WNOHANG/WIFSIGNALED/WTERMSIG/WIFEXITED */

#include <signal.h>         /* sigaction/SIGCHLD/SIGPIPE/SIGTERM/SIG_DFL/SIG_IGN */
#include <time.h>           /* time（时间戳） */


/* =============================================================================
 * 二、模块内部全局变量（static，仅本文件可见）
 * =============================================================================
 * 【设计文档要求】
 *   进程数组 proc[4] = {UI, LLM, OCR, ASR}
 *
 * 【变量说明】
 *   g_pids[i]       ：进程 i 的 PID（父进程视角下子进程的 PID）
 *   g_write_fds[i]  ：向进程 i 写数据的管道 fd（-1=无连接）
 *   g_read_fds[i]   ：从进程 i 读数据的管道 fd（-1=无连接）
 *   g_self_id       ：当前进程是哪个（父=UI，子=各自 ID）
 *   g_initialized   ：初始化标志
 *   g_sigchld_flag  ：SIGCHLD 信号标志（volatile sig_atomic_t 保证信号安全）
 *   g_child_handler ：子进程入口回调（fork 后子进程调用）
 * ========================================================================== */

/* 各进程的 PID。父进程视角：g_pids[UI]=自己，g_pids[LLM/OCR/ASR]=子 PID */
static pid_t g_pids[PROC_ID_MAX];

/* 向各进程写数据的 fd。父进程视角：g_write_fds[LLM]=pipe_to_llm 写端 */
static int   g_write_fds[PROC_ID_MAX];

/* 从各进程读数据的 fd。父进程视角：g_read_fds[LLM]=pipe_from_llm 读端 */
static int   g_read_fds[PROC_ID_MAX];

/* 当前进程身份（fork 前默认 UI，fork 后子进程改为各自 ID） */
static proc_id_t g_self_id = PROC_ID_UI;

/* 初始化标志：0=未初始化，1=已初始化 */
static int g_initialized = 0;

/* SIGCHLD 信号标志。
 * volatile：告诉编译器不要优化到寄存器，每次从内存读（可能被信号改）
 * sig_atomic_t：保证读/写是原子的（不会被信号打断到一半）
 * 信号处理器中只能修改这种类型的变量，新手务必记住。 */
static volatile sig_atomic_t g_sigchld_flag = 0;

/* 子进程入口回调。proc_init/restart_child fork 后，子进程调用此函数。 */
static proc_child_fn g_child_handler = NULL;


/* =============================================================================
 * 三、内部辅助函数（static，仅本文件可用）
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 函数名：set_fd_nonblock
 * -----------------------------------------------------------------------------
 * 【作用】将文件描述符设为非阻塞模式
 * 【参数】fd：要设置的文件描述符
 * 【返回值】0=成功，-1=失败
 * 【Linux API 说明】
 *   fcntl(fd, F_GETFL)    ：获取当前 fd 标志位
 *   fcntl(fd, F_SETFL, v) ：设置 fd 标志位
 *   O_NONBLOCK            ：非阻塞标志，设置后 read/write 不再无限等待
 * 【易错点】
 *   必须先 F_GETFL 再 |O_NONBLOCK，不能直接 F_SETFL O_NONBLOCK，
 *   否则会清掉其他标志位（如 O_CLOEXEC）。
 * -----------------------------------------------------------------------------
 */
static int set_fd_nonblock(int fd)
{
    int flags;   /* 当前标志位缓存 */

    /* 获取当前标志 */
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("[multi_proc] fcntl F_GETFL failed");
        return -1;
    }

    /* 追加 O_NONBLOCK 标志并设置 */
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("[multi_proc] fcntl F_SETFL failed");
        return -1;
    }

    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：sigchld_handler
 * -----------------------------------------------------------------------------
 * 【作用】SIGCHLD 信号处理器：子进程状态变化时内核回调
 * 【参数】sig：触发的信号编号（SIGCHLD=17）
 * 【信号安全要点（新手重点）】
 *   ① 只能调用异步信号安全函数（write/simple 信号安全，printf/malloc 不安全）
 *   ② 只能修改 volatile sig_atomic_t 变量
 *   ③ 不能加锁、不能 fork、不能做复杂逻辑
 *   这里只设一个标志位，真正的回收在 proc_monitor() 中做（安全且简单）。
 * 【设计文档对应】
 *   "捕获子进程崩溃信号，自动重启故障子进程" —— 信号捕获在此，重启在 monitor
 * -----------------------------------------------------------------------------
 */
static void sigchld_handler(int sig)
{
    /* (void)sig 显式忽略参数，消除未使用警告 */
    (void)sig;

    /* 只设标志，不做其他事。
     * 真正的 waitpid 和重启在 proc_monitor() 中执行。 */
    g_sigchld_flag = 1;
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：install_signal_handlers
 * -----------------------------------------------------------------------------
 * 【作用】安装信号处理器（父进程专用，fork 前调用，子进程继承）
 * 【信号说明】
 *   SIGCHLD：子进程退出/停止时内核发给父进程。用 sigaction 安装自定义 handler。
 *   SIGPIPE：向已关闭的管道写数据时触发。默认动作是杀死进程！必须忽略，
 *            改用 write 返回值的 errno=EPIPE 判断对端关闭。
 * 【sigaction 字段说明】
 *   sa_handler ：信号处理函数
 *   sa_flags   ：SA_RESTART=中断系统调用自动重启；SA_NOCLDSTOP=只在退出时发信号
 *   sa_mask    ：处理期间屏蔽的其他信号（这里不额外屏蔽）
 * 【易错点】
 *   SA_RESTART 对 select() 无效——select 被 SIGCHLD 中断必返回 EINTR。
 *   所以 proc_recv/send 中要处理 EINTR。
 * -----------------------------------------------------------------------------
 */
static void install_signal_handlers(void)
{
    struct sigaction sa;   /* 信号动作结构体 */

    /* ---- 安装 SIGCHLD 处理器 ---- */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    /* SA_RESTART：被信号中断的系统调用自动重启（read/write 等）
     * SA_NOCLDSTOP：子进程 stop/continue 不发 SIGCHLD，只在 exit 时发 */
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    /* 处理期间不额外屏蔽其他信号 */
    sigemptyset(&sa.sa_mask);
    /* 安装：SIGCHLD 到来时调用 sa.sa_handler */
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("[multi_proc] sigaction SIGCHLD failed");
    }

    /* ---- 忽略 SIGPIPE ----
     * 向已关闭的管道写会触发 SIGPIPE，默认杀死进程。
     * 忽略后 write 返回 -1，errno=EPIPE，我们据此判断对端关闭。
     * SIG_IGN 是内置的"忽略"处理器。 */
    signal(SIGPIPE, SIG_IGN);
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：create_pipe_pair
 * -----------------------------------------------------------------------------
 * 【作用】创建一对非阻塞匿名管道（父→子 + 子→父）
 * 【参数】to_child[2]   ：输出，父→子管道（父写 to_child[1]，子读 to_child[0]）
 * 【参数】to_parent[2]  ：输出，子→父管道（子写 to_parent[1]，父读 to_parent[0]）
 * 【返回值】0=成功，-1=失败
 * 【Linux API 说明】
 *   pipe(fds[2])：创建匿名管道
 *     fds[0]：读端
 *     fds[1]：写端
 *   管道是内核缓冲区（Linux 默认 64KB），单方向，先进先出。
 * 【易错点】
 *   管道两端都要设非阻塞，否则 read/write 可能永久阻塞导致死锁。
 * -----------------------------------------------------------------------------
 */
static int create_pipe_pair(int to_child[2], int to_parent[2])
{
    /* 创建父→子管道 */
    if (pipe(to_child) < 0) {
        perror("[multi_proc] pipe to_child failed");
        return -1;
    }

    /* 创建子→父管道 */
    if (pipe(to_parent) < 0) {
        perror("[multi_proc] pipe to_parent failed");
        close(to_child[0]);
        close(to_child[1]);
        return -1;
    }

    /* 4 个 fd 全部设非阻塞 */
    set_fd_nonblock(to_child[0]);
    set_fd_nonblock(to_child[1]);
    set_fd_nonblock(to_parent[0]);
    set_fd_nonblock(to_parent[1]);

    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：child_setup_after_fork
 * -----------------------------------------------------------------------------
 * 【作用】子进程 fork 后的初始化：关闭无关管道，设置自己的 fd
 * 【参数】self_id：子进程自己的 proc_id
 * 【参数】to_child[2]  ：自己这条父→子管道
 * 【参数】to_parent[2] ：自己这条子→父管道
 * 【设计要点】
 *   fork 后子进程继承了父进程所有 fd 副本，必须关闭不需要的，否则：
 *   ① fd 泄漏  ② 其他子进程的管道端点不关闭会导致父进程 read 永不返回 EOF
 *   子进程只保留：
 *     to_child[0]  ：从父进程读数据 → 存入 g_read_fds[PROC_ID_UI]
 *     to_parent[1] ：向父进程写数据 → 存入 g_write_fds[PROC_ID_UI]
 *   关闭：
 *     to_child[1]  ：父进程的写端（子进程不需要）
 *     to_parent[0] ：父进程的读端（子进程不需要）
 * -----------------------------------------------------------------------------
 */
static void child_setup_after_fork(proc_id_t self_id,
                                   int to_child[2], int to_parent[2])
{
    int i;   /* 循环变量 */

    /* 设置自己的身份 */
    g_self_id = self_id;
    /* 自己的 PID */
    g_pids[self_id] = getpid();
    /* 父进程的 PID（getppid 获取） */
    g_pids[PROC_ID_UI] = getppid();

    /* 清空所有 fd（后面只设置自己需要的） */
    for (i = 0; i < PROC_ID_MAX; i++) {
        g_write_fds[i] = -1;
        g_read_fds[i] = -1;
    }

    /* 设置自己的通信 fd：
     * g_read_fds[PROC_ID_UI]  = 从父进程读的 fd
     * g_write_fds[PROC_ID_UI] = 向父进程写的 fd
     * 这样子进程调用 proc_recv(PROC_ID_UI,...) 和 proc_send(PROC_ID_UI,...) 就能通信 */
    g_read_fds[PROC_ID_UI]  = to_child[0];    /* 父→子管道的读端 */
    g_write_fds[PROC_ID_UI] = to_parent[1];   /* 子→父管道的写端 */

    /* 关闭自己不需要的管道端点 */
    close(to_child[1]);     /* 父→子管道的写端（父进程用，子进程关闭） */
    close(to_parent[0]);    /* 子→父管道的读端（父进程用，子进程关闭） */

    /* 【关键】设置 g_initialized = 1
     * proc_init 在 fork 循环结束后才设 g_initialized=1，
     * 但 fork 时子进程继承的是 0。若不在此设置，
     * 子进程的 proc_send/proc_recv 会因 !g_initialized 返回 -1，
     * 导致子进程收不到任务、发不出结果。 */
    g_initialized = 1;

    /* 子进程恢复 SIGCHLD 默认动作（子进程不关心自己的子进程） */
    signal(SIGCHLD, SIG_DFL);
    /* SIGPIPE 继续忽略（子进程也可能写已关闭的管道） */
    signal(SIGPIPE, SIG_IGN);
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：parent_setup_after_fork
 * -----------------------------------------------------------------------------
 * 【作用】父进程 fork 子进程后的设置：保存子 PID，记录管道 fd，关闭子端
 * 【参数】child_id：刚 fork 的子进程 proc_id
 * 【参数】child_pid：子进程 PID
 * 【参数】to_child[2]  ：父→子管道
 * 【参数】to_parent[2] ：子→父管道
 * -----------------------------------------------------------------------------
 */
static void parent_setup_after_fork(proc_id_t child_id, pid_t child_pid,
                                    int to_child[2], int to_parent[2])
{
    /* 保存子进程 PID */
    g_pids[child_id] = child_pid;

    /* 父进程保留：
     *   to_child[1]  ：向该子进程写数据 → g_write_fds[child_id]
     *   to_parent[0] ：从该子进程读数据 → g_read_fds[child_id] */
    g_write_fds[child_id] = to_child[1];
    g_read_fds[child_id]  = to_parent[0];

    /* 关闭子进程端的 fd（父进程不需要） */
    close(to_child[0]);    /* 父→子管道的读端（子进程用） */
    close(to_parent[1]);   /* 子→父管道的写端（子进程用） */
}


/*
 * -----------------------------------------------------------------------------
 * 函数名：restart_child
 * -----------------------------------------------------------------------------
 * 【作用】重启崩溃的子进程（仅父进程调用，在 proc_monitor 中调用）
 * 【参数】child_id：要重启的子进程 proc_id
 * 【返回值】0=父进程视角成功，子进程视角调用回调不返回
 * 【重启流程】
 *   1. 关闭旧管道（子进程已死，旧管道可能 broken）
 *   2. 创建新管道
 *   3. fork 新子进程
 *   4. 子进程：child_setup_after_fork + 调用 g_child_handler
 *   5. 父进程：parent_setup_after_fork
 * 【设计文档对应】
 *   "捕获子进程崩溃信号，自动重启故障子进程"
 * -----------------------------------------------------------------------------
 */
static int restart_child(proc_id_t child_id)
{
    int to_child[2];      /* 新的父→子管道 */
    int to_parent[2];     /* 新的子→父管道 */
    pid_t pid;            /* fork 返回值 */

    /* 参数校验：只能重启子进程（LLM/OCR/ASR） */
    if (child_id < PROC_ID_LLM || child_id >= PROC_ID_MAX) {
        return -1;
    }

    /* 校验回调已注册 */
    if (g_child_handler == NULL) {
        printf("[multi_proc] 无法重启：子进程回调未注册\n");
        return -1;
    }

    /* ---- 1. 关闭旧管道 ---- */
    if (g_write_fds[child_id] >= 0) {
        close(g_write_fds[child_id]);
        g_write_fds[child_id] = -1;
    }
    if (g_read_fds[child_id] >= 0) {
        close(g_read_fds[child_id]);
        g_read_fds[child_id] = -1;
    }

    /* ---- 2. 创建新管道 ---- */
    if (create_pipe_pair(to_child, to_parent) < 0) {
        return -1;
    }

    /* ---- 3. fork ---- */
    pid = fork();
    if (pid < 0) {
        perror("[multi_proc] restart fork failed");
        close(to_child[0]); close(to_child[1]);
        close(to_parent[0]); close(to_parent[1]);
        return -1;
    }

    if (pid == 0) {
        /* ==== 子进程：设置 fd + 调用回调（不返回） ==== */
        child_setup_after_fork(child_id, to_child, to_parent);

        printf("[multi_proc] 子进程 %d 重启成功，PID=%d\n", child_id, getpid());

        /* 调用用户注册的子进程入口函数。
         * 该函数应包含工作循环并最终 exit()，不应 return。 */
        g_child_handler(child_id);

        /* 安全网：如果回调意外 return，退出子进程 */
        exit(0);
    }

    /* ==== 父进程：记录新子进程信息 ==== */
    parent_setup_after_fork(child_id, pid, to_child, to_parent);

    printf("[multi_proc] 父进程：子进程 %d 已重启，新 PID=%d\n", child_id, pid);
    return 0;
}


/* =============================================================================
 * 四、对外接口实现
 * =============================================================================
 */


/*
 * -----------------------------------------------------------------------------
 * 函数名：proc_set_child_handler
 * -----------------------------------------------------------------------------
 * 【作用】注册子进程入口回调函数
 * 【参数】fn：回调函数指针，签名 void fn(proc_id_t self_id)
 * 【说明】必须在 proc_init() 之前调用。子进程 fork 后会调用 fn(self_id)。
 *        fn 内部应包含工作循环并最终 exit()，不应 return。
 * -----------------------------------------------------------------------------
 */
void proc_set_child_handler(proc_child_fn fn)
{
    g_child_handler = fn;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 1：proc_init —— 初始化多进程架构
 * -----------------------------------------------------------------------------
 * 【返回值】PROC_ID_UI(0)=父进程，1~3=子进程（正常会在回调内 exit()，不返回），-1=错误
 * 【作用】
 *   1. 初始化全局数组
 *   2. 安装信号处理器（SIGCHLD/SIGPIPE）
 *   3. 为 3 个子进程各创建一对管道（父→子 + 子→父）
 *   4. 依次 fork 3 个子进程
 *   5. 子进程：设置 fd，调用注册的回调
 *   6. 父进程：记录子 PID 和管道 fd
 * 【设计文档对应】
 *   "proc_init()：fork4进程、创建双向匿名管道"
 * -----------------------------------------------------------------------------
 */
int proc_init(void)
{
    int to_child[PROC_ID_MAX][2];     /* 父→子管道，[0]=读端 [1]=写端 */
    int to_parent[PROC_ID_MAX][2];    /* 子→父管道 */
    int i;                            /* 循环变量 */

    /* 防止重复初始化 */
    if (g_initialized) {
        printf("[multi_proc] 已初始化，请勿重复调用\n");
        return -1;
    }

    /* 校验子进程回调已注册 */
    if (g_child_handler == NULL) {
        printf("[multi_proc] 错误：未注册子进程回调，请先调用 proc_set_child_handler()\n");
        return -1;
    }

    /* ---- 1. 初始化全局数组 ---- */
    for (i = 0; i < PROC_ID_MAX; i++) {
        g_pids[i] = 0;
        g_write_fds[i] = -1;
        g_read_fds[i] = -1;
        to_child[i][0] = to_child[i][1] = -1;
        to_parent[i][0] = to_parent[i][1] = -1;
    }

    /* ---- 2. 安装信号处理器（fork 前安装，子进程继承）---- */
    install_signal_handlers();

    /* ---- 3. 为每个子进程创建管道对 ---- */
    for (i = PROC_ID_LLM; i < PROC_ID_MAX; i++) {
        if (create_pipe_pair(to_child[i], to_parent[i]) < 0) {
            printf("[multi_proc] 创建管道 %d 失败\n", i);
            return -1;
        }
    }

    /* 父进程记录自己的 PID */
    g_pids[PROC_ID_UI] = getpid();
    g_self_id = PROC_ID_UI;

    /* ---- 4. 依次 fork 3 个子进程 ---- */
    for (i = PROC_ID_LLM; i < PROC_ID_MAX; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            /* fork 失败：清理已创建的资源 */
            perror("[multi_proc] fork failed");
            return -1;
        }

        if (pid == 0) {
            /* ==== 子进程分支 ====
             * fork 返回 0 表示当前是子进程。
             * 子进程需要：
             *   1. 关闭其他子进程的管道（防止 fd 泄漏和 EOF 问题）
             *   2. 设置自己的管道 fd
             *   3. 调用注册的回调函数（进入工作循环） */

            /* 关闭其他子进程的管道（重要！）
             * 如果不关闭，其他子进程管道的写端会有多个持有者，
             * 导致父进程 read 永远等不到 EOF。 */
            int j;
            for (j = PROC_ID_LLM; j < PROC_ID_MAX; j++) {
                if (j != i) {
                    close(to_child[j][0]); close(to_child[j][1]);
                    close(to_parent[j][0]); close(to_parent[j][1]);
                }
            }

            /* 设置自己的 fd */
            child_setup_after_fork((proc_id_t)i, to_child[i], to_parent[i]);

            printf("[multi_proc] 子进程 %d 启动，PID=%d\n", i, getpid());

            /* 调用回调函数（不返回） */
            g_child_handler((proc_id_t)i);

            /* 安全网：回调意外 return 则退出 */
            exit(0);
        }

        /* ==== 父进程分支 ====
         * fork 返回 >0 表示当前是父进程，返回值是子进程 PID。 */
        parent_setup_after_fork((proc_id_t)i, pid, to_child[i], to_parent[i]);
    }

    g_initialized = 1;
    printf("[multi_proc] 父进程初始化完成，PID=%d，子进程: LLM=%d OCR=%d ASR=%d\n",
           g_pids[PROC_ID_UI], g_pids[PROC_ID_LLM], g_pids[PROC_ID_OCR], g_pids[PROC_ID_ASR]);

    return PROC_ID_UI;   /* 父进程返回 0 */
}


/*
 * -----------------------------------------------------------------------------
 * 接口 2：proc_send —— 发送 TaskData（非阻塞 + 超时）
 * -----------------------------------------------------------------------------
 * 【参数】target：目标进程 ID
 * 【参数】data：待发送数据指针
 * 【返回值】>0=发送字节数，-1=错误，-2=超时
 * 【IPC 原理】
 *   write(fd, buf, len) 把 buf 的 len 字节写入管道内核缓冲区。
 *   非阻塞模式下：
 *     - 缓冲区有空间：立即返回写入字节数
 *     - 缓冲区满：返回 -1，errno=EAGAIN，需等待
 *   用 select 检查管道是否可写，避免 busy-loop。
 * 【死锁避免】
 *   select 设 1 秒超时，超时即放弃，不死等。
 *   若对端已关闭，write 返回 EPIPE（SIGPIPE 已忽略）。
 * -----------------------------------------------------------------------------
 */
int proc_send(proc_id_t target, const TaskData *data)
{
    int fd;               /* 写入的管道 fd */
    int ret;              /* 临时返回值 */
    fd_set wfds;          /* select 的可写 fd 集合 */
    struct timeval tv;    /* select 超时 */
    ssize_t written;      /* write 返回值 */

    /* 参数校验 */
    if (!g_initialized || target < 0 || target >= PROC_ID_MAX) {
        return -1;
    }
    if (data == NULL) {
        return -1;
    }

    fd = g_write_fds[target];
    if (fd < 0) {
        return -1;        /* 无连接 */
    }

    /* ---- 用 select 检查管道是否可写（1 秒超时）---- */
    FD_ZERO(&wfds);                    /* 清空集合 */
    FD_SET(fd, &wfds);                 /* 加入 fd */
    tv.tv_sec  = 1;                    /* 1 秒 */
    tv.tv_usec = 0;

    ret = select(fd + 1, NULL, &wfds, NULL, &tv);

    if (ret < 0) {
        /* select 出错。
         * EINTR=被信号中断（如 SIGCHLD），按超时处理。 */
        if (errno == EINTR) {
            return -2;   /* 视为超时 */
        }
        return -1;       /* 其他错误 */
    }
    if (ret == 0) {
        return -2;       /* 1 秒内管道满，超时 */
    }

    /* ---- 管道可写，写入整个 TaskData ---- */
    written = write(fd, data, sizeof(TaskData));

    if (written < 0) {
        /* EAGAIN=非阻塞管道满；EPIPE=对端关闭 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2;   /* 缓冲区满，视为超时 */
        }
        if (errno == EPIPE) {
            return -1;   /* 对端关闭 */
        }
        return -1;
    }

    /* 检查是否完整写入。
     * TaskData ≈ 4KB，管道缓冲区 64KB，正常情况一次写完。
     * 若部分写入（极少见），简化处理视为错误。 */
    if (written != (ssize_t)sizeof(TaskData)) {
        printf("[multi_proc] 警告：部分写入 %zd/%zu 字节\n", written, sizeof(TaskData));
        return -1;
    }

    return (int)written;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 3：proc_recv —— 接收 TaskData（非阻塞 + 超时）
 * -----------------------------------------------------------------------------
 * 【参数】source：来源进程 ID
 * 【参数】data：接收缓冲区
 * 【参数】timeout_ms：超时毫秒（0=立即返回不等待）
 * 【返回值】>0=接收字节数，-1=错误，-2=超时，-3=EOF(对端关闭)
 * 【IPC 原理】
 *   read(fd, buf, len) 从管道读 len 字节到 buf。
 *   非阻塞模式下：
 *     - 有数据：立即返回读取字节数
 *     - 无数据：返回 -1，errno=EAGAIN
 *     - 对端关闭且无数据：返回 0（EOF）
 *   用 select 检查管道是否可读，避免 busy-loop。
 * 【死锁避免】
 *   select 设 timeout_ms 超时，超时即返回 -2。
 *   EOF(返回0)表示对端关闭，返回 -3 让调用者处理。
 * -----------------------------------------------------------------------------
 */
int proc_recv(proc_id_t source, TaskData *data, int timeout_ms)
{
    int fd;               /* 读取的管道 fd */
    int ret;              /* 临时返回值 */
    fd_set rfds;          /* select 的可读 fd 集合 */
    struct timeval tv;    /* select 超时 */
    ssize_t got;          /* read 返回值 */

    /* 参数校验 */
    if (!g_initialized || source < 0 || source >= PROC_ID_MAX) {
        return -1;
    }
    if (data == NULL) {
        return -1;
    }

    fd = g_read_fds[source];
    if (fd < 0) {
        return -1;        /* 无连接 */
    }

    /* ---- 用 select 检查管道是否可读 ---- */
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    /* timeout_ms=0 时 tv={0,0}，select 立即返回（轮询模式） */
    ret = select(fd + 1, &rfds, NULL, NULL, &tv);

    if (ret < 0) {
        /* 被信号中断按超时处理 */
        if (errno == EINTR) {
            return -2;
        }
        return -1;
    }
    if (ret == 0) {
        return -2;       /* 超时无数据 */
    }

    /* ---- 管道可读，读取 TaskData ---- */
    got = read(fd, data, sizeof(TaskData));

    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2;   /* 无数据（select 可能有虚假唤醒） */
        }
        return -1;       /* 其他错误 */
    }
    if (got == 0) {
        return -3;       /* EOF：对端关闭了写端 */
    }

    /* 检查是否完整读取 */
    if (got != (ssize_t)sizeof(TaskData)) {
        printf("[multi_proc] 警告：部分读取 %zd/%zu 字节\n", got, sizeof(TaskData));
        return -1;
    }

    return (int)got;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 4：proc_monitor —— 检测崩溃并自动重启（仅父进程）
 * -----------------------------------------------------------------------------
 * 【返回值】>=0=本次重启的子进程数，-1=错误
 * 【作用】
 *   1. 检查 g_sigchld_flag，若被 SIGCHLD 置位则处理
 *   2. 用 waitpid(-1, &status, WNOHANG) 回收所有已退出的子进程（非阻塞）
 *   3. 若 WIFSIGNALED(status) 为真（被信号杀死），调用 restart_child 重启
 *   4. 若 WIFEXITED(status) 为真（正常退出），不重启
 * 【Linux API 说明】
 *   waitpid(-1, &status, WNOHANG)：
 *     -1    ：等待任意子进程
 *     status：填充子进程退出状态
 *     WNOHANG：无子进程退出时立即返回 0（非阻塞）
 *   WIFSIGNALED(status)：真=被信号杀死（崩溃）
 *   WTERMSIG(status)   ：导致死亡的信号编号
 *   WIFEXITED(status)  ：真=正常 exit
 *   WEXITSTATUS(status)：exit 码
 * 【僵尸进程回收】
 *   子进程退出后内核保留 task_struct 供父进程 wait，这期间是"僵尸"。
 *   不 wait 会一直占 PID 资源。本接口用 WNOHANG 循环回收所有僵尸。
 * -----------------------------------------------------------------------------
 */
int proc_monitor(void)
{
    int restarted = 0;   /* 本次重启计数 */
    pid_t pid;           /* waitpid 返回的子进程 PID */
    int status;          /* 子进程退出状态 */

    if (!g_initialized) {
        return -1;
    }
    /* 只有父进程才需要 monitor */
    if (g_self_id != PROC_ID_UI) {
        return -1;
    }
    /* 无 SIGCHLD 则无需处理 */
    if (!g_sigchld_flag) {
        return 0;
    }

    /* 清除标志，处理期间若又有 SIGCHLD 会重新置位 */
    g_sigchld_flag = 0;

    /* 循环回收所有可回收的子进程（WNOHANG 非阻塞） */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int i;   /* 循环变量 */

        /* 查找是哪个子进程退出了 */
        for (i = PROC_ID_LLM; i < PROC_ID_MAX; i++) {
            if (g_pids[i] == pid) {
                /* 找到退出的子进程 */
                g_pids[i] = 0;

                if (WIFSIGNALED(status)) {
                    /* 被信号杀死 = 崩溃，需要重启 */
                    int sig = WTERMSIG(status);
                    printf("[multi_proc] 子进程 %d (PID=%d) 崩溃，信号=%d，正在重启...\n",
                           i, pid, sig);
                    restart_child((proc_id_t)i);
                    restarted++;
                } else if (WIFEXITED(status)) {
                    /* 正常 exit，不重启 */
                    int code = WEXITSTATUS(status);
                    printf("[multi_proc] 子进程 %d (PID=%d) 正常退出，code=%d\n",
                           i, pid, code);
                }

                break;   /* 找到就跳出查找循环 */
            }
        }
    }

    return restarted;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 5：proc_cleanup —— 清理所有资源
 * -----------------------------------------------------------------------------
 * 【返回值】0=成功，-1=错误
 * 【作用】
 *   父进程：kill 所有子进程(SIGTERM)，waitpid 等待退出，关闭所有管道
 *   子进程：仅关闭自己的管道
 * 【设计要点】
 *   先 SIGTERM 让子进程优雅退出，再 wait 等待。
 *   不用 SIGKILL（无法清理资源）。
 * -----------------------------------------------------------------------------
 */
int proc_cleanup(void)
{
    int i;   /* 循环变量 */

    if (!g_initialized) {
        return -1;
    }

    if (g_self_id == PROC_ID_UI) {
        /* ==== 父进程：杀子 + 等待 + 关管道 ==== */

        /* 1. 向所有存活子进程发 SIGTERM */
        for (i = PROC_ID_LLM; i < PROC_ID_MAX; i++) {
            if (g_pids[i] > 0) {
                kill(g_pids[i], SIGTERM);
            }
        }

        /* 2. 等待所有子进程退出（阻塞 wait，最多等几秒） */
        for (i = PROC_ID_LLM; i < PROC_ID_MAX; i++) {
            if (g_pids[i] > 0) {
                int status;
                /* 用阻塞 waitpid 等待该子进程退出 */
                waitpid(g_pids[i], &status, 0);
                g_pids[i] = 0;
            }
        }

        /* 3. 关闭所有管道 fd */
        for (i = 0; i < PROC_ID_MAX; i++) {
            if (g_write_fds[i] >= 0) {
                close(g_write_fds[i]);
                g_write_fds[i] = -1;
            }
            if (g_read_fds[i] >= 0) {
                close(g_read_fds[i]);
                g_read_fds[i] = -1;
            }
        }

        printf("[multi_proc] 父进程清理完成\n");
    } else {
        /* ==== 子进程：仅关闭自己的管道 ==== */
        for (i = 0; i < PROC_ID_MAX; i++) {
            if (g_write_fds[i] >= 0) {
                close(g_write_fds[i]);
                g_write_fds[i] = -1;
            }
            if (g_read_fds[i] >= 0) {
                close(g_read_fds[i]);
                g_read_fds[i] = -1;
            }
        }
    }

    g_initialized = 0;
    return 0;
}


/*
 * -----------------------------------------------------------------------------
 * 接口 6：proc_self_id —— 获取当前进程身份
 * -----------------------------------------------------------------------------
 * 【返回值】PROC_ID_UI~PROC_ID_ASR
 * 【作用】让调用者知道自己在哪个进程，据此执行不同逻辑。
 * -----------------------------------------------------------------------------
 */
proc_id_t proc_self_id(void)
{
    return g_self_id;
}
