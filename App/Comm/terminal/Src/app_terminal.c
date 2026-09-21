/**
 * @file    app_terminal.c
 * @brief   USB CDC 终端实现（CherrySH 绑定：会话 / 输出缓冲 / 回调链）
 * @author  Kaiser
 *
 * 结构：
 *   CherrySH（SDK 中间件：chry_shell.c + builtin/help.c）
 *     ├─ sput → TX 环（cherryrb 2KB）→ 1kHz 非阻塞 flush → app_usb
 *     ├─ sget ← app_usb（驱动内部 RX 环，非阻塞）
 *     ├─ 补全：上下文候选（命令/子命令/参数名）+ 重复 TAB 循环选择
 *     └─ 用户回调链：Ctrl-C → job 中止 → 原回调
 *
 * 约束：
 *   - 运行在 1kHz 慢任务；不在 ISR 上下文执行；
 *   - 输出门控 DTR（主机未打开端口时丢弃，不累积陈旧输出）；
 *   - sput 恒返回 size（release 配置下 CherryRL 忽略返回值，不可依赖重试），
 *     环满时经有界 flush 后仍不足则丢弃并计数。
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal.h"

#include "app_debug_rtt.h"
#include "app_terminal_job.h"
#include "app_terminal_monitor.h"
#include "app_usb.h"
#include "chry_ringbuffer.h"
#include "csh.h"
#include "intf_clock.h"
#include "params_meta_generated.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* FSymTab/VSymTab 链接段边界（linkers/gcc/user_linker.ld 已 KEEP） */
extern const int __fsymtab_start;
extern const int __fsymtab_end;
extern const int __vsymtab_start;
extern const int __vsymtab_end;

#define APP_TERMINAL_TX_RING_SIZE       (8192U) /**< TX 环容量（2 的幂，cherryrb 要求；需容纳欢迎界面 ~4KB） */
/* 单次 USB 写上限：取 256（< HS MPS=512）→ 每次都是短包，避免"整 MPS 倍数需补 ZLP"
 * 那条链路（ZLP 完成回调依赖更强，历史上是 TX 卡死入口） */
#define APP_TERMINAL_TX_CHUNK_SIZE      (256U)  /**< 单次 USB 写上限（< HS MPS，免 ZLP） */
#define APP_TERMINAL_TX_FLUSH_CHUNKS    (4U)    /**< 单拍最多推送块数 */
#define APP_TERMINAL_RUN_BUDGET_US      (200U)  /**< 单次 run_once 输出 flush 总预算 [µs] */
#define APP_TERMINAL_LINE_SIZE          (256U)  /**< 命令行上限 */
#define APP_TERMINAL_HISTORY_SIZE       (256U)  /**< 历史缓冲（2 的幂） */
#define APP_TERMINAL_PROMPT_SIZE        (128U)  /**< 提示符缓冲 */

#define APP_TERMINAL_USER "HPM53M1" /**< 提示符用户名 */
#define APP_TERMINAL_HOST "G6618"   /**< 提示符主机名 */
#define APP_TERMINAL_PATH "/"       /**< 提示符路径（const path 模式） */

/*
 * 命令查找路径（CherrySH 环境变量 PATH，必需）：
 *   - "/bin"  = CSH_CMD_EXPORT_ALIAS 默认路径（工程命令）
 *   - "/sbin" = CSH_SCMD_EXPORT 默认路径（builtin help）
 * 缺失 PATH 时 terminal 无法解析任何命令（command not found）。
 */
static const char s_env_path[] = "/sbin:/bin";
CSH_RVAR_EXPORT(s_env_path, PATH, sizeof(s_env_path));

/** @brief 补全回调原型（与 chry_readline_t.cplt.acb 一致） */
typedef uint8_t (*app_terminal_completion_t)(chry_readline_t*, char*, uint16_t*, const char**, uint8_t*,
                                          uint8_t);

/** @brief 用户事件回调原型（与 chry_readline_t.ucb 一致） */
typedef int (*app_terminal_user_cb_t)(chry_readline_t*, uint8_t);

static chry_shell_t s_csh;                      /**< CherrySH 实例 */
static chry_ringbuffer_t s_tx_ring;               /**< TX 环（cherryrb） */
static uint8_t s_tx_pool[APP_TERMINAL_TX_RING_SIZE]; /**< TX 环存储 */
static uint32_t s_tx_drop_bytes;                  /**< 输出丢弃统计 [byte] */
static uint32_t s_line_feeds;                     /**< 输出流换行计数（monitor 布局判定） */
static bool s_ready;                              /**< 初始化完成 */
static bool s_last_dtr;                           /**< 上一拍 DTR（边沿检测） */
static app_terminal_completion_t s_original_completion; /**< 原补全回调（链式） */
static app_terminal_user_cb_t s_original_user_cb;       /**< 原用户事件回调（链式） */

