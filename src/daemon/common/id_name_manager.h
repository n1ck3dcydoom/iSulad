/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2023. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: zhongtao
 * Create: 2023-06-27
 * Description: provide id and name manage functions
 ******************************************************************************/
#ifndef DAEMON_COMMON_ID_NAME_MANAGER_H
#define DAEMON_COMMON_ID_NAME_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int id_name_manager_init(void);
void id_name_manager_release(void);
bool id_name_manager_add_entry_with_existing_id(const char *id, const char *name);
bool id_name_manager_add_entry_with_new_id(const char *name, char **id);
bool id_name_manager_add_entry_with_new_id_and_name(char **id, char **name);
bool id_name_manager_remove_entry(const char *id, const char *name);
bool id_name_manager_rename(const char *new_name, const char *old_name);

#ifdef __cplusplus
}
#endif

#endif