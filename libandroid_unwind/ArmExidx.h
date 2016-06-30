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

#ifndef _LIBANDROID_UNWIND_ARM_EXIDX_H
#define _LIBANDROID_UNWIND_ARM_EXIDX_H

#include <stdint.h>

#include <deque>

#include "Memory.h"
#include "Regs.h"

enum ArmStatus : size_t {
  ARM_STATUS_NONE = 0,
  ARM_STATUS_NO_UNWIND,
  ARM_STATUS_FINISH,
  ARM_STATUS_RESERVED,
  ARM_STATUS_SPARE,
  ARM_STATUS_TRUNCATED,
  ARM_STATUS_READ_FAILED,
  ARM_STATUS_MALFORMED,
  ARM_STATUS_INVALID_PERSONALITY,
};

enum ArmOp : uint8_t {
  ARM_OP_FINISH = 0xb0,
};

class ArmExidx {
 public:
  ArmExidx(Regs32* regs, Memory* elf_memory, Memory* process_memory)
      : regs_(regs), elf_memory_(elf_memory), process_memory_(process_memory) {}
  virtual ~ArmExidx() {}

  bool ExtractEntry(uint32_t entry);

  bool Eval();

  bool Decode();

  std::deque<uint8_t>* data() { return &data_; }

  ArmStatus status() { return status_; }

  Regs32* regs() { return regs_; }

  uint32_t cfa() { return cfa_; }
  void set_cfa(uint32_t cfa) { cfa_ = cfa; }

 private:
  bool GetByte(uint8_t* byte);

  bool DecodePrefix2_0(uint8_t byte);
  bool DecodePrefix2_1(uint8_t byte);
  bool DecodePrefix2_2(uint8_t byte);
  bool DecodePrefix2_3(uint8_t byte);
  bool DecodePrefix2(uint8_t byte);

  bool DecodePrefix3_0(uint8_t byte);
  bool DecodePrefix3_1(uint8_t byte);
  bool DecodePrefix3_2(uint8_t byte);
  bool DecodePrefix3(uint8_t byte);

  Regs32* regs_ = nullptr;
  uint32_t cfa_ = 0;
  std::deque<uint8_t> data_;
  ArmStatus status_ = ARM_STATUS_NONE;

  Memory* elf_memory_;
  Memory* process_memory_;
};

#endif  // _LIBANDROID_UNWIND_ARM_EXIDX_H