static char s_line_buffer[APP_TERMINAL_LINE_SIZE];       /**< 命令行缓冲 */
static char s_history_buffer[APP_TERMINAL_HISTORY_SIZE]; /**< 历史缓冲 */
static char s_prompt_buffer[APP_TERMINAL_PROMPT_SIZE];   /**< 提示符缓冲 */

static uint32_t s_cycles_per_ms;     /**< 每毫秒 cycle 数（init 缓存） */
static uint32_t s_ms_last_cycle;     /**< 上次计时 cycle */
static uint32_t s_ms_remainder;      /**< 计时余数（避免截断漂移） */
static uint32_t s_now_ms;            /**< 毫秒计数（供 job 使用） */
static uint32_t s_flush_deadline;    /**< 本轮 run_once 的输出 flush 截止 cycle */
static bool s_flush_deadline_valid;  /**< 截止时间有效（run_once 期间） */

/* ============================================================================
 * TX：flush / sput
 * ============================================================================ */

/**
 * @brief 推送 TX 环数据到 USB（非阻塞，单拍有界）。
 */
static void app_terminal_tx_flush(void) {
    uint32_t chunks;

    for (chunks = 0U; chunks < APP_TERMINAL_TX_FLUSH_CHUNKS; chunks++) {
        uint32_t size = 0U;
        uint8_t* chunk = chry_ringbuffer_linear_read_setup(&s_tx_ring, &size);

        if ((chunk == NULL) || (size == 0U)) {
            break;
        }
        if (size > APP_TERMINAL_TX_CHUNK_SIZE) {
            size = APP_TERMINAL_TX_CHUNK_SIZE;
        }
        if (app_usb_write_timeout(chunk, (size_t)size, 0U) != 0) {
            break; /* 端点忙 / 未就绪：下拍重试 */
        }
        (void)chry_ringbuffer_linear_read_done(&s_tx_ring, size);
    }
}

/**
 * @brief 有界 flush：为 need_bytes 腾空间；受本轮 run_once 总预算约束，无进展立即放弃。
 * @param need_bytes 需要的空闲字节数
 */
static void app_terminal_tx_flush_bounded(uint32_t need_bytes) {
    uint32_t deadline;

    if (s_flush_deadline_valid) {
        deadline = s_flush_deadline;
    } else {
        deadline = intf_clock_get_cycle() + (s_cycles_per_ms * APP_TERMINAL_RUN_BUDGET_US / 1000U);
    }

    while (chry_ringbuffer_get_free(&s_tx_ring) < need_bytes) {
        uint32_t used_before = chry_ringbuffer_get_used(&s_tx_ring);

        app_terminal_tx_flush();
        if (chry_ringbuffer_get_used(&s_tx_ring) >= used_before) {
            break; /* 无进展（端点忙 / 主机不读）：立即放弃 */
        }
        if ((int32_t)(intf_clock_get_cycle() - deadline) >= 0) {
            break; /* 本轮预算耗尽：其余数据下拍继续 */
        }
    }
}

/**
 * @brief 输出核心：TX 环写入 + 有界 flush（环满丢弃计数）。
 * @param data 数据
 * @param size 长度
 * @return 入参 size（调用方不重试、不挂死）
 */
static uint32_t app_terminal_output(const void* data, uint32_t size) {
    uint32_t written;

    /* 换行计数（monitor 状态区布局判定：有换行 = 有输出/新提示符行） */
    if (data != NULL) {
        const char* text = (const char*)data;

        for (uint32_t i = 0U; i < size; i++) {
            if (text[i] == '\n') {
                s_line_feeds++;
            }
        }
    }

    if (!s_ready || !app_usb_is_dtr()) {
        return size; /* 主机未打开端口：直接丢弃，不累积陈旧输出 */
    }

    written = chry_ringbuffer_write(&s_tx_ring, (void*)data, size);
    if (written < size) {
        app_terminal_tx_flush_bounded(size - written);
        written += chry_ringbuffer_write(&s_tx_ring, (void*)((const uint8_t*)data + written),
                                         size - written);
    }
    if (written < size) {
        s_tx_drop_bytes += size - written;
    }

    return size;
}

