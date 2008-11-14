# Copyright (C) 2007 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../mkbootimg
LOCAL_SRC_FILES := protocol.c engine.c bootimg.c fastboot.c 
LOCAL_MODULE := fastboot

ifeq ($(HOST_OS),linux)
  LOCAL_SRC_FILES += usb_linux.c util_linux.c
endif

ifeq ($(HOST_OS),freebsd)
  LOCAL_SRC_FILES += usb_freebsd.c util_freebsd.c
endif

ifeq ($(HOST_OS),darwin)
  LOCAL_SRC_FILES += usb_osx.c util_osx.c
  LOCAL_LDLIBS += -lpthread -framework CoreFoundation -framework IOKit \
	-framework Carbon
endif

ifeq ($(HOST_OS),windows)
  LOCAL_SRC_FILES += usb_windows.c util_windows.c
  EXTRA_STATIC_LIBS := AdbWinApi
  LOCAL_C_INCLUDES += /usr/include/w32api/ddk development/host/windows/usb/api
  ifeq ($(strip $(USE_CYGWIN)),)
    LOCAL_LDLIBS += -lws2_32
    USE_SYSDEPS_WIN32 := 1
  endif
endif

LOCAL_STATIC_LIBRARIES := $(EXTRA_STATIC_LIBS) libzipfile libunz

include $(BUILD_HOST_EXECUTABLE)
$(call dist-for-goals,droid,$(LOCAL_BUILT_MODULE))

ifeq ($(HOST_OS),linux)
include $(CLEAR_VARS)
LOCAL_SRC_FILES := usbtest.c usb_linux.c
LOCAL_MODULE := usbtest
include $(BUILD_HOST_EXECUTABLE)
endif

ifeq ($(HOST_OS),windows)
$(LOCAL_INSTALLED_MODULE): $(HOST_OUT_EXECUTABLES)/AdbWinApi.dll
endif
