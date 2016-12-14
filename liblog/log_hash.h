/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef _LIBLOG_LOG_HASH_H__
#define _LIBLOG_LOG_HASH_H__

#include <log/uio.h>

#include "log_portability.h"

#ifdef __cplusplus
extern "C" {
#endif

LIBLOG_ABI_PRIVATE size_t __android_log_hash(struct iovec* vecs, int count);

#ifdef __cplusplus
}
#endif

#endif