/**
 * @brief 输出回调：CherrySH → TX 环（恒返回 size，环满有界丢弃）。
 * @param rl readline 实例（未用）
 * @param data 数据
 * @param size 长度
 * @return size（恒等于入参；release 配置下调用方忽略返回值）
 */
static uint16_t app_terminal_sput(chry_readline_t* rl, const void* data, uint16_t size) {
    (void)rl;

    return (uint16_t)app_terminal_output(data, (uint32_t)size);
}

/* ============================================================================
 * RX：sget / 回调链
 * ============================================================================ */

/**
 * @brief 输入回调：USB 驱动环 → CherrySH（非阻塞；任意新输入中止前台 job）。
 * @param rl readline 实例（未用）
 * @param data 输出缓冲
 * @param size 期望长度
 * @return 实际读取字节数
 */
static uint16_t app_terminal_sget(chry_readline_t* rl, void* data, uint16_t size) {
    int received;

    (void)rl;

    received = app_usb_read((uint8_t*)data, (size_t)size);
    if (received > 0) {
        if (app_terminal_job_is_active()) {
            app_terminal_job_abort(); /* 任意新输入中止前台 job */
            return 0U;             /* 吞掉触发键，避免其落入命令行 */
        }
        return (uint16_t)received;
    }

    return 0U;
}

/**
 * @brief 取行内下一个 token（空格分隔）。
 * @param line 行缓冲
 * @param length 有效长度
 * @param position 起始位置
 * @param token 输出 token 起点
 * @param token_length 输出 token 长度
 * @return 下一个扫描位置
 */
static uint16_t app_terminal_line_token(const char* line, uint16_t length, uint16_t position,
                                     const char** token, uint16_t* token_length) {
    uint16_t start;
    uint16_t pos = position;

    while ((pos < length) && (line[pos] == ' ')) {
        pos++;
    }
    start = pos;
    while ((pos < length) && (line[pos] != ' ')) {
        pos++;
    }

    *token = &line[start];
    *token_length = (uint16_t)(pos - start);
    return pos;
}

/* ============================================================================
 * 补全（上下文感知：命令 → 子命令 → 参数；重复 TAB 循环候选）
 * ============================================================================ */

#define APP_TERMINAL_COMPLETION_MAX (40U) /**< 候选上限（与 CONFIG_CSH_MAX_COMPLETION 一致） */

/** 子命令补全表：命令 → 候选 */
typedef struct {
    const char* command;      /**< 命令名 */
    const char* const* items; /**< 候选（静态字符串） */
    uint8_t count;            /**< 候选数 */
} app_terminal_subcommand_t;

/** 参数补全表：命令 + 子命令 → 第 3 个 token 候选 */
typedef struct {
    const char* command;      /**< 命令名 */
    const char* subcommand;   /**< 子命令名 */
    const char* const* items; /**< 候选（静态字符串） */
    uint8_t count;            /**< 候选数 */
} app_terminal_argument_t;

static const char* const s_sub_motor[] = {"start", "stop", "status", "freq", "mod", "iq", "help"};
static const char* const s_sub_inv[] = {"u", "v", "w", "all", "off"};
static const char* const s_sub_foc[] = {"status", "on", "off"};
static const char* const s_sub_adc[] = {"dump", "diag", "delay"};
static const char* const s_sub_enc[] = {"info", "zero", "clear"};
static const char* const s_sub_fault[] = {"show", "clear"};
static const char* const s_sub_pwm[] = {"dump"};
static const char* const s_sub_profiler[] = {"dump"};
static const char* const s_sub_cal[] = {"current", "encoder"};
static const char* const s_sub_reboot[] = {"confirm"};
static const char* const s_sub_param[] = {"list", "get", "set", "reset"};

static const app_terminal_subcommand_t s_subcommands[] = {
    {"motor", s_sub_motor, (uint8_t)(sizeof(s_sub_motor) / sizeof(s_sub_motor[0]))},
    {"inv", s_sub_inv, (uint8_t)(sizeof(s_sub_inv) / sizeof(s_sub_inv[0]))},
    {"foc", s_sub_foc, (uint8_t)(sizeof(s_sub_foc) / sizeof(s_sub_foc[0]))},
    {"adc", s_sub_adc, (uint8_t)(sizeof(s_sub_adc) / sizeof(s_sub_adc[0]))},
    {"enc", s_sub_enc, (uint8_t)(sizeof(s_sub_enc) / sizeof(s_sub_enc[0]))},
    {"fault", s_sub_fault, (uint8_t)(sizeof(s_sub_fault) / sizeof(s_sub_fault[0]))},
    {"pwm", s_sub_pwm, (uint8_t)(sizeof(s_sub_pwm) / sizeof(s_sub_pwm[0]))},
    {"profiler", s_sub_profiler, (uint8_t)(sizeof(s_sub_profiler) / sizeof(s_sub_profiler[0]))},
    {"cal", s_sub_cal, (uint8_t)(sizeof(s_sub_cal) / sizeof(s_sub_cal[0]))},
    {"reboot", s_sub_reboot, (uint8_t)(sizeof(s_sub_reboot) / sizeof(s_sub_reboot[0]))},
    {"param", s_sub_param, (uint8_t)(sizeof(s_sub_param) / sizeof(s_sub_param[0]))},
};

