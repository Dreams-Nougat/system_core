LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := sdcard.c
LOCAL_MODULE := sdcard
LOCAL_CFLAGS := -Wall -Wno-unused-parameter -Werror
LOCAL_C_INCLUDES := frameworks/base/libs/packagelistparser
LOCAL_SHARED_LIBRARIES := libcutils libpackagelistparser

include $(BUILD_EXECUTABLE)
