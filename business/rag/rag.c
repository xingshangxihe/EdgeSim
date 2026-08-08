/*
 * =============================================================================
 * EdgeSim 本地 RAG 知识库检索模块实现  rag.c
 * =============================================================================
 * 【实现思路】
 *   1. 分词：把用户查询切分为若干"关键词"
 *        - 英文/数字：按空格与标点切分（如 "what is edge computing" → 4 词）
 *        - 中文：相邻两字合并为一个"二元组"，如 "边缘计算" → "边缘/缘计/计算"
 *     2. 打分：对 kb_files 表里的每个 TXT/MD 文件，统计每个 token
 *        在文件全文中的出现次数并累加，得到文件得分（越高越相关）
 *     3. 选 Top N：按得分从高到低选取前 top_n 个文件
 *     4. 提片段：在命中文件里定位首个命中位置，截取前后文作为片段
 *     5. 拼接：把各片段（含路径标注）拼入输出缓冲区
 *
 * 【局限说明】（教学/演示用途可接受）
 *   - 不使用词法还原（stemming），"edge/edges" 视为不同词
 *   - 不处理同义词；中文用二元组近似分词（不是真正的分词器）
 *   - 不做向量化/语义检索；纯关键词命中
 *   这些简化让代码量小、零依赖，符合"不引入新技术栈"约束；
 *   后续可无缝替换为更复杂的评分函数而不改接口。
 * =============================================================================
 */

#include "rag.h"
#include "sqlite_db.h"     /* kb_query：遍历知识库文件表 */

#include <stdio.h>
#include <stdlib.h>        /* qsort 排序用 */
#include <string.h>
#include <strings.h>       /* strcasestr：大小写不敏感子串查找（Linux GNU） */
#include <ctype.h>         /* isalnum：ASCII 字母数字判断 */
#include <sys/stat.h>      /* stat：获取文件修改时间（缓存失效判断用） */

/* 单个 token 的最大长度 */
#define RAG_TOKEN_MAX    64
/* 一次最多支持的 token 数 */
#define RAG_TOKEN_MAX_N  32
/* 每个片段截取的上下文窗口长度（命中位置前后各多少字符） */
#define RAG_SNIPPET_WIN  200

/* 文件候选：文件名 + 内容 + 得分 */
typedef struct {
    char   path[KB_PATH_MAX];            /* 文件路径（来自 kb_files 表） */
    char   content[RAG_FILE_CONTENT_MAX];/* 文件全文内容 */
    int    score;                        /* 命中得分（token 出现总次数） */
} rag_file_candidate_t;


/* ---- 知识库内容缓存（阶段 4.5 性能优化）----
 * 背景：rag_retrieve 每次提问都重新 fopen/fread 每个文件全文并扫描，
 *       无任何缓存。当知识库文件多/大时，发送消息的瞬间（UI 线程
 *       同步调用）会明显卡顿。
 * 方案：用 path + mtime（修改时间）判断文件是否变化：
 *   - 文件未变化 → 直接复用内存中的缓存内容，省掉磁盘 IO
 *   - 文件新增/修改 → 重新读取并更新缓存
 * 内存占用：最多 RAG_MAX_FILES(64) 个文件 × RAG_FILE_CONTENT_MAX(64KB)
 *           ≈ 4MB，可接受。 */
typedef struct {
    char  path[KB_PATH_MAX];                 /* 文件路径（匹配键） */
    long  mtime;                             /* 文件修改时间（失效键） */
    char  content[RAG_FILE_CONTENT_MAX];     /* 缓存的文件内容 */
    int   valid;                             /* 1=缓存槽位有效 */
} rag_content_cache_t;

/* 全局内容缓存（静态，进程生命周期内常驻） */
static rag_content_cache_t g_rag_cache[RAG_MAX_FILES];


/* -----------------------------------------------------------------------------
 * is_cjk_start —— 判断 UTF-8 字节流当前位置是否是一个 CJK 汉字开头
 * -----------------------------------------------------------------------------
 * 【参数】s：指向 UTF-8 字节序列的指针
 * 【返回值】1=是 CJK 统一表意文字（U+4E00~U+9FFF），0=不是
 * 【算法】CJK 基本区汉字 UTF-8 编码为 3 字节：首字节范围 0xE4~0xE9。
 *        用首字节快速判定即可（无需校验后续字节）。
 * -----------------------------------------------------------------------------
 */
static int is_cjk_start(const unsigned char *s)
{
    unsigned char b = s[0];
    return (b >= 0xE4 && b <= 0xE9);
}