static const char* const s_arg_rotor_output[] = {"rotor", "output"};
static const char* const s_arg_param_domain[] = {"motor", "hardware", "software"};

static const app_terminal_argument_t s_arguments[] = {
    {"enc", "zero", s_arg_rotor_output, (uint8_t)(sizeof(s_arg_rotor_output) / sizeof(s_arg_rotor_output[0]))},
    {"param", "list", s_arg_param_domain, (uint8_t)(sizeof(s_arg_param_domain) / sizeof(s_arg_param_domain[0]))},
};

/** 补全会话（重复 TAB 循环选择） */
typedef struct {
    bool valid;                                  /**< 会话有效 */
    uint16_t word_start;                         /**< 词起点（行内偏移） */
    uint16_t line_len;                           /**< 会话建立时的行长度（失效检测） */
    uint8_t word_len;                            /**< 会话建立时的词长 */
    char word[48];                               /**< 会话建立时的词（识别重复 TAB） */
    const char* items[APP_TERMINAL_COMPLETION_MAX]; /**< 候选（静态字符串） */
    uint8_t lengths[APP_TERMINAL_COMPLETION_MAX];   /**< 候选长度 */
    uint8_t count;                               /**< 候选数 */
    int8_t index;                                /**< 当前选中（-1 = 刚列出） */
} app_terminal_completion_session_t;

static app_terminal_completion_session_t s_completion;

/**
 * @brief 文本与 token 匹配（长度 + 内容）。
 */
static bool app_terminal_token_is(const char* token, uint16_t token_len, const char* text) {
    return (strlen(text) == token_len) && (strncmp(token, text, token_len) == 0);
}

/**
 * @brief 前缀匹配加入候选。
 */
static void app_terminal_add_matches(const char* const* items, uint8_t item_count, const char* word,
                                  uint16_t word_len, const char** candidates, uint8_t* lengths,
                                  uint8_t max, uint8_t* count) {
    for (uint8_t i = 0U; i < item_count; i++) {
        if (*count >= max) {
            return;
        }
        if ((word_len == 0U) || (strncmp(items[i], word, word_len) == 0)) {
            candidates[*count] = items[i];
            lengths[*count] = (uint8_t)strlen(items[i]);
            (*count)++;
        }
    }
}

/**
 * @brief 按光标上下文构建候选（命令 / 子命令 / 参数；空前缀 = 全部）。
 */
