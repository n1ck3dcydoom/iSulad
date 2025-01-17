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
 * Author: lifeng
 * Create: 2018-11-08
 * Description: provide runtime image functions
 ******************************************************************************/

#include "cri_v1_runtime_image_service.h"
#include <memory>
#include <string>
#include <vector>
#include "isula_libutils/log.h"
#include "cri_helpers.h"
#include "v1_cri_image_manager_service_impl.h"

RuntimeV1ImageServiceImpl::RuntimeV1ImageServiceImpl()
{
    std::unique_ptr<ImageManagerService> service(new ImageManagerServiceImpl);
    rService = std::move(service);
}

grpc::Status RuntimeV1ImageServiceImpl::PullImage(grpc::ServerContext *context,
                                                const runtime::v1::PullImageRequest *request,
                                                runtime::v1::PullImageResponse *reply)
{
    Errors error;

    if (request == nullptr || reply == nullptr) {
        ERROR("Invalid arguments");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid arguments");
    }

    EVENT("Event: {Object: CRI, Type: Pulling image %s}", request->image().image().c_str());

    std::string imageRef = rService->PullImage(request->image(), request->auth(), error);
    if (!error.Empty() || imageRef.empty()) {
        ERROR("{Object: CRI, Type: Failed to pull image %s}", request->image().image().c_str());
        return grpc::Status(grpc::StatusCode::UNKNOWN, error.GetMessage());
    }
    reply->set_image_ref(imageRef);

    EVENT("Event: {Object: CRI, Type: Pulled image %s with ref %s}", request->image().image().c_str(),
          imageRef.c_str());
    return grpc::Status::OK;
}

grpc::Status RuntimeV1ImageServiceImpl::ListImages(grpc::ServerContext *context,
                                                 const runtime::v1::ListImagesRequest *request,
                                                 runtime::v1::ListImagesResponse *reply)
{
    std::vector<std::unique_ptr<runtime::v1::Image>> images;
    Errors error;

    if (request == nullptr || reply == nullptr) {
        ERROR("Invalid arguments");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid arguments");
    }

    INFO("Event: {Object: CRI, Type: Listing all images}");

    rService->ListImages(request->filter(), images, error);
    if (!error.Empty()) {
        ERROR("{Object: CRI, Type: Failed to list all images: %s}", error.GetMessage().c_str());
        return grpc::Status(grpc::StatusCode::UNKNOWN, error.GetMessage());
    }

    for (auto iter = images.begin(); iter != images.end(); iter++) {
        runtime::v1::Image *image = reply->add_images();
        if (image == nullptr) {
            return grpc::Status(grpc::StatusCode::UNKNOWN, "Out of memory");
        }
        *image = *(iter->get());
    }

    INFO("Event: {Object: CRI, Type: Listed all images}");

    return grpc::Status::OK;
}

grpc::Status RuntimeV1ImageServiceImpl::ImageStatus(grpc::ServerContext *context,
                                                  const runtime::v1::ImageStatusRequest *request,
                                                  runtime::v1::ImageStatusResponse *reply)
{
    std::unique_ptr<runtime::v1::Image> image_info = nullptr;
    Errors error;

    if (request == nullptr || reply == nullptr) {
        ERROR("Invalid arguments");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid arguments");
    }

    INFO("Event: {Object: CRI, Type: Statusing image %s}", request->image().image().c_str());

    image_info = rService->ImageStatus(request->image(), error);
    if (!error.Empty() && !CRIHelpers::IsImageNotFoundError(error.GetMessage())) {
        ERROR("{Object: CRI, Type: Failed to status image: %s due to %s}", request->image().image().c_str(),
              error.GetMessage().c_str());
        return grpc::Status(grpc::StatusCode::UNKNOWN, error.GetMessage());
    }

    if (image_info != nullptr) {
        runtime::v1::Image *image = reply->mutable_image();
        *image = *image_info;
    }

    INFO("Event: {Object: CRI, Type: Statused image %s}", request->image().image().c_str());

    return grpc::Status::OK;
}

grpc::Status RuntimeV1ImageServiceImpl::ImageFsInfo(grpc::ServerContext *context,
                                                  const runtime::v1::ImageFsInfoRequest *request,
                                                  runtime::v1::ImageFsInfoResponse *reply)
{
    std::vector<std::unique_ptr<runtime::v1::FilesystemUsage>> usages;
    Errors error;

    if (request == nullptr || reply == nullptr) {
        ERROR("Invalid arguments");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid arguments");
    }

    INFO("Event: {Object: CRI, Type: Statusing image fs info}");

    rService->ImageFsInfo(usages, error);
    if (!error.Empty()) {
        ERROR("{Object: CRI, Type: Failed to status image fs info: %s}", error.GetMessage().c_str());
        return grpc::Status(grpc::StatusCode::UNKNOWN, error.GetMessage());
    }

    for (auto iter = usages.begin(); iter != usages.end(); ++iter) {
        runtime::v1::FilesystemUsage *fs_info = reply->add_image_filesystems();
        if (fs_info == nullptr) {
            ERROR("{Object: CRI, Type: Failed to status image fs info: Out of memory}");
            return grpc::Status(grpc::StatusCode::UNKNOWN, "Out of memory");
        }
        *fs_info = *(iter->get());
    }

    INFO("Event: {Object: CRI, Type: Statused image fs info}");
    return grpc::Status::OK;
}

grpc::Status RuntimeV1ImageServiceImpl::RemoveImage(grpc::ServerContext *context,
                                                  const runtime::v1::RemoveImageRequest *request,
                                                  runtime::v1::RemoveImageResponse *reply)
{
    Errors error;

    if (request == nullptr) {
        ERROR("Invalid arguments");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid arguments");
    }

    EVENT("Event: {Object: CRI, Type: Removing image %s}", request->image().image().c_str());

    rService->RemoveImage(request->image(), error);
    if (!error.Empty()) {
        ERROR("{Object: CRI, Type: Failed to remove image %s due to: %s}", request->image().image().c_str(),
              error.GetMessage().c_str());
        return grpc::Status(grpc::StatusCode::UNKNOWN, error.GetMessage());
    }

    EVENT("Event: {Object: CRI, Type: Removed image %s}", request->image().image().c_str());
    return grpc::Status::OK;
}
