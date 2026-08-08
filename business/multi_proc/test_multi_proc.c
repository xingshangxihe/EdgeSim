/*
 * =============================================================================
 * EdgeSim 多进程管道模块独立测试  test_multi_proc.c
 * =============================================================================
 * 【文件作用】
 *   独立测试 multi_proc 模块，验证：
 *     1. 4 进程 fork（1 父 + 3 子）
 *     2. 双向匿名管道通信（父发 PING，子回 PONG）
 *     3. 非阻塞 recv（0ms 超时立即返回）
 *     4. 超时 recv（100ms 超时等待后返回）
 *     5. 崩溃自动重启（发 CRASH 让子进程 SIGSEGV，proc_monitor 重启）
 *     6. 优雅退出（发 EXIT，子进程退出，父进程 cleanup）
 *
 * 【编译运行方式】
 *   在 business/multi_proc/ 目录下：
 *     make test          # 一键编译并运行
 *   或手动编译：
 *     gcc -Wall -g test_multi_proc.c multi_proc.c -o test_multi_proc
 *     ./test_multi_proc
 *
 * 【设计文档对应】
 *   EdgeSim_Design.md 第 3.2 节 + 第 7.2 条「编译自测」
 * =============================================================================
 */

#include <stdio.h>          /* printf */
#include <string.h>         /* memset/strlen */
#include <stdlib.h>         /* exit */
#include <unistd.h>         /* sleep */
#include <time.h>           /* time */
#include "multi_proc.h"     /* 本模块接口 */


/* =============================================================================
 * 子进程入口回调函数
 * =============================================================================
 * 【作用】子进程 fork 后调用此函数，进入工作循环。
 *         接收父进程发来的 TaskData，根据 cmd 处理并回复。
 * 【参数】self_id：自己的 proc_id（LLM/OCR/ASR）
 * 【说明】此函数不应 return，应通过 exit() 退出。
 * ========================================================================== */
static void child_worker(proc_id_t self_id)
{
    TaskData task;       /* 接收缓冲区 */
    TaskData resp;       /* 回复缓冲区 */
    int ret;             /* 接口返回值 */

    printf("[子进程 %d] 启动工作循环，PID=%d\n", self_id, getpid());

    /* ---- 工作循环：收任务 → 处理 → 回复 ---- */
    while (1) {
        /* 阻塞等待父进程消息，5 秒超时。
         * 超时则 continue 继续等；EOF(-3) 表示父进程关闭，退出。 */
        ret = proc_recv(PROC_ID_UI, &task, 5000);

        if (ret == -3) {
            /* EOF：父进程关闭了管道，退出 */
            printf("[子进程 %d] 父进程关闭连接，退出\n", self_id);
            break;
        }
        if (ret == -2) {
            /* 超时：继续等 */
            continue;
        }
        if (ret < 0) {
            /* 其他错误：继续等 */
            continue;
        }

        /* ---- 根据 cmd 处理 ---- */
        switch (task.cmd) {

        case TASK_CMD_PING:
            /* 心跳请求：回复 PONG */
            memset(&resp, 0, sizeof(resp));
            resp.cmd       = TASK_CMD_PONG;
            resp.model_id  = self_id;
            resp.timestamp = time(NULL);
            /* 拼接回复消息 */
            snprintf(resp.data_buf, TASK_DATA_MAX,
                     "PONG from proc %d (pid %d)", self_id, getpid());
            resp.data_len  = (int)strlen(resp.data_buf) + 1;

            /* 发送回复 */
            proc_send(PROC_ID_UI, &resp);
            printf("[子进程 %d] 收到 PING，已回复 PONG\n", self_id);
            break;

        case TASK_CMD_CRASH:
            /* 模拟崩溃：解引用空指针触发 SIGSEGV。
             * 父进程的 proc_monitor 会检测到并重启本子进程。 */
            printf("[子进程 %d] 收到 CRASH 指令，模拟崩溃...\n", self_id);
            {
                volatile int *null_ptr = NULL;
                *null_ptr = 42;   /* 必触发 SIGSEGV */
            }
            break;

        case TASK_CMD_EXIT:
            /* 退出指令：跳出循环 */
            printf("[子进程 %d] 收到 EXIT，退出\n", self_id);
            goto child_exit;

        default:
            /* 未知命令：忽略 */
            printf("[子进程 %d] 未知 cmd=%d，忽略\n", self_id, task.cmd);
            break;
        }
    }

child_exit:
    /* 子进程清理自己的管道 */
    proc_cleanup();
    exit(0);   /* 子进程退出 */
}


/* =============================================================================
 * 辅助函数：构造并发送 PING
 * =============================================================================
 * ========================================================================== */
static void send_ping(proc_id_t target)
{
    TaskData task;
    memset(&task, 0, sizeof(task));
    task.cmd       = TASK_CMD_PING;
    task.model_id  = target;
    task.timestamp = time(NULL);
    snprintf(task.data_buf, TASK_DATA_MAX, "PING to proc %d", target);
    task.data_len  = (int)strlen(task.data_buf) + 1;

    if (proc_send(target, &task) > 0) {
        printf("[父进程] 已向子进程 %d 发送 PING\n", target);
    } else {
        printf("[父进程] 向子进程 %d 发送 PING 失败\n", target);
    }
}


/* =============================================================================
 * 辅助函数：等待并接收 PONG
 * =============================================================================
 * ========================================================================== */
static int recv_pong(proc_id_t source, int timeout_ms)
{
    TaskData resp;
    int ret = proc_recv(source, &resp, timeout_ms);
    if (ret > 0 && resp.cmd == TASK_CMD_PONG) {
        printf("[父进程] 收到子进程 %d 回复：%s\n", source, resp.data_buf);
        return 0;
    }
    printf("[父进程] 等待子进程 %d 回复超时/失败 (ret=%d)\n", source, ret);
    return -1;
}