static uint8_t app_terminal_build_candidates(const char* line, uint16_t word_start, uint16_t word_len,
                                          const char** candidates, uint8_t* candidate_lengths,
                                          uint8_t candidate_max) {
    const char* t0 = NULL;
    const char* t1 = NULL;
    uint16_t t0_len = 0U;
    uint16_t t1_len = 0U;
    uint16_t pos = 0U;
    uint16_t index = 0U;
    uint8_t count = 0U;
    uint8_t max = (candidate_max < APP_TERMINAL_COMPLETION_MAX) ? candidate_max
                                                            : APP_TERMINAL_COMPLETION_MAX;

    /* 当前词的 token 序号（0 = 命令位） */
    while (pos < word_start) {
        const char* token;
        uint16_t token_len;
        uint16_t next = app_terminal_line_token(line, word_start, pos, &token, &token_len);

        if (token_len == 0U) {
            break;
        }
        if (index == 0U) {
            t0 = token;
            t0_len = token_len;
        } else if (index == 1U) {
            t1 = token;
            t1_len = token_len;
        }
        index++;
        if (next <= pos) {
            break;
        }
        pos = next;
    }

    if (index == 0U) {
        /* 命令位 */
        for (const chry_syscall_t* call = s_csh.cmd_tbl_beg; call < s_csh.cmd_tbl_end; call++) {
            if (count >= max) {
                break;
            }
            if ((word_len == 0U) || (strncmp(call->name, &line[word_start], word_len) == 0)) {
                candidates[count] = call->name;
                candidate_lengths[count] = (uint8_t)strlen(call->name);
                count++;
            }
        }
        return count;
    }

    if ((index == 1U) && (t0 != NULL)) {
        /* 子命令位 */
        for (size_t i = 0U; i < sizeof(s_subcommands) / sizeof(s_subcommands[0]); i++) {
            if (app_terminal_token_is(t0, t0_len, s_subcommands[i].command)) {
                app_terminal_add_matches(s_subcommands[i].items, s_subcommands[i].count,
                                      &line[word_start], word_len, candidates, candidate_lengths, max,
                                      &count);
                return count;
            }
        }
        return 0U;
    }

    if ((index == 2U) && (t0 != NULL) && (t1 != NULL)) {
        /* 参数位：param get/set/reset → 参数名（元数据表） */
        if (app_terminal_token_is(t0, t0_len, "param")
            && (app_terminal_token_is(t1, t1_len, "get") || app_terminal_token_is(t1, t1_len, "set")
                || app_terminal_token_is(t1, t1_len, "reset"))) {
            for (uint32_t i = 0U; (i < g_params_meta_count) && (count < max); i++) {
                if ((word_len == 0U)
                    || (strncmp(g_params_meta[i].name, &line[word_start], word_len) == 0)) {
                    candidates[count] = g_params_meta[i].name;
                    candidate_lengths[count] = (uint8_t)strlen(g_params_meta[i].name);
                    count++;
                }
            }
            return count;
        }
        /* 参数位：表驱动（enc zero / param list） */
        for (size_t i = 0U; i < sizeof(s_arguments) / sizeof(s_arguments[0]); i++) {
            if (app_terminal_token_is(t0, t0_len, s_arguments[i].command)
                && app_terminal_token_is(t1, t1_len, s_arguments[i].subcommand)) {
                app_terminal_add_matches(s_arguments[i].items, s_arguments[i].count, &line[word_start],
                                      word_len, candidates, candidate_lengths, max, &count);
                return count;
            }
        }
        return 0U;
    }

    return 0U;
}

/**
 * @brief 词替换（TAB 循环：删旧词 + 插新词 + 重绘）。
 */
static void app_terminal_replace_word(chry_readline_t* rl, uint16_t old_len, const char* text,
                                   uint8_t text_len) {
    for (uint16_t i = 0U; i < old_len; i++) {
        (void)chry_readline_edit_backspace(rl);
    }
    for (uint8_t i = 0U; i < text_len; i++) {
        (void)chry_readline_edit_insert(rl, text[i]);
    }
    (void)chry_readline_edit_refresh(rl);
}

/**
 * @brief 重复 TAB 循环：当前词与会话一致时替换为下一个候选。
 * @return true = 已处理（词替换已在回调内完成）
 */
static bool app_terminal_completion_cycle(chry_readline_t* rl, const char* line, uint16_t word_start,
                                       uint16_t word_len) {
    const char* word;
    const char* expected;
    uint8_t expected_len;
    bool matched = false;

    if (!s_completion.valid || (word_start != s_completion.word_start)
        || (s_completion.count == 0U)) {
        return false;
    }

    word = &line[word_start];

    if (s_completion.index < 0) {
        expected = s_completion.word;
        expected_len = s_completion.word_len;
    } else {
        expected = s_completion.items[s_completion.index];
        expected_len = s_completion.lengths[s_completion.index];
    }

    if ((word_len == expected_len) && (strncmp(word, expected, word_len) == 0)) {
        /* 词未变（或等于当前候选）：行长度须与会话一致（防跨行误判） */
        matched = (rl->ln.buff->size == s_completion.line_len);
    } else if ((s_completion.index < 0) && (word_len > s_completion.word_len)
               && (strncmp(word, s_completion.word, s_completion.word_len) == 0)
               && (rl->ln.buff->size > s_completion.line_len)) {
        /* 公共前缀扩展（首 TAB 后内置逻辑加长了词）：当前词以会话词开头，
         * 且所有候选仍以当前词开头 → 同一会话，继续循环 */
        matched = true;
        for (uint8_t i = 0U; i < s_completion.count; i++) {
            if (strncmp(s_completion.items[i], word, word_len) != 0) {
                matched = false;
                break;
            }
        }
    } else {
        matched = false;
    }

    if (!matched) {
        return false;
    }

    s_completion.index = (int8_t)((s_completion.index + 1) % (int8_t)s_completion.count);
    app_terminal_replace_word(rl, word_len, s_completion.items[s_completion.index],
                           s_completion.lengths[s_completion.index]);
    s_completion.line_len = rl->ln.buff->size; /* 替换后的行长度（保持会话有效） */
    return true;
}

