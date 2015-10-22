/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <windows.h>

#include "base/u8.h"

#include <fcntl.h>

#include <string>

namespace android {
namespace base {

bool WideToUTF8(const wchar_t* utf16, const size_t size, std::string* utf8) {
  utf8->clear();

  if (size == 0) {
    return true;
  }

  // TODO: Consider using std::wstring_convert once libcxx is supported on
  // Windows. Or, consider libutils/Unicode.cpp.

  // Only Vista or later has this flag that causes WideCharToMultiByte() to
  // return an error on invalid characters.
  const DWORD flags =
#if (WINVER >= 0x0600)
    WC_ERR_INVALID_CHARS;
#else
    0;
#endif

  const int chars_required = WideCharToMultiByte(CP_UTF8, flags, utf16, size,
                                                 NULL, 0, NULL, NULL);
  if (chars_required <= 0) {
    return false;
  }

  // This could potentially throw a std::bad_alloc exception.
  utf8->resize(chars_required);

  const int result = WideCharToMultiByte(CP_UTF8, flags, utf16, size,
                                         &(*utf8)[0], chars_required, NULL,
                                         NULL);
  if (result != chars_required) {
    utf8->clear();
    return false;
  }

  return true;
}

bool WideToUTF8(const wchar_t* utf16, std::string* utf8) {
  // Compute string length of NULL-terminated string with wcslen().
  return WideToUTF8(utf16, wcslen(utf16), utf8);
}

bool WideToUTF8(const std::wstring& utf16, std::string* utf8) {
  // Use the stored length of the string which allows embedded NULL characters
  // to be converted.
  return WideToUTF8(utf16.c_str(), utf16.length(), utf8);
}

bool UTF8ToWide(const char* utf8, const size_t size, std::wstring* utf16) {
  utf16->clear();

  if (size == 0) {
    return true;
  }

  // TODO: Consider using std::wstring_convert once libcxx is supported on
  // Windows. Or, consider libutils/Unicode.cpp.
  const int chars_required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                 utf8, size, NULL, 0);
  if (chars_required <= 0) {
    return false;
  }

  // This could potentially throw a std::bad_alloc exception.
  utf16->resize(chars_required);

  const int result = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8,
                                         size, &(*utf16)[0], chars_required);
  if (result != chars_required) {
    utf16->clear();
    return false;
  }

  return true;
}

bool UTF8ToWide(const char* utf8, std::wstring* utf16) {
  // Compute string length of NULL-terminated string with strlen().
  return UTF8ToWide(utf8, strlen(utf8), utf16);
}

bool UTF8ToWide(const std::string& utf8, std::wstring* utf16) {
  // Use the stored length of the string which allows embedded NULL characters
  // to be converted.
  return UTF8ToWide(utf8.c_str(), utf8.length(), utf16);
}

// Versions of APIs that support UTF-8 paths.
namespace u8 {

int open(const char* name, int flags, ...) {
  std::wstring name_utf16;
  if (!UTF8ToWide(name, &name_utf16)) {
    errno = EINVAL;
    return -1;
  }

  int mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }

  return _wopen(name_utf16.c_str(), flags, mode);
}

int unlink(const char* name) {
  std::wstring name_utf16;
  if (!UTF8ToWide(name, &name_utf16)) {
    errno = EINVAL;
    return -1;
  }

  return _wunlink(name_utf16.c_str());
}

}  // namespace u8
}  // namespace base
}  // namespace android