/* -----------------------------------------------------------------------------
 * extract_hanzi —— 从 UTF-8 字节流提取一个汉字（3 字节）到 out
 * -----------------------------------------------------------------------------
 * 【参数】s  ：指向汉字首字节
 * 【参数】out：输出缓冲区（至少 4 字节：3 字节汉字 + '\0'）
 * 【返回值】消耗的字节数（3）
 * -----------------------------------------------------------------------------
 */
static int extract_hanzi(const unsigned char *s, char *out)
{
    out[0] = (char)s[0];
    out[1] = (char)s[1];
    out[2] = (char)s[2];
    out[3] = '\0';
    return 3;
}


/* -----------------------------------------------------------------------------
 * tokenize —— 轻量分词：把查询切分为关键词数组（支持中英文）
 * -----------------------------------------------------------------------------
 * 【参数】query   ：用户查询文本（UTF-8）
 * 【参数】tokens  ：输出数组，每个元素存一个关键词（以 '\0' 结尾）
 * 【参数】max_tokens：tokens 数组容量
 * 【返回值】切出的 token 个数（0=查询为空）
 * 【算法】
 *   第一遍：把查询切为"单元"数组——
 *     - 英文/数字：连续字母数字组成一个单元（一个单词）
 *     - 中文：每个汉字单独一个单元
 *     - 空格/标点：分隔符，跳过
 *   第二遍：把单元转成 token——
 *     - 英文单词：直接作为 token（如 "edge"）
 *     - 中文：每 2 个连续汉字合并为一个"二元组 bigram"（滑动窗口），
 *       如 "边缘计算" → ["边缘","缘计","计算"]。
 *       bigram 让中文短语（如"边缘"、"计算"）都能独立命中文档，
 *       解决旧版"整段中文当 1 个 token、必须整串匹配"导致的检索失败。
 * 【注意】本函数不修改调用者字符串（不用 strtok）。
 * -----------------------------------------------------------------------------
 */
static int tokenize(const char *query, char tokens[][RAG_TOKEN_MAX], int max_tokens)
{
    const unsigned char *p = (const unsigned char *)query; /* 扫描指针 */
    char  units[64][RAG_TOKEN_MAX];   /* 第一遍的单元数组（单词/单字） */
    int   n_units = 0;                /* 单元个数 */
    char  cur[RAG_TOKEN_MAX];         /* 正在累积的英文单词缓冲 */
    int   clen = 0;                   /* cur 当前长度 */
    int   n = 0;                      /* 输出 token 个数 */
    int   i;                          /* 循环索引 */

    if (query == NULL || *p == '\0') {
        return 0;
    }

    /* ---- 第一遍：切分为单元（英文整词 / 中文单字） ---- */
    while (*p != '\0') {
        if (is_cjk_start(p)) {
            /* 中文汉字：先冲刷当前英文单词 */
            if (clen > 0) {
                cur[clen] = '\0';
                if (n_units < 64) {
                    strncpy(units[n_units++], cur, RAG_TOKEN_MAX - 1);
                }
                clen = 0;
            }
            /* 每个汉字作为独立单元 */
            if (n_units < 64) {
                extract_hanzi(p, units[n_units++]);
            }
            p += 3;                          /* UTF-8 汉字占 3 字节 */
        } else if (isalnum(*p)) {
            /* 英文/数字：累积进当前单词 */
            if (clen < RAG_TOKEN_MAX - 1) {
                cur[clen++] = (char)*p;
            }
            p++;
        } else {
            /* 空格/标点/其他：分隔符，冲刷当前单词 */
            if (clen > 0) {
                cur[clen] = '\0';
                if (n_units < 64) {
                    strncpy(units[n_units++], cur, RAG_TOKEN_MAX - 1);
                }
                clen = 0;
            }
            p++;
        }
    }
    /* 收尾：冲刷最后一个英文单词 */
    if (clen > 0) {
        cur[clen] = '\0';
        if (n_units < 64) {
            strncpy(units[n_units++], cur, RAG_TOKEN_MAX - 1);
        }
    }

    /* ---- 第二遍：单元 → token（中文相邻两字合并为 bigram） ---- */
    for (i = 0; i < n_units && n < max_tokens; i++) {
        if (is_cjk_start((const unsigned char *)units[i])) {
            /* 中文单元：若下一单元也是汉字，合并为 bigram（消费两个字） */
            if (i + 1 < n_units &&
                is_cjk_start((const unsigned char *)units[i + 1])) {
                snprintf(tokens[n], RAG_TOKEN_MAX, "%s%s",
                         units[i], units[i + 1]);
                n++;
                i++;                            /* 跳过已消费的下一字 */
            } else {
                /* 孤立的单个汉字（前后不连中文）：单独作为 token */
                strncpy(tokens[n], units[i], RAG_TOKEN_MAX - 1);
                tokens[n][RAG_TOKEN_MAX - 1] = '\0';
                n++;
            }
        } else {
            /* 英文单词：直接作为 token */
            strncpy(tokens[n], units[i], RAG_TOKEN_MAX - 1);
            tokens[n][RAG_TOKEN_MAX - 1] = '\0';
            n++;
        }
    }
    return n;
}


