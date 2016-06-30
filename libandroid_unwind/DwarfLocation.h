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

#ifndef _LIBANDROID_UNWIND_DWARF_LOCATION_H
#define _LIBANDROID_UNWIND_DWARF_LOCATION_H

#include <stdint.h>

enum DwarfLocationEnum : uint8_t {
  DWARF_LOCATION_UNDEFINED = 0,
  DWARF_LOCATION_SAME,
  DWARF_LOCATION_OFFSET,
  DWARF_LOCATION_VAL_OFFSET,
  DWARF_LOCATION_REGISTER,
  DWARF_LOCATION_EXPRESSION,
  DWARF_LOCATION_VAL_EXPRESSION,
};

struct DwarfLocation {
  DwarfLocationEnum type;
  uint64_t value;
};

#endif  // _LIBANDROID_UNWIND_DWARF_LOCATION_H
