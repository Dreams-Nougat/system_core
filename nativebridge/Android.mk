LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
  native_bridge.cc

LOCAL_MODULE:= nativebridge
LOCAL_CPP_EXTENSION := .cc

LOCAL_CFLAGS := -Werror
LOCAL_CPPFLAGS := -std=gnu++11
LOCAL_LDFLAGS := -ldl

ifeq (,$(TARGET_BUILD_APPS))
include external/libcxx/libcxx.mk
else
include external/stlport/libstlport.mk
endif

LOCAL_MULTILIB := both

include $(BUILD_SHARED_LIBRARY)