/* -----------------------------------------------------------------------------
 * read_file_text —— 读取文本文件内容（限长）
 * -----------------------------------------------------------------------------
 * 【参数】path ：文件路径
 * 【参数】buf  ：输出缓冲区
 * 【参数】cap  ：缓冲区容量（字节）
 * 【返回值】读取的字节数；-1=失败（无法打开/读取）
 * 【说明】用 fopen/fread 读全文，末尾补 '\0'。
 *        文件超过 cap-1 字节时只读前 cap-1 字节（防大文件撑爆内存）。
 * -----------------------------------------------------------------------------
 */
static long read_file_text(const char *path, char *buf, size_t cap)
{
    FILE *fp;
    long  n;

    if (path == NULL || buf == NULL || cap < 2) {
        return -1;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;                   /* 文件不存在或不可读 */
    }

    n = (long)fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[n] = '\0';                   /* 确保是合法 C 字符串 */
    return n;
}


/* -----------------------------------------------------------------------------
 * rag_cached_read —— 带缓存的文件读取（阶段 4.5 性能优化新增）
 * -----------------------------------------------------------------------------
 * 【参数】path    ：文件路径
 * 【参数】out     ：输出缓冲区（文件内容）
 * 【参数】out_size：输出缓冲区容量
 * 【返回值】内容字节数（>=0）；-1=失败（文件不存在/不可读）
 * 【算法】
 *   1. stat 获取文件修改时间 mtime
 *   2. 遍历缓存：path 相同 且 mtime 相同 → 直接复制缓存内容返回
 *      （文件未变化，省掉 fopen/fread 磁盘 IO）
 *   3. 未命中 → read_file_text 读文件；成功后更新缓存：
 *      a. 优先覆盖同 path 旧槽位
 *      b. 否则找空槽位
 *      c. 全满则覆盖 0 号（简单 FIFO 兜底）
 * 【设计说明】
 *   本函数把"每次提问重读全部知识库文件"降为"仅在文件变化时读一次"，
 *   显著降低 rag_retrieve 在发送消息时的同步阻塞时长（用户实测
 *   "点击按钮就卡"的主要嫌疑即每次发送时的全量文件 IO）。
 * -----------------------------------------------------------------------------
 */
