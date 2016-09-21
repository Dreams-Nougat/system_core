#
# Copyright (C) 2016 The Android Open Source Project
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
#

LOCAL_PATH:= $(call my-dir)

android_unwind_cflags := \
    -Wall \
    -Werror \
    -O0 \
    -g \

android_unwind_src_files := \
    ArmUnwind.cpp \
    DwarfCfa.cpp \
    DwarfOp.cpp \
    Elf.cpp \
    ElfInterface.cpp \
    ElfInterfaceArm.cpp \
    Memory.cpp \

android_unwind_shared_libraries := \
    libbase \
    liblog \

android_unwind_test_libraries := \
    libandroid_unwind \
    libbase \

android_unwind_test_src_files := \
    tests/ArmUnwindDecodeTest.cpp \
    tests/ArmUnwindExtractTest.cpp \
    tests/ElfInterfaceArmTest.cpp \
    tests/ElfInterfaceTest.cpp \
    tests/ElfTest.cpp \
    tests/LogFake.cpp \
    tests/MemoryTest.cpp \
    tests/MemoryFake.cpp \

#    tests/DwarfOpTest.cpp \

include $(CLEAR_VARS)

LOCAL_MODULE := libandroid_unwind
LOCAL_SRC_FILES := $(android_unwind_src_files) Log.cpp
LOCAL_CFLAGS := $(android_unwind_cflags)
LOCAL_SHARED_LIBRARIES := $(android_unwind_shared_libraries)

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE := libandroid_unwind_test
LOCAL_SRC_FILES := $(android_unwind_test_src_files) Log.cpp
LOCAL_CFLAGS := $(android_unwind_test_cflags)
LOCAL_SHARED_LIBRARIES := $(android_unwind_test_libraries)
LOCAL_MODULE := libandroid_unwind_test

include $(BUILD_NATIVE_TEST)


include $(CLEAR_VARS)

LOCAL_MODULE := libandroid_unwind
LOCAL_SRC_FILES := $(android_unwind_src_files)
LOCAL_CFLAGS := $(android_unwind_cflags)
LOCAL_SHARED_LIBRARIES := $(android_unwind_shared_libraries)

include $(BUILD_HOST_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE := libandroid_unwind_test
LOCAL_SRC_FILES := $(android_unwind_test_src_files) Log.cpp
LOCAL_CFLAGS := $(android_unwind_test_cflags)
LOCAL_STATIC_LIBRARIES := $(android_unwind_test_libraries)

include $(BUILD_HOST_NATIVE_TEST)

include $(CLEAR_VARS)

LOCAL_MODULE := unwind_info
LOCAL_SRC_FILES := $(android_unwind_src_files) \
    LogPrint.cpp \
	unwind_info.cpp \

LOCAL_CFLAGS := $(android_unwind_cflags)
LOCAL_STATIC_LIBRARIES := \
    libbase \
    liblog \

include $(BUILD_HOST_EXECUTABLE)