/**
 * @brief 补全回调：上下文候选（命令/子命令/参数）+ 重复 TAB 循环选择。
 *
 * 行为：
 *   首次 TAB → 返回候选（内置逻辑：公共前缀扩展 + 列候选）；
 *   再次 TAB（同一个词）→ 用下一个候选替换当前词（循环）；回车即执行所选命令。
 */
static uint8_t app_terminal_completion(chry_readline_t* rl, char* prefix, uint16_t* size,
                                    const char** candidates, uint8_t* candidate_lengths,
                                    uint8_t candidate_max) {
    const char* line;
    uint16_t word_start;
    uint16_t word_len;
    uint8_t count;

    if (rl->ln.buff == NULL) {
        return 0U;
    }
    line = rl->ln.buff->pbuf;

    /* 当前词范围（光标向左至空格） */
    word_start = rl->ln.curoff;
    while ((word_start > 0U) && (line[word_start - 1U] != ' ')) {
        word_start--;
    }
    word_len = (uint16_t)(rl->ln.curoff - word_start);

    /* 重复 TAB：循环候选（词替换已在回调内完成） */
    if (app_terminal_completion_cycle(rl, line, word_start, word_len)) {
        return 0U;
    }

    /* 构建候选 */
    count = app_terminal_build_candidates(line, word_start, word_len, candidates, candidate_lengths,
                                       candidate_max);
    if (count == 0U) {
        s_completion.valid = false;
        /* 变量补全（$ 前缀）交原回调 */
        if ((word_len > 0U) && (line[word_start] == '$') && (s_original_completion != NULL)) {
            return s_original_completion(rl, prefix, size, candidates, candidate_lengths,
                                         candidate_max);
        }
        return 0U;
    }

    /* 记录会话（供下次 TAB 循环） */
    s_completion.valid = true;
    s_completion.word_start = word_start;
    s_completion.line_len = rl->ln.buff->size;
    s_completion.word_len = (word_len < (sizeof(s_completion.word) - 1U))
                                ? (uint8_t)word_len
                                : (uint8_t)(sizeof(s_completion.word) - 1U);
    memcpy(s_completion.word, &line[word_start], s_completion.word_len);
    s_completion.count = count;
    s_completion.index = -1;
    for (uint8_t i = 0U; i < count; i++) {
        s_completion.items[i] = candidates[i];
        s_completion.lengths[i] = candidate_lengths[i];
    }

    return count;
}

/**
 * @brief 用户事件回调（链式）：Ctrl-C 中止前台 job / 关闭 monitor → 原回调。
 *
 * 优先级：前台 job（如 cal current）→ monitor 常驻状态区。
 * 注：CherryRL 在"行非空"时 Ctrl-C 仅清行，不触发本回调（再按一次即可）。
 */
static int app_terminal_user_cb(chry_readline_t* rl, uint8_t exec) {
    if (exec == CHRY_READLINE_EXEC_SIGINT) {
        if (app_terminal_job_is_active()) {
            app_terminal_job_abort();
        } else if (app_terminal_monitor_is_active()) {
            static const char s_monitor_off_msg[] = "\r\nmonitor: OFF (Ctrl-C)\r\n";

            app_terminal_monitor_set(false);
            app_terminal_write(s_monitor_off_msg, sizeof(s_monitor_off_msg) - 1U);
        } else {
            /* 无 job / 无 monitor：交原回调处理（清行/重绘） */
        }
    }

    if (s_original_user_cb != NULL) {
        return s_original_user_cb(rl, exec);
    }
    return 1;
}

/* ============================================================================
 * DTR 边沿 + 欢迎界面
 * ============================================================================ */

/** 连接欢迎界面（DTR 上升沿输出）
 *  主标题：ANSI Shadow 艺术字（pyfiglet "ansi_shadow"）——Hajimi Dynamics 单行（110 列 × 6 行）
 *  副标题：ANSI Regular 艺术字（pyfiglet "ansi_regular"）——G6618（38 列 × 5 行，尺寸明显小于主标题）
 *  配色：蒸汽波 256 色（主标题纵向渐变 粉→紫→青；副标题青→粉；边框横向 6 段渐变）
 *  排版：边框内宽 112，总宽 114 列，共 17 行；UTF-8 块字符（需终端 UTF-8） */
