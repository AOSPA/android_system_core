# Copyright 2013 The Android Open Source Project

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_SRC_FILES := healthd_board_default.cpp
LOCAL_MODULE := libhealthd.default
LOCAL_CFLAGS := -Werror
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	healthd.cpp \
	healthd_mode_android.cpp \
	healthd_mode_charger.cpp \
	BatteryMonitor.cpp \
	BatteryPropertiesRegistrar.cpp

LOCAL_MODULE := healthd
LOCAL_MODULE_TAGS := optional
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT_SBIN)
LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_SBIN_UNSTRIPPED)

LOCAL_CFLAGS := -D__STDC_LIMIT_MACROS -Werror

ifeq ($(strip $(BOARD_CHARGER_DISABLE_INIT_BLANK)),true)
LOCAL_CFLAGS += -DCHARGER_DISABLE_INIT_BLANK
endif

ifeq ($(strip $(BOARD_CHARGER_ENABLE_SUSPEND)),true)
LOCAL_CFLAGS += -DCHARGER_ENABLE_SUSPEND
endif

LOCAL_C_INCLUDES := bootable/recovery

LOCAL_STATIC_LIBRARIES := libbatteryservice libbinder libminui libpng libz libutils libstdc++ libcutils liblog libm libc

ifeq ($(strip $(BOARD_CHARGER_ENABLE_SUSPEND)),true)
LOCAL_STATIC_LIBRARIES += libsuspend
endif

LOCAL_HAL_STATIC_LIBRARIES := libhealthd

# Symlink /charger to /sbin/healthd
LOCAL_POST_INSTALL_CMD := $(hide) mkdir -p $(TARGET_ROOT_OUT) \
    && ln -sf /sbin/healthd $(TARGET_ROOT_OUT)/charger

include $(BUILD_EXECUTABLE)

ifeq ($(strip $(BOARD_CHARGER_FASHION)),true)
include $(CLEAR_VARS)
LOCAL_SRC_FILES := healthd_board_fashion.cpp
LOCAL_MODULE := libhealthd.fashion
LOCAL_CFLAGS := -Werror
include $(BUILD_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := font_log.png
LOCAL_SRC_FILES := fonts/$(PRODUCT_AAPT_PREF_CONFIG)/font_log.png
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)/res/images
include $(BUILD_PREBUILT)
endif

define _add-charger-image
include $$(CLEAR_VARS)
LOCAL_MODULE := system_core_charger_$(notdir $(1))
LOCAL_MODULE_STEM := $(notdir $(1))
_img_modules += $$(LOCAL_MODULE)
LOCAL_SRC_FILES := $1
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $$(TARGET_ROOT_OUT)/res/images/charger
include $$(BUILD_PREBUILT)
endef

_img_modules :=
ifeq ($(strip $(BOARD_HEALTHD_CUSTOM_CHARGER_RES)),)
ifeq ($(strip $(BOARD_CHARGER_FASHION)),true)
IMAGES_DIR := images/$(PRODUCT_AAPT_PREF_CONFIG)
else
IMAGES_DIR := images/legacy
endif
else
IMAGES_DIR := ../../../$(BOARD_HEALTHD_CUSTOM_CHARGER_RES)
endif
_images :=
$(foreach _img, $(call find-subdir-subdir-files, "$(IMAGES_DIR)", "*.png"), \
  $(eval $(call _add-charger-image,$(_img))))

include $(CLEAR_VARS)
LOCAL_MODULE := charger_res_images
LOCAL_MODULE_TAGS := optional
LOCAL_REQUIRED_MODULES := $(_img_modules)
include $(BUILD_PHONY_PACKAGE)

_add-charger-image :=
_img_modules :=
