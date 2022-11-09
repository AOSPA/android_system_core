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

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/os/IVold.h>
#include <binder/IServiceManager.h>
#include <bootloader_message/bootloader_message.h>
#include <cutils/android_reboot.h>
#include <fs_mgr_overlayfs.h>
#include <fs_mgr_priv.h>
#include <fstab/fstab.h>
#include <libavb_user/libavb_user.h>
#include <libgsi/libgsid.h>

#include "fs_mgr_priv_overlayfs.h"

using namespace std::literals;
using android::fs_mgr::Fstab;
using android::fs_mgr::FstabEntry;

namespace {

void usage() {
    LOG(INFO) << getprogname()
              << " [-h] [-R] [-T fstab_file] [partition]...\n"
                 "\t-h --help\tthis help\n"
                 "\t-R --reboot\tdisable verity & reboot to facilitate remount\n"
                 "\t-T --fstab\tcustom fstab file location\n"
                 "\tpartition\tspecific partition(s) (empty does all)\n"
                 "\n"
                 "Remount specified partition(s) read-write, by name or mount point.\n"
                 "-R notwithstanding, verity must be disabled on partition(s).\n"
                 "-R within a DSU guest system reboots into the DSU instead of the host system,\n"
                 "this command would enable DSU (one-shot) if not already enabled.";
}

const std::string system_mount_point(const android::fs_mgr::FstabEntry& entry) {
    if (entry.mount_point == "/") return "/system";
    return entry.mount_point;
}

const FstabEntry* GetWrappedEntry(const Fstab& overlayfs_candidates, const FstabEntry& entry) {
    auto mount_point = system_mount_point(entry);
    auto it = std::find_if(overlayfs_candidates.begin(), overlayfs_candidates.end(),
                           [&mount_point](const auto& entry) {
                               return android::base::StartsWith(mount_point,
                                                                system_mount_point(entry) + "/");
                           });
    if (it == overlayfs_candidates.end()) return nullptr;
    return &(*it);
}

class MyLogger {
  public:
    explicit MyLogger(bool verbose) : verbose_(verbose) {}

    void operator()(android::base::LogId id, android::base::LogSeverity severity, const char* tag,
                    const char* file, unsigned int line, const char* message) {
        // By default, print ERROR logs and logs of this program (does not start with '[')
        // Print [libfs_mgr] INFO logs only if -v is given.
        if (verbose_ || severity >= android::base::ERROR || message[0] != '[') {
            fprintf(stderr, "%s\n", message);
        }
        logd_(id, severity, tag, file, line, message);
    }

  private:
    android::base::LogdLogger logd_;
    bool verbose_;
};

[[noreturn]] void reboot(const std::string& name) {
    LOG(INFO) << "Rebooting device for new settings to take effect";
    ::sync();
    android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot," + name);
    ::sleep(60);
    LOG(ERROR) << "Failed to reboot";
    ::exit(1);
}

static android::sp<android::os::IVold> GetVold() {
    while (true) {
        if (auto sm = android::defaultServiceManager()) {
            if (auto binder = sm->getService(android::String16("vold"))) {
                if (auto vold = android::interface_cast<android::os::IVold>(binder)) {
                    return vold;
                }
            }
        }
        std::this_thread::sleep_for(2s);
    }
}

}  // namespace

enum RemountStatus {
    REMOUNT_SUCCESS = 0,
    UNKNOWN_PARTITION = 5,
    INVALID_PARTITION,
    VERITY_PARTITION,
    BAD_OVERLAY,
    NO_MOUNTS,
    REMOUNT_FAILED,
    BINDER_ERROR,
    CHECKPOINTING,
    GSID_ERROR,
};