static const char* const s_banner_lines[] = {
    "\r\n",
    "╭\033[38;5;213m───────────────────\033[38;5;207m───────────────────\033[38;5;141m───────────────────\033[38;5;51m───────────────────\033[38;5;121m──────────────────\033[38;5;51m──────────────────\033[38;5;213m╮\r\n",
    "\033[38;5;213m" "│ ██╗  ██╗ █████╗      ██╗██╗███╗   ███╗██╗    ██████╗ ██╗   ██╗███╗   ██╗ █████╗ ███╗   ███╗██╗ ██████╗███████╗ │\r\n" "\033[0m",
    "\033[38;5;207m" "│ ██║  ██║██╔══██╗     ██║██║████╗ ████║██║    ██╔══██╗╚██╗ ██╔╝████╗  ██║██╔══██╗████╗ ████║██║██╔════╝██╔════╝ │\r\n" "\033[0m",
    "\033[38;5;141m" "│ ███████║███████║     ██║██║██╔████╔██║██║    ██║  ██║ ╚████╔╝ ██╔██╗ ██║███████║██╔████╔██║██║██║     ███████╗ │\r\n" "\033[0m",
    "\033[38;5;99m" "│ ██╔══██║██╔══██║██   ██║██║██║╚██╔╝██║██║    ██║  ██║  ╚██╔╝  ██║╚██╗██║██╔══██║██║╚██╔╝██║██║██║     ╚════██║ │\r\n" "\033[0m",
    "\033[38;5;63m" "│ ██║  ██║██║  ██║╚█████╔╝██║██║ ╚═╝ ██║██║    ██████╔╝   ██║   ██║ ╚████║██║  ██║██║ ╚═╝ ██║██║╚██████╗███████║ │\r\n" "\033[0m",
    "\033[38;5;51m" "│ ╚═╝  ╚═╝╚═╝  ╚═╝ ╚════╝ ╚═╝╚═╝     ╚═╝╚═╝    ╚═════╝    ╚═╝   ╚═╝  ╚═══╝╚═╝  ╚═╝╚═╝     ╚═╝╚═╝ ╚═════╝╚══════╝ │\r\n" "\033[0m",
    "│                                                                                                                │\r\n",
    "\033[38;5;45m" "│                                      ██████   ██████   ██████   ██  █████                                      │\r\n" "\033[0m",
    "\033[38;5;51m" "│                                     ██       ██       ██       ███ ██   ██                                     │\r\n" "\033[0m",
    "\033[38;5;141m" "│                                     ██   ███ ███████  ███████   ██  █████                                      │\r\n" "\033[0m",
    "\033[38;5;207m" "│                                     ██    ██ ██    ██ ██    ██  ██ ██   ██                                     │\r\n" "\033[0m",
    "\033[38;5;213m" "│                                      ██████   ██████   ██████   ██  █████                                      │\r\n" "\033[0m",
    "│                                                                                                                │\r\n",
    "\033[38;5;141m" "│                                        Kaiser @ Alliance HardwareGroup                                         │\r\n" "\033[0m",
    "\033[38;5;121m" "│                                  firmware v0.1.0  |  type 'help' for commands                                  │\r\n" "\033[0m",
    "╰\033[38;5;51m───────────────────\033[38;5;121m───────────────────\033[38;5;51m───────────────────\033[38;5;141m───────────────────\033[38;5;207m──────────────────\033[38;5;213m──────────────────\033[38;5;213m╯\r\n",
    "\r\n",
};

/**
 * @brief 输出欢迎界面（逐行写入 TX 环）。
 */
static void app_terminal_write_banner(void) {
    for (size_t i = 0U; i < sizeof(s_banner_lines) / sizeof(s_banner_lines[0]); i++) {
        app_terminal_write(s_banner_lines[i], strlen(s_banner_lines[i]));
    }
}

/**
 * @brief DTR 上升沿：清 TX 环 + 欢迎界面 + 提示符重绘（避免重连后陈旧输出）。
 */
static void app_terminal_dtr_process(void) {
    bool dtr = app_usb_is_dtr();

    if (dtr && !s_last_dtr) {
        chry_ringbuffer_reset(&s_tx_ring);
        app_terminal_write_banner();
        (void)chry_readline_edit_refresh(&s_csh.rl);
    }

    s_last_dtr = dtr;
}

/* ============================================================================
 * 对外接口
 * ============================================================================ */

