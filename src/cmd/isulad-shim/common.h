/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: leizhongkai
 * Create: 2020-1-20
 * Description: common definition of isulad-shim
 ******************************************************************************/

#ifndef CMD_ISULAD_SHIM_COMMON_H
#define CMD_ISULAD_SHIM_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#include <isula_libutils/utils.h>

#ifdef __cplusplus
extern "C" {
#endif

// error code
#define SHIM_ERR_BASE (-10000)
#define SHIM_SYS_ERR(err) (SHIM_ERR_BASE - err)
#define SHIM_OK 0
#define SHIM_ERR (-1)
#define SHIM_ERR_WAIT (-2)
#define SHIM_ERR_NOT_REQUIRED (-3)
#define SHIM_ERR_TIMEOUT (-4)

// common exit code is defined in stdlib.h
// EXIT_FAILURE 1   : Failing exit status.
// EXIT_SUCCESS 0   : Successful exit status.
// custom shim exit code
// SHIM_EXIT_TIMEOUT 2: Container process timeout exit code
#define SHIM_EXIT_TIMEOUT 2

#define INFO_MSG "info"
#define WARN_MSG "warn"
#define ERR_MSG "error"

#define DEFAULT_TIMEOUT 120 // sec
#define CONTAINER_ID_LEN 64
#define MAX_RT_NAME_LEN 64
#define MAX_CONSOLE_SOCK_LEN 32

#define MAX_RUNTIME_ARGS 100

#define SHIM_BINARY "isulad-shim"
#define SHIM_LOG_NAME "shim-log.json"

#define CONTAINER_ACTION_REBOOT 129
#define CONTAINER_ACTION_SHUTDOWN 130

int init_shim_log(void);

void signal_routine(int sig);

/**
 * retry_cnt: max count of call cb;
 * interval_us: how many us to sleep, after call cb;
 * cb: retry call function;
 * return:
 *  0 is cb successful at least once;
 *  1 is all cb are failure;
*/
#define DO_RETRY_CALL(retry_cnt, interval_us, ret, cb, ...) do {    \
        size_t i = 0;                                               \
        for(; i < retry_cnt; i++) {                                 \
            ret = cb(##__VA_ARGS__);                                  \
            if (ret == 0) {                                         \
                break;                                              \
            }                                                       \
            isula_usleep_nointerupt(interval_us);                    \
        }                                                           \
    } while(0)

#define UTIL_FREE_AND_SET_NULL(p) \
    do {                          \
        if ((p) != NULL) {        \
            free((void *)(p));    \
            (p) = NULL;           \
        }                         \
    } while (0)

char *read_text_file(const char *path);

int cmd_combined_output(const char *binary, const char *params[], void *output, int *output_len);

void write_message(const char *level, const char *fmt, ...);

int generate_random_str(char *id, size_t len);

void close_fd(int *pfd);

int open_no_inherit(const char *path, int flag, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