static bool ReadFstab(const char* fstab_file, android::fs_mgr::Fstab* fstab) {
    if (fstab_file) {
        return android::fs_mgr::ReadFstabFromFile(fstab_file, fstab);
    }
    if (!android::fs_mgr::ReadDefaultFstab(fstab)) {
        return false;
    }

    // Manufacture a / entry from /proc/mounts if missing.
    if (!GetEntryForMountPoint(fstab, "/system") && !GetEntryForMountPoint(fstab, "/")) {
        android::fs_mgr::Fstab mounts;
        if (android::fs_mgr::ReadFstabFromFile("/proc/mounts", &mounts)) {
            if (auto entry = GetEntryForMountPoint(&mounts, "/")) {
                if (entry->fs_type != "rootfs") fstab->emplace_back(*entry);
            }
        }
    }
    return true;
}

static RemountStatus VerifyCheckpointing() {
    if (!android::base::GetBoolProperty("ro.virtual_ab.enabled", false) &&
        !android::base::GetBoolProperty("ro.virtual_ab.retrofit", false)) {
        return REMOUNT_SUCCESS;
    }

    // Virtual A/B devices can use /data as backing storage; make sure we're
    // not checkpointing.
    auto vold = GetVold();
    bool checkpointing = false;
    if (!vold->isCheckpointing(&checkpointing).isOk()) {
        LOG(ERROR) << "Could not determine checkpointing status.";
        return BINDER_ERROR;
    }
    if (checkpointing) {
        LOG(ERROR) << "Cannot use remount when a checkpoint is in progress.";
        return CHECKPOINTING;
    }
    return REMOUNT_SUCCESS;
}

static bool IsRemountable(Fstab& candidates, const FstabEntry& entry) {
    if (entry.fs_mgr_flags.vold_managed || entry.fs_mgr_flags.recovery_only ||
        entry.fs_mgr_flags.slot_select_other) {
        return false;
    }
    if (!(entry.flags & MS_RDONLY)) {
        return false;
    }
    if (entry.fs_type == "vfat") {
        return false;
    }
    if (auto candidate_entry = GetEntryForMountPoint(&candidates, entry.mount_point)) {
        return candidate_entry->fs_type == entry.fs_type;
    }
    if (GetWrappedEntry(candidates, entry)) {
        return false;
    }
    return true;
}

static Fstab::const_iterator FindPartition(const Fstab& fstab, const std::string& partition) {
    Fstab mounts;
    if (!android::fs_mgr::ReadFstabFromFile("/proc/mounts", &mounts)) {
        LOG(ERROR) << "Failed to read /proc/mounts";
        return fstab.end();
    }

    for (auto iter = fstab.begin(); iter != fstab.end(); iter++) {
        const auto mount_point = system_mount_point(*iter);
        if (partition == mount_point || partition == android::base::Basename(mount_point)) {
            // In case fstab has multiple entries, pick the one that matches the
            // actual mounted filesystem type.
            auto proc_mount_point = (iter->mount_point == "/system") ? "/" : iter->mount_point;
            auto mounted = GetEntryForMountPoint(&mounts, proc_mount_point);
            if (mounted && mounted->fs_type == iter->fs_type) {
                return iter;
            }
        }
    }
    return fstab.end();
}

static Fstab GetAllRemountablePartitions(Fstab& fstab) {
    auto candidates = fs_mgr_overlayfs_candidate_list(fstab);

    Fstab partitions;
    for (const auto& entry : fstab) {
        if (IsRemountable(candidates, entry)) {
            partitions.emplace_back(entry);
        }
    }
    return partitions;
}

static RemountStatus GetRemountList(const Fstab& fstab, const std::vector<std::string>& argv,
                                    Fstab* partitions) {
    auto candidates = fs_mgr_overlayfs_candidate_list(fstab);

    for (const auto& arg : argv) {
        std::string partition = arg;
        if (partition == "/") {
            partition = "/system";
        }

        auto it = FindPartition(fstab, partition);
        if (it == fstab.end()) {
            LOG(ERROR) << "Unknown partition " << arg;
            return UNKNOWN_PARTITION;
        }

        const FstabEntry* entry = &*it;
        if (auto wrap = GetWrappedEntry(candidates, *entry); wrap != nullptr) {
            LOG(INFO) << "partition " << arg << " covered by overlayfs for " << wrap->mount_point
                      << ", switching";
            entry = wrap;
        }

        // If it's already remounted, include it so it gets gracefully skipped
        // later on.
        if (!fs_mgr_overlayfs_already_mounted(entry->mount_point) &&
            !IsRemountable(candidates, *entry)) {
            LOG(ERROR) << "Invalid partition " << arg;
            return INVALID_PARTITION;
        }
        if (GetEntryForMountPoint(partitions, entry->mount_point) != nullptr) {
            continue;
        }
        partitions->emplace_back(*entry);
    }

    return REMOUNT_SUCCESS;
}

