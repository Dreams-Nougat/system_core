# Copyright (C) 2015 The Android Open Source Project
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

LOCAL_PATH := $(call my-dir)

metrics_cpp_extension := .cc
libmetrics_sources := \
  c_metrics_library.cc \
  metrics_library.cc \
  serialization/metric_sample.cc \
  serialization/serialization_utils.cc

metrics_client_sources := \
  metrics_client.cc

metrics_daemon_sources := \
  metrics_daemon.cc \
  metrics_daemon_main.cc \
  persistent_integer.cc \
  uploader/metrics_hashes.cc \
  uploader/metrics_log_base.cc \
  uploader/metrics_log.cc \
  uploader/sender_http.cc \
  uploader/system_profile_android.cc \
  uploader/upload_service.cc \
  serialization/metric_sample.cc \
  serialization/serialization_utils.cc

metrics_CFLAGS := -Wall -D__BRILLO__ \
  -Wno-char-subscripts -Wno-missing-field-initializers \
  -Wno-unused-function -Wno-unused-parameter -Werror -fvisibility=default
metrics_CPPFLAGS := -Wno-non-virtual-dtor -Wno-sign-promo \
  -Wno-strict-aliasing -fvisibility=default
metrics_includes := external/gtest/include \
  $(LOCAL_PATH)/include
metrics_shared_libraries := libchrome libchromeos

# Shared library for metrics.
# ========================================================
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := $(metrics_cpp_extension)
LOCAL_MODULE := libmetrics
LOCAL_SRC_FILES := $(libmetrics_sources)
LOCAL_C_INCLUDES := $(metrics_includes)
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/include
LOCAL_SHARED_LIBRARIES := $(metrics_shared_libraries)
LOCAL_CFLAGS := $(metrics_CFLAGS)
LOCAL_CPPFLAGS := $(metrics_CPPFLAGS)
include $(BUILD_SHARED_LIBRARY)

# CLI client for metrics.
# ========================================================
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := $(metrics_cpp_extension)
LOCAL_MODULE := metrics_client
LOCAL_SRC_FILES := $(metrics_client_sources)
LOCAL_C_INCLUDES := $(metrics_includes)
LOCAL_SHARED_LIBRARIES := $(metrics_shared_libraries) libmetrics
LOCAL_CFLAGS := $(metrics_CFLAGS)
LOCAL_CPPFLAGS := $(metrics_CPPFLAGS)
include $(BUILD_EXECUTABLE)

# Protobuf library for metrics_daemon.
# ========================================================
include $(CLEAR_VARS)
LOCAL_MODULE := metrics_daemon_protos
LOCAL_SRC_FILES :=  $(call all-proto-files-under,uploader/proto)
LOCAL_MODULE_CLASS := STATIC_LIBRARIES
generated_sources_dir := $(call local-generated-sources-dir)
LOCAL_STATIC_LIBRARIES := libprotobuf-cpp-lite
LOCAL_EXPORT_C_INCLUDE_DIRS += \
    $(generated_sources_dir)/proto/system/core/metrics
include $(BUILD_STATIC_LIBRARY)

# metrics daemon.
# ========================================================
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := $(metrics_cpp_extension)
LOCAL_MODULE := metrics_daemon
LOCAL_SRC_FILES := $(metrics_daemon_sources)
LOCAL_C_INCLUDES := $(metrics_includes) external/libchromeos
LOCAL_SHARED_LIBRARIES := $(metrics_shared_libraries) libmetrics \
  libprotobuf-cpp-lite libchromeos-http libchromeos-dbus libdbus
LOCAL_STATIC_LIBRARIES := metrics_daemon_protos
LOCAL_CFLAGS := $(metrics_CFLAGS)
LOCAL_CPPFLAGS := $(metrics_CPPFLAGS)
LOCAL_RTTI_FLAG := -frtti
include $(BUILD_EXECUTABLE)
