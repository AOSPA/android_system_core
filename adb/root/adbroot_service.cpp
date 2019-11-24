/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <private/android_filesystem_config.h>

#include "adbroot_service.h"

namespace {
const std::string kStoragePath = "/data/adbroot/";
const std::string kEnabled = "enabled";

static android::binder::Status SecurityException(const std::string& msg) {
    LOG(ERROR) << msg;
    return android::binder::Status::fromExceptionCode(android::binder::Status::EX_SECURITY,
            android::String8(msg.c_str()));
}
}  // anonymous namespace

namespace android {
namespace adbroot {

using base::ReadFileToString;
using base::Trim;
using base::WriteStringToFile;

ADBRootService::ADBRootService() : enabled_(false) {
    std::string buf;
    if (ReadFileToString(kStoragePath + kEnabled, &buf)) {
        enabled_ = std::stoi(Trim(buf));
    }
}

void ADBRootService::Register() {
    auto ret = BinderService<ADBRootService>::publish();
    if (ret != OK) {
        LOG(FATAL) << "Could not register adbroot service: " << ret;
    }
}

binder::Status ADBRootService::setEnabled(bool enabled) {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (uid != AID_SYSTEM) {
        return SecurityException("Caller must be system");
    }

    AutoMutex _l(lock_);

    if (enabled_ != enabled) {
        enabled_ = enabled;
        WriteStringToFile(std::to_string(enabled), kStoragePath + kEnabled);

        // Turning off adb root, restart adbd.
        if (!enabled) {
            base::SetProperty("lineage.service.adb.root", "0");
            base::SetProperty("ctl.restart", "adbd");
        }
    }

    return binder::Status::ok();
}

binder::Status ADBRootService::getEnabled(bool* _aidl_return) {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (uid != AID_SYSTEM && uid != AID_SHELL) {
        return SecurityException("Caller must be system or shell");
    }

    AutoMutex _l(lock_);
    *_aidl_return = enabled_;
    return binder::Status::ok();
}

}  // namespace adbroot
}  // namespace android