struct RemountCheckResult {
    bool reboot_later = false;
    bool setup_overlayfs = false;
    bool disabled_verity = false;
    bool verity_error = false;
    bool remounted_anything = false;
};

static RemountStatus CheckVerity(const FstabEntry& entry, RemountCheckResult* result) {
    if (!fs_mgr_is_verity_enabled(entry)) {
        return REMOUNT_SUCCESS;
    }

    std::unique_ptr<AvbOps, decltype(&::avb_ops_user_free)> ops(avb_ops_user_new(),
                                                                &::avb_ops_user_free);
    if (!ops) {
        return VERITY_PARTITION;
    }
    if (!avb_user_verity_set(ops.get(), fs_mgr_get_slot_suffix().c_str(), false)) {
        return VERITY_PARTITION;
    }
    result->disabled_verity = true;
    result->reboot_later = true;
    return REMOUNT_SUCCESS;
}

static RemountStatus CheckVerityAndOverlayfs(Fstab* partitions, RemountCheckResult* result) {
    RemountStatus status = REMOUNT_SUCCESS;
    for (auto it = partitions->begin(); it != partitions->end();) {
        auto& entry = *it;
        const auto& mount_point = entry.mount_point;

        if (auto rv = CheckVerity(entry, result); rv != REMOUNT_SUCCESS) {
            LOG(ERROR) << "Skipping verified partition " << mount_point << " for remount";
            status = rv;
            it = partitions->erase(it);
            continue;
        }

        if (fs_mgr_wants_overlayfs(&entry)) {
            bool want_reboot = false;
            bool force = result->disabled_verity;
            if (!fs_mgr_overlayfs_setup(mount_point.c_str(), &want_reboot, force)) {
                LOG(ERROR) << "Overlayfs setup for " << mount_point << " failed, skipping";
                status = BAD_OVERLAY;
                it = partitions->erase(it);
                continue;
            }
            if (want_reboot) {
                LOG(INFO) << "Using overlayfs for " << mount_point;
                result->reboot_later = true;
                result->setup_overlayfs = true;
            }
        }
        it++;
    }
    return status;
}

static RemountStatus EnableDsuIfNeeded() {
    auto gsid = android::gsi::GetGsiService();
    if (!gsid) {
        return REMOUNT_SUCCESS;
    }

    auto dsu_running = false;
    if (auto status = gsid->isGsiRunning(&dsu_running); !status.isOk()) {
        LOG(ERROR) << "Failed to get DSU running state: " << status;
        return BINDER_ERROR;
    }
    auto dsu_enabled = false;
    if (auto status = gsid->isGsiEnabled(&dsu_enabled); !status.isOk()) {
        LOG(ERROR) << "Failed to get DSU enabled state: " << status;
        return BINDER_ERROR;
    }
    if (dsu_running && !dsu_enabled) {
        std::string dsu_slot;
        if (auto status = gsid->getActiveDsuSlot(&dsu_slot); !status.isOk()) {
            LOG(ERROR) << "Failed to get active DSU slot: " << status;
            return BINDER_ERROR;
        }
        LOG(INFO) << "DSU is running but disabled, enable DSU so that we stay within the "
                     "DSU guest system after reboot";
        int error = 0;
        if (auto status = gsid->enableGsi(/* oneShot = */ true, dsu_slot, &error);
            !status.isOk() || error != android::gsi::IGsiService::INSTALL_OK) {
            LOG(ERROR) << "Failed to enable DSU: " << status << ", error code: " << error;
            return !status.isOk() ? BINDER_ERROR : GSID_ERROR;
        }
        LOG(INFO) << "Successfully enabled DSU (one-shot mode)";
    }
    return REMOUNT_SUCCESS;
}

