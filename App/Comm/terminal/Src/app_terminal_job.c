/**
 * @file    app_terminal_job.c
 * @brief   Terminal 长命令 job 框架实现（单前台 job）
 * @author  Kaiser
 *
 * Copyright (c) 2026 Alliance HardwareGroup
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_terminal_job.h"

#include <stddef.h>

/** 当前前台 job（NULL = 无） */
static app_terminal_job_t *s_active_job;

int app_terminal_job_start(app_terminal_job_t *job) {
    if ((job == NULL) || (job->tick == NULL)) {
        return -1;
    }

    if (s_active_job != NULL) {
        app_terminal_job_abort();
    }

    job->active = true;
    s_active_job = job;

    return 0;
}

void app_terminal_job_tick(uint32_t now_ms) {
    if (s_active_job == NULL) {
        return;
    }
    if (!s_active_job->active) {
        s_active_job = NULL;
        return;
    }

    s_active_job->tick(now_ms);
}

void app_terminal_job_abort(void) {
    app_terminal_job_t *job = s_active_job;

    if (job == NULL) {
        return;
    }

    s_active_job = NULL;
    job->active = false;
    if (job->abort != NULL) {
        job->abort();
    }
}

bool app_terminal_job_is_active(void) {
    return (s_active_job != NULL) && s_active_job->active;
}

const char *app_terminal_job_name(void) {
    return (s_active_job != NULL) ? s_active_job->name : NULL;
}
