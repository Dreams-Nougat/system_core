/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef NATIVE_BRIDGE_H_
#define NATIVE_BRIDGE_H_

#include <stdint.h>

namespace android {

// Runtime interfaces to native bridge.
struct NativeBridgeRuntimeCallbacks;

// Initialize the native bridge, if any. Should be called by Runtime::Init().
// A null library filename signals that we do not want to load a native bridge.
void SetupNativeBridge(const char* native_bridge_library_filename,
                       const NativeBridgeRuntimeCallbacks* runtime_callbacks);

// Load a shared library that is supported by the native-bridge.
void* NativeBridgeLoadLibrary(const char* libpath, int flag);

// Get a native-bridge trampoline for specified native method.
void* NativeBridgeGetTrampoline(void* handle, const char* name, const char* shorty, uint32_t len);

// True if native library is valid and is for an ABI that is supported by native-bridge.
bool NativeBridgeIsSupported(const char* libpath);

};  // namespace android

#endif  // NATIVE_BRIDGE_H_