static RemountStatus RemountPartition(Fstab& fstab, Fstab& mounts, FstabEntry& entry) {
    // unlock the r/o key for the mount point device
    if (entry.fs_mgr_flags.logical) {
        fs_mgr_update_logical_partition(&entry);
    }
    auto blk_device = entry.blk_device;
    auto mount_point = entry.mount_point;

    auto found = false;
    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it) {
        auto& rentry = *it;
        if (mount_point == rentry.mount_point) {
            blk_device = rentry.blk_device;
            found = true;
            break;
        }
        // Find overlayfs mount point?
        if ((mount_point == "/" && rentry.mount_point == "/system") ||
            (mount_point == "/system" && rentry.mount_point == "/")) {
            blk_device = rentry.blk_device;
            mount_point = "/system";
            found = true;
            break;
        }
    }
    if (!found) {
        PLOG(INFO) << "skip unmounted partition dev:" << blk_device << " mnt:" << mount_point;
        return REMOUNT_SUCCESS;
    }
    if (blk_device == "/dev/root") {
        auto from_fstab = GetEntryForMountPoint(&fstab, mount_point);
        if (from_fstab) blk_device = from_fstab->blk_device;
    }
    fs_mgr_set_blk_ro(blk_device, false);

    // Find system-as-root mount point?
    if ((mount_point == "/system") && !GetEntryForMountPoint(&mounts, mount_point) &&
        GetEntryForMountPoint(&mounts, "/")) {
        mount_point = "/";
    }

    // Now remount!
    if (::mount(blk_device.c_str(), mount_point.c_str(), entry.fs_type.c_str(), MS_REMOUNT,
                nullptr) == 0) {
        return REMOUNT_SUCCESS;
    }
    if ((errno == EINVAL) && (mount_point != entry.mount_point)) {
        mount_point = entry.mount_point;
        if (::mount(blk_device.c_str(), mount_point.c_str(), entry.fs_type.c_str(), MS_REMOUNT,
                    nullptr) == 0) {
            return REMOUNT_SUCCESS;
        }
    }

    PLOG(ERROR) << "failed to remount partition dev:" << blk_device << " mnt:" << mount_point;
    return REMOUNT_FAILED;
}

static int do_remount(Fstab& fstab, const std::vector<std::string>& partition_args,
                      RemountCheckResult* check_result) {
    Fstab partitions;
    if (partition_args.empty()) {
        partitions = GetAllRemountablePartitions(fstab);
    } else {
        if (auto rv = GetRemountList(fstab, partition_args, &partitions); rv != REMOUNT_SUCCESS) {
            return rv;
        }
    }

    // Check verity and optionally setup overlayfs backing.
    auto retval = CheckVerityAndOverlayfs(&partitions, check_result);

    if (partitions.empty() || check_result->disabled_verity) {
        if (partitions.empty()) {
            LOG(WARNING) << "No remountable partitions were found.";
        }
        return retval;
    }

    // Mount overlayfs.
    if (!fs_mgr_overlayfs_mount_all(&partitions)) {
        LOG(WARNING) << "Cannot mount overlayfs for some partitions";
        // Continue regardless to handle raw remount case.
    }

    // Get actual mounts _after_ overlayfs has been added.
    android::fs_mgr::Fstab mounts;
    if (!android::fs_mgr::ReadFstabFromFile("/proc/mounts", &mounts) || mounts.empty()) {
        PLOG(ERROR) << "Failed to read /proc/mounts";
        return NO_MOUNTS;
    }

    // Remount selected partitions.
    for (auto& entry : partitions) {
        if (auto rv = RemountPartition(fstab, mounts, entry); rv != REMOUNT_SUCCESS) {
            retval = rv;
        } else {
            check_result->remounted_anything = true;
        }
    }
    return retval;
}

