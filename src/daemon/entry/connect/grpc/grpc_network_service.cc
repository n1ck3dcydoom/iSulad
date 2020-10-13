/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2018-2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: zhangxiaoyu
 * Create: 2020-09-11
 * Description: provide grpc network functions
 ******************************************************************************/
#include "grpc_network_service.h"

#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include "grpc_server_tls_auth.h"
#include "isula_libutils/log.h"
#include "utils.h"
#include "error.h"

int NetworkServiceImpl::create_request_from_grpc(const NetworkCreateRequest *grequest, network_create_request **request)
{
    network_create_request *tmpreq = nullptr;

    tmpreq = (network_create_request *)util_common_calloc_s(sizeof(network_create_request));
    if (tmpreq == nullptr) {
        ERROR("Out of memory");
        return -1;
    }

    if (!grequest->name().empty()) {
        tmpreq->name = util_strdup_s(grequest->name().c_str());
    }
    if (!grequest->driver().empty()) {
        tmpreq->driver = util_strdup_s(grequest->driver().c_str());
    }
    if (!grequest->gateway().empty()) {
        tmpreq->gateway = util_strdup_s(grequest->gateway().c_str());
    }
    tmpreq->internal = grequest->internal();
    if (!grequest->subnet().empty()) {
        tmpreq->subnet = util_strdup_s(grequest->subnet().c_str());
    }

    *request = tmpreq;
    return 0;
}

int NetworkServiceImpl::create_response_to_grpc(const network_create_response *response,
                                                NetworkCreateResponse *gresponse)
{
    if (response == nullptr) {
        gresponse->set_cc(ISULAD_ERR_EXEC);
        return 0;
    }
    gresponse->set_cc(response->cc);
    if (response->errmsg != nullptr) {
        gresponse->set_errmsg(response->errmsg);
    }
    if (response->path != nullptr) {
        gresponse->set_path(response->path);
    }
    return 0;
}

Status NetworkServiceImpl::Create(ServerContext *context, const NetworkCreateRequest *request,
                                  NetworkCreateResponse *reply)
{
    int ret, tret;
    service_executor_t *cb = nullptr;
    network_create_response *network_res = nullptr;
    network_create_request *network_req = nullptr;

    auto status = GrpcServerTlsAuth::auth(context, "network_create");
    if (!status.ok()) {
        return status;
    }
    cb = get_service_executor();
    if (cb == nullptr || cb->network.create == nullptr) {
        return Status(StatusCode::UNIMPLEMENTED, "Unimplemented callback");
    }

    tret = create_request_from_grpc(request, &network_req);
    if (tret != 0) {
        ERROR("Failed to transform grpc request");
        reply->set_cc(ISULAD_ERR_INPUT);
        return Status::OK;
    }

    ret = cb->network.create(network_req, &network_res);
    tret = create_response_to_grpc(network_res, reply);

    free_network_create_request(network_req);
    free_network_create_response(network_res);
    if (tret != 0) {
        reply->set_errmsg(errno_to_error_message(ISULAD_ERR_INTERNAL));
        reply->set_cc(ISULAD_ERR_INTERNAL);
        ERROR("Failed to translate response to grpc, operation is %s", ret ? "failed" : "success");
    }
    return Status::OK;
}