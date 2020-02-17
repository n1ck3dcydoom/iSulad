/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
 * iSulad licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *     http://license.coscl.org.cn/MulanPSL
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v1 for more details.
 * Author: wujing
 * Create: 2020-02-14
 * Description: provide isulad config mock
 ******************************************************************************/

#ifndef ISULAD_CONFIG_MOCK_H_
#define ISULAD_CONFIG_MOCK_H_

#include <gmock/gmock.h>
#include "isulad_config.h"

class MockIsuladConf {
public:
    virtual ~MockIsuladConf() = default;
    MOCK_METHOD1(GetRuntimeDir, char *(const char *name));
    MOCK_METHOD3(ParseLogopts, int(struct service_arguments *args, const char *key, const char *value));
};

void MockIsuladConf_SetMock(MockIsuladConf* mock);

#endif  // ISULAD_CONFIG_MOCK_H_