int main(int argc, char* argv[]) {
    // Do not use MyLogger() when running as clean_scratch_files, as stdout/stderr of daemon process
    // are discarded.
    if (argc > 0 && android::base::Basename(argv[0]) == "clean_scratch_files"s) {
        android::fs_mgr::CleanupOldScratchFiles();
        return 0;
    }

    android::base::InitLogging(argv, MyLogger(false /* verbose */));

    const char* fstab_file = nullptr;
    bool auto_reboot = false;
    bool verbose = false;
    std::vector<std::string> partition_args;

    struct option longopts[] = {
            {"fstab", required_argument, nullptr, 'T'},
            {"help", no_argument, nullptr, 'h'},
            {"reboot", no_argument, nullptr, 'R'},
            {"verbose", no_argument, nullptr, 'v'},
            {0, 0, nullptr, 0},
    };
    for (int opt; (opt = ::getopt_long(argc, argv, "hRT:v", longopts, nullptr)) != -1;) {
        switch (opt) {
            case 'h':
                usage();
                return 0;
            case 'R':
                auto_reboot = true;
                break;
            case 'T':
                if (fstab_file) {
                    LOG(ERROR) << "Cannot supply two fstabs: -T " << fstab_file << " -T " << optarg;
                    usage();
                    return 1;
                }
                fstab_file = optarg;
                break;
            case 'v':
                verbose = true;
                break;
            default:
                LOG(ERROR) << "Bad argument -" << char(opt);
                usage();
                return 1;
        }
    }

    if (verbose) {
        android::base::SetLogger(MyLogger(verbose));
    }

    for (; argc > optind; ++optind) {
        partition_args.emplace_back(argv[optind]);
    }

    // Make sure we are root.
    if (::getuid() != 0) {
        LOG(ERROR) << "Not running as root. Try \"adb root\" first.";
        return 1;
    }

    // If somehow this executable is delivered on a "user" build, it can
    // not function, so providing a clear message to the caller rather than
    // letting if fall through and provide a lot of confusing failure messages.
    if (!ALLOW_ADBD_DISABLE_VERITY || !android::base::GetBoolProperty("ro.debuggable", false)) {
        LOG(ERROR) << "Device must be userdebug build";
        return 1;
    }

    if (android::base::GetProperty("ro.boot.vbmeta.device_state", "") == "locked") {
        LOG(ERROR) << "Device must be bootloader unlocked";
        return 1;
    }

    // Make sure checkpointing is disabled if necessary.
    if (auto rv = VerifyCheckpointing(); rv != REMOUNT_SUCCESS) {
        return rv;
    }

    // Read the selected fstab.
    Fstab fstab;
    if (!ReadFstab(fstab_file, &fstab) || fstab.empty()) {
        PLOG(ERROR) << "Failed to read fstab";
        return 1;
    }

    RemountCheckResult check_result;
    int result = do_remount(fstab, partition_args, &check_result);

    if (check_result.disabled_verity && check_result.setup_overlayfs) {
        LOG(INFO) << "Verity disabled; overlayfs enabled.";
    } else if (check_result.disabled_verity) {
        LOG(INFO) << "Verity disabled.";
    } else if (check_result.setup_overlayfs) {
        LOG(INFO) << "Overlayfs enabled.";
    }
    if (result == REMOUNT_SUCCESS) {
        LOG(INFO) << "remount succeeded";
    } else {
        LOG(ERROR) << "remount failed";
    }
    if (check_result.reboot_later) {
        if (auto_reboot) {
            // If (1) remount requires a reboot to take effect, (2) system is currently
            // running a DSU guest and (3) DSU is disabled, then enable DSU so that the
            // next reboot would not take us back to the host system but stay within
            // the guest system.
            if (auto rv = EnableDsuIfNeeded(); rv != REMOUNT_SUCCESS) {
                LOG(ERROR) << "Unable to automatically enable DSU";
                return rv;
            }
            reboot("remount");
        } else {
            LOG(INFO) << "Now reboot your device for settings to take effect";
        }
        return REMOUNT_SUCCESS;
    }
    return result;
}