static long rag_cached_read(const char *path, char *out, size_t out_size)
{
    struct stat st;              /* stat 结果：取 st_mtime 判断文件是否变化 */
    long  mtime = 0;             /* 文件修改时间（秒） */
    int   i;                     /* 缓存遍历索引 */
    int   slot = -1;             /* 待写入的缓存槽位（-1=未定） */

    if (path == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    /* 1. 获取修改时间（stat 失败视为 mtime=0，缓存仍可工作但不失效） */
    if (stat(path, &st) == 0) {
        mtime = (long)st.st_mtime;
    }

    /* 2. 查缓存：path 相同 + mtime 相同 → 命中，直接复用 */
    for (i = 0; i < RAG_MAX_FILES; i++) {
        if (g_rag_cache[i].valid &&
            strcmp(g_rag_cache[i].path, path) == 0 &&
            g_rag_cache[i].mtime == mtime) {
            strncpy(out, g_rag_cache[i].content, out_size - 1);
            out[out_size - 1] = '\0';
            return (long)strlen(out);
        }
    }

    /* 3. 未命中：读文件（失败直接返回，不入缓存） */
    if (read_file_text(path, out, out_size) < 0) {
        return -1;
    }

    /* 4. 更新缓存 */
    /* 4a. 优先覆盖同 path 的旧槽位（文件内容更新场景） */
    for (i = 0; i < RAG_MAX_FILES; i++) {
        if (g_rag_cache[i].valid &&
            strcmp(g_rag_cache[i].path, path) == 0) {
            slot = i;
            break;
        }
    }
    /* 4b. 否则找空槽位 */
    if (slot < 0) {
        for (i = 0; i < RAG_MAX_FILES; i++) {
            if (!g_rag_cache[i].valid) {
                slot = i;
                break;
            }
        }
    }
    /* 4c. 全满：覆盖 0 号（简单策略，功能不受影响） */
    if (slot < 0) {
        slot = 0;
    }

    /* 写入缓存槽位（限长复制，防止越界） */
    strncpy(g_rag_cache[slot].path, path,
            sizeof(g_rag_cache[slot].path) - 1);
    g_rag_cache[slot].path[sizeof(g_rag_cache[slot].path) - 1] = '\0';
    g_rag_cache[slot].mtime = mtime;
    strncpy(g_rag_cache[slot].content, out,
            sizeof(g_rag_cache[slot].content) - 1);
    g_rag_cache[slot].content[sizeof(g_rag_cache[slot].content) - 1] = '\0';
    g_rag_cache[slot].valid = 1;

    return (long)strlen(out);
}


/* -----------------------------------------------------------------------------
 * count_occurrences —— 统计某 token 在文本中的出现次数
 * -----------------------------------------------------------------------------
 * 【参数】haystack：被搜索文本
 * 【参数】needle  ：关键词
 * 【返回值】出现次数（>=0）
 * 【算法】用 strcasestr 循环查找（大小写不敏感），
 *        每找到一次前进一个字符（允许重叠匹配，对中文子串友好）。
 * -----------------------------------------------------------------------------
 */
static int count_occurrences(const char *haystack, const char *needle)
{
    int       count = 0;
    const char *p   = haystack;

    if (needle == NULL || needle[0] == '\0') {
        return 0;
    }

    while ((p = strcasestr(p, needle)) != NULL) {
        count++;
        p++;                        /* 前进 1 字符，允许重叠匹配 */
    }
    return count;
}


/* -----------------------------------------------------------------------------
 * file_score —— 计算一个文件与查询的命中得分
 * -----------------------------------------------------------------------------
 * 【参数】cand ：文件候选（其 content 已填好）
 * 【参数】tokens：分词结果数组
 * 【参数】n_tokens：token 个数
 * 【返回值】得分（>=0）。得分 = 所有 token 在文件中的出现次数之和。
 * 【说明】得分存回 cand->score。
 * -----------------------------------------------------------------------------
 */
static void file_score(rag_file_candidate_t *cand,
                       char tokens[][RAG_TOKEN_MAX], int n_tokens)
{
    int i;
    int total = 0;

    for (i = 0; i < n_tokens; i++) {
        total += count_occurrences(cand->content, tokens[i]);
    }
    cand->score = total;
}


/* -----------------------------------------------------------------------------
 * extract_snippet —— 从文件内容提取包含命中的片段
 * -----------------------------------------------------------------------------
 * 【参数】cand：文件候选
 * 【参数】tokens/n_tokens：分词结果（用于定位首个命中）
 * 【参数】out/out_size：输出缓冲区
 * 【说明】找到第一个 token 出现的位置，截取 [pos-win, pos+win] 前后文，
 *        若命中位置靠头/靠尾则自动收窄到内容边界。
 * -----------------------------------------------------------------------------
 */
static void extract_snippet(const rag_file_candidate_t *cand,
                            char tokens[][RAG_TOKEN_MAX], int n_tokens,
                            char *out, size_t out_size)
{
    const char *hit = NULL;          /* 首个命中位置指针 */
    long        hit_off;             /* 命中相对文件开头的偏移 */
    long        start, end;          /* 片段起止位置 */
    long        len;

    int i;

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    /* 1. 找首个命中位置（按 token 顺序取最早出现者） */
    for (i = 0; i < n_tokens; i++) {
        hit = strcasestr(cand->content, tokens[i]);
        if (hit != NULL) {
            break;                   /* 找到第一个命中的 token 即可 */
        }
    }
    if (hit == NULL) {
        /* 无命中（理论上不会，因为 score>0 才进此函数），
         * 退化为取文件开头 */
        hit = cand->content;
    }

    /* 2. 计算片段边界（前后各 RAG_SNIPPET_WIN 字符，钳位到内容边界） */
    hit_off = (long)(hit - cand->content);
    start   = hit_off - RAG_SNIPPET_WIN;
    if (start < 0) start = 0;
    end     = hit_off + RAG_SNIPPET_WIN;
    if (end > (long)strlen(cand->content)) end = (long)strlen(cand->content);

    /* 3. 复制片段 */
    len = end - start;
    if (len > (long)out_size - 1) {
        len = (long)out_size - 1;
    }
    memcpy(out, cand->content + start, (size_t)len);
    out[len] = '\0';
}


/*
 * -----------------------------------------------------------------------------
 * 公共接口：rag_retrieve
 * -----------------------------------------------------------------------------
 * 【实现步骤】见文件头注释。核心为：
 *   分词 → 遍历 kb_files 打分 → 选择 Top N → 提取片段 → 拼接输出。
 * -----------------------------------------------------------------------------
 */
int rag_retrieve(const char *query, char *out, size_t out_size, int top_n)
{
    kb_file_t             files[RAG_MAX_FILES];   /* kb_files 表行 */
    int                   n_files = 0;            /* 知识库文件数 */
    rag_file_candidate_t *cands = NULL;            /* 打分候选数组 */
    char                  tokens[RAG_TOKEN_MAX_N][RAG_TOKEN_MAX]; /* 分词 */
    int                   n_tokens;               /* token 个数 */
    int                   used = 0;               /* 已用候选数 */
    int                   i, j;                   /* 循环索引 */
    int                   best_idx;               /* 当前得分最高下标 */
    char                  snippet[RAG_FILE_CONTENT_MAX]; /* 片段缓冲 */
    size_t                room;                   /* 输出剩余空间 */

    /* ---- 0. 参数校验 ---- */
    if (query == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    out[0] = '\0';                                 /* 先置空输出 */
    if (top_n <= 0) {
        top_n = RAG_DEFAULT_TOP_N;                 /* 用默认值 */
    }
    if (top_n > 4) {
        top_n = 4;                                 /* 上限 4 个片段 */
    }

    /* ---- 1. 分词 ---- */
    n_tokens = tokenize(query, tokens, RAG_TOKEN_MAX_N);
    if (n_tokens <= 0) {
        return 0;                                  /* 查询为空，无检索意义 */
    }

    /* ---- 2. 遍历 kb_files 表，读取文本文件并打分 ---- */
    if (kb_query(files, RAG_MAX_FILES, &n_files) != 0 || n_files <= 0) {
        return 0;                                  /* 知识库为空 */
    }

    cands = (rag_file_candidate_t *)calloc((size_t)n_files,
                                           sizeof(rag_file_candidate_t));
    if (cands == NULL) {
        return -1;                                 /* 内存分配失败 */
    }

    for (i = 0; i < n_files; i++) {
        /* 只处理 TXT(0)/MD(1)；PDF(2)/图片(3) 无法直接读文本，跳过 */
        if (files[i].type > 1) {
            continue;
        }

        /* 读文件全文（带缓存：path+mtime 未变化时复用内存内容，
         * 省掉每次提问的重复磁盘 IO；失败则跳过该文件） */
        if (rag_cached_read(files[i].path, cands[used].content,
                            sizeof(cands[used].content)) < 0) {
            continue;
        }
        strncpy(cands[used].path, files[i].path, sizeof(cands[used].path) - 1);
        cands[used].path[sizeof(cands[used].path) - 1] = '\0';

        /* 打分 */
        file_score(&cands[used], tokens, n_tokens);
        if (cands[used].score > 0) {
            used++;                                /* 只保留有命中的文件 */
        }
    }

    /* ---- 3. 从候选中选 Top N（简单选择排序，N 很小） ---- */
    for (j = 0; j < top_n && j < used; j++) {
        /* 找当前未输出候选里得分最高的 */
        best_idx = -1;
        for (i = j; i < used; i++) {
            if (best_idx < 0 || cands[i].score > cands[best_idx].score) {
                best_idx = i;
            }
        }
        if (best_idx < 0) {
            break;
        }

        /* 把 best 换到位置 j，之后不再处理它 */
        if (best_idx != j) {
            rag_file_candidate_t tmp = cands[j];
            cands[j]    = cands[best_idx];
            cands[best_idx] = tmp;
        }

        /* ---- 4. 提取片段并拼接 ---- */
        extract_snippet(&cands[j], tokens, n_tokens, snippet,
                        sizeof(snippet));
        room = out_size - strlen(out) - 1;
        if (room < 8) {
            break;                                 /* 输出缓冲快满 */
        }
        snprintf(out + strlen(out), room,
                 "[知识库: %s]\n%s\n\n", cands[j].path, snippet);
    }

    free(cands);
    return 0;
}
