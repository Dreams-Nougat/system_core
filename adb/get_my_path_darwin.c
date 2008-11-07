/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <utils/executablepath.h>
#import <Carbon/Carbon.h>
#include <unistd.h>

void get_my_path(char *s, size_t maxLen)
{
    ProcessSerialNumber psn;
    GetCurrentProcess(&psn);
    CFDictionaryRef dict;
    dict = ProcessInformationCopyDictionary(&psn, 0xffffffff);
    CFStringRef value = (CFStringRef)CFDictionaryGetValue(dict,
                CFSTR("CFBundleExecutable"));
    CFStringGetCString(value, s, maxLen, kCFStringEncodingUTF8);
}