/* =============================================================================
 * 父进程测试主函数
 * =============================================================================
 * 测试流程：
 *   步骤 1：向 3 个子进程发 PING，收 PONG（验证双向通信）
 *   步骤 2：非阻塞 recv（0ms 超时，验证立即返回）
 *   步骤 3：超时 recv（100ms 超时，验证等待后返回）
 *   步骤 4：发 CRASH 给 LLM，等 1s，调 proc_monitor 重启
 *   步骤 5：再向 LLM 发 PING 验证重启成功
 *   步骤 6：发 EXIT 给所有子进程，cleanup
 * ========================================================================== */
static void parent_main(void)
{
    int i;       /* 循环变量 */
    int ret;     /* 接口返回值 */
    TaskData task;

    printf("\n============================================================\n");
    printf("  父进程测试开始，PID=%d\n", getpid());
    printf("============================================================\n\n");

    /* ---- 步骤 1：双向通信测试 ---- */
    printf("【步骤 1】双向管道通信测试（PING/PONG）\n");
    for (i = PROC_ID_LLM; i < PROC_ID_MAX; i++) {
        send_ping((proc_id_t)i);
    }
    /* 接收 3 个 PONG（3 秒超时） */
    for (i = PROC_ID_LLM; i < PROC_ID_MAX; i++) {
        recv_pong((proc_id_t)i, 3000);
    }
    printf("\n");

    /* ---- 步骤 2：非阻塞 recv 测试 ---- */
    printf("【步骤 2】非阻塞 recv 测试（0ms 超时）\n");
    printf("  预期：无数据时立即返回 -2（超时）\n");
    ret = proc_recv(PROC_ID_LLM, &task, 0);
    printf("  结果：ret=%d %s\n", ret,
           ret == -2 ? "✓ 超时返回（符合预期）" : "✗ 异常");
    printf("\n");

    /* ---- 步骤 3：超时 recv 测试 ---- */
    printf("【步骤 3】超时 recv 测试（100ms 等待）\n");
    printf("  预期：等待 100ms 后返回 -2（超时）\n");
    ret = proc_recv(PROC_ID_OCR, &task, 100);
    printf("  结果：ret=%d %s\n", ret,
           ret == -2 ? "✓ 超时返回（符合预期）" : "✗ 异常");
    printf("\n");

    /* ---- 步骤 4：崩溃自动重启测试 ---- */
    printf("【步骤 4】崩溃自动重启测试\n");
    printf("  向 LLM 子进程发 CRASH 指令，预期 SIGSEGV 后 proc_monitor 重启\n");

    /* 发 CRASH */
    memset(&task, 0, sizeof(task));
    task.cmd = TASK_CMD_CRASH;
    task.timestamp = time(NULL);
    proc_send(PROC_ID_LLM, &task);

    /* 等 1 秒让子进程崩溃 + SIGCHLD 到达 */
    printf("  等待 1 秒让子进程崩溃...\n");
    sleep(1);

    /* 调 proc_monitor 检测并重启 */
    printf("  调用 proc_monitor()...\n");
    ret = proc_monitor();
    printf("  proc_monitor 返回重启数=%d\n", ret);

    /* 等待重启的子进程就绪 */
    printf("  等待 1 秒让新子进程就绪...\n");
    sleep(1);
    printf("\n");

    /* ---- 步骤 5：验证重启成功 ---- */
    printf("【步骤 5】验证 LLM 子进程重启成功\n");
    send_ping(PROC_ID_LLM);
    recv_pong(PROC_ID_LLM, 3000);
    printf("\n");

    /* ---- 步骤 6：优雅退出 ---- */
    printf("【步骤 6】优雅退出测试\n");
    /* 向所有子进程发 EXIT */
    for (i = PROC_ID_LLM; i < PROC_ID_MAX; i++) {
        memset(&task, 0, sizeof(task));
        task.cmd = TASK_CMD_EXIT;
        proc_send((proc_id_t)i, &task);
    }
    /* 等 1 秒让子进程退出 */
    sleep(1);
    /* 处理残留的 SIGCHLD（子进程退出会触发） */
    proc_monitor();

    printf("\n============================================================\n");
    printf("  父进程测试完成\n");
    printf("============================================================\n");
}


/* =============================================================================
 * 主函数
 * =============================================================================
 * 流程：
 *   1. 注册子进程回调
 *   2. 调 proc_init() fork 4 进程
 *   3. 父进程返回 PROC_ID_UI → 执行 parent_main()
 *   4. 子进程返回 1~3 → 回调被调用 → 执行 child_worker()
 * ========================================================================== */
int main(void)
{
    int self_id;   /* proc_init 返回值 */

    printf("============================================================\n");
    printf("  EdgeSim multi_proc 模块独立测试\n");
    printf("============================================================\n");

    /* 1. 注册子进程入口回调 */
    proc_set_child_handler(child_worker);

    /* 2. 初始化多进程架构 */
    self_id = proc_init();
    if (self_id < 0) {
        printf("  proc_init 失败\n");
        return 1;
    }

    /* 3. 根据返回值分流 */
    if (self_id == PROC_ID_UI) {
        /* 父进程：执行测试主逻辑 */
        parent_main();
        /* 清理（子进程已在 EXIT 时自行退出，这里再 wait 一次确保回收） */
        proc_cleanup();
    } else {
        /* 子进程：回调已被 proc_init 内部调用，不会走到这里。
         * 但作为安全网，若回调 return 了，这里也退出。 */
        exit(0);
    }

    return 0;
}