void app_terminal_init(void) {
    chry_shell_init_t csh_init = {0};
    chry_readline_t* rl;
    uint32_t cpu_freq;

    if (chry_ringbuffer_init(&s_tx_ring, s_tx_pool, sizeof(s_tx_pool)) != 0) {
        return;
    }

    csh_init.sput = app_terminal_sput;
    csh_init.sget = app_terminal_sget;
    csh_init.command_table_beg = &__fsymtab_start;
    csh_init.command_table_end = &__fsymtab_end;
    csh_init.variable_table_beg = &__vsymtab_start;
    csh_init.variable_table_end = &__vsymtab_end;
    csh_init.prompt_buffer = s_prompt_buffer;
    csh_init.prompt_buffer_size = (uint16_t)sizeof(s_prompt_buffer);
    csh_init.history_buffer = s_history_buffer;
    csh_init.history_buffer_size = (uint16_t)sizeof(s_history_buffer);
    csh_init.line_buffer = s_line_buffer;
    csh_init.line_buffer_size = (uint32_t)sizeof(s_line_buffer);
    csh_init.uid = 0;
    csh_init.user[0] = APP_TERMINAL_USER;
    csh_init.hash[0] = ""; /* 无登录（空串：避免 hash 为 NULL 时 strnlen 读空指针） */
    csh_init.host = APP_TERMINAL_HOST;
    csh_init.user_data = NULL;

    if (chry_shell_init(&s_csh, &csh_init) != 0) {
        return;
    }
    (void)chry_shell_set_path(&s_csh, 0U, APP_TERMINAL_PATH);

    /* 防御：命令查找依赖 PATH 环境变量（变量表非空）与命令表非空；
     * 表为空时所有命令将 "command not found"，此处经 RTT 记录便于排查。 */
    if ((&__fsymtab_start == &__fsymtab_end) || (&__vsymtab_start == &__vsymtab_end)) {
        app_debug_printf("[SHELL] WARN: command/variable table empty (commands will not resolve)\r\n");
    }

    /* 回调链：保存 CherrySH 内部回调后接管（补全 / 用户事件） */
    rl = &s_csh.rl;
    s_original_completion = rl->cplt.acb;
    chry_readline_set_completion_cb(rl, app_terminal_completion);
    s_original_user_cb = rl->ucb;
    chry_readline_set_user_cb(rl, app_terminal_user_cb);

    cpu_freq = intf_clock_get_cpu_freq();
    s_cycles_per_ms = cpu_freq / 1000U;
    s_ms_last_cycle = intf_clock_get_cycle();
    s_ms_remainder = 0U;
    s_now_ms = 0U;
    s_tx_drop_bytes = 0U;
    s_last_dtr = app_usb_is_dtr();

    s_ready = true;
}

void app_terminal_run_once(void) {
    uint32_t cycle;
    uint32_t delta;
    uint32_t total;

    if (!s_ready) {
        return;
    }

    /* 本轮输出 flush 总预算（命令/回显/job 输出共享，保证单拍有界） */
    s_flush_deadline = intf_clock_get_cycle() + (s_cycles_per_ms * APP_TERMINAL_RUN_BUDGET_US / 1000U);
    s_flush_deadline_valid = true;

    /* 1) 执行已解析命令（单线程配置下由 task_repl 内部完成；此处处理状态机推进） */
    chry_shell_task_exec(&s_csh);

    /* 2) 读取输入（非阻塞；命令解析/执行亦在此步完成） */
    (void)chry_shell_task_repl(&s_csh);

    /* 3) 毫秒计时推进 + job tick */
    cycle = intf_clock_get_cycle();
    delta = cycle - s_ms_last_cycle;
    s_ms_last_cycle = cycle;
    if (s_cycles_per_ms > 0U) {
        total = delta + s_ms_remainder;
        s_now_ms += total / s_cycles_per_ms;
        s_ms_remainder = total % s_cycles_per_ms;
    }
    app_terminal_job_tick(s_now_ms);

    /* 3a) 常驻状态区刷新（monitor；未启用时空操作） */
    app_terminal_monitor_run_once(s_now_ms);

    /* 4) DTR 边沿（清环 + banner + 提示符） */
    app_terminal_dtr_process();

    /* 5) TX flush（非阻塞，单拍有界） */
    app_terminal_tx_flush();
    s_flush_deadline_valid = false;
}

bool app_terminal_is_ready(void) {
    return s_ready;
}

void app_terminal_write(const void* data, size_t len) {
    if ((data == NULL) || (len == 0U)) {
        return;
    }

    (void)app_terminal_output(data, (uint32_t)len);
}

void app_terminal_refresh(void) {
    if (!s_ready) {
        return;
    }

    (void)chry_readline_edit_refresh(&s_csh.rl);
}

uint32_t app_terminal_get_tx_drop_bytes(void) {
    return s_tx_drop_bytes;
}

uint32_t app_terminal_get_line_feeds(void) {
    return s_line_feeds;
}
