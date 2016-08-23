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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "MemoryFake.h"

void MemoryFake::SetData(uint64_t addr, uint32_t value) {
  if (addr & 0x3) {
    printf("Addr is not aligned to a 32 bit boundary: 0x%" PRIx64 "\n", addr);
    abort();
  }
  if (data_.find(addr) != data_.end()) {
    printf("Attempt to insert a second value at 0x%" PRIx64 "\n", addr);
    abort();
  }
  data_.insert({addr, value});
}

void MemoryFake::OverwriteData(uint64_t addr, uint32_t value) {
  if (addr & 0x3) {
    printf("Addr is not aligned to a 32 bit boundary: 0x%" PRIx64 "\n", addr);
    abort();
  }
  auto entry = data_.find(addr);
  if (entry == data_.end()) {
    printf("Attempt to overwrite data not already present at 0x%" PRIx64 "\n", addr);
    abort();
  }
  entry->second = value;
}


bool MemoryFake::Read(uint64_t addr, void* memory, size_t size) {
  uint8_t* buffer = reinterpret_cast<uint8_t*>(memory);
  uint64_t aligned_addr = addr & ~0x3;

  auto value = data_.find(aligned_addr);
  if (value == data_.end()) {
    return false;
  }
  size_t align_bytes = 4 - (addr & 0x3);
  size_t read_bytes = (size > align_bytes) ? align_bytes : size;
  uint8_t* src = reinterpret_cast<uint8_t*>(&value->second);
  memcpy(buffer, &src[4 - align_bytes], read_bytes);
  size -= read_bytes;
  buffer += read_bytes;
  aligned_addr += 4;

  while (size >= 4) {
    value = data_.find(aligned_addr);
    if (value == data_.end()) {
      return false;
    }
    memcpy(buffer, &value->second, align_bytes);
    size -= 4;
    buffer += 4;
    aligned_addr += 4;
  }

  if (size) {
    value = data_.find(aligned_addr);
    if (value == data_.end()) {
      return false;
    }
    memcpy(buffer, &value->second, size);
  }
  return true;
}
