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

#ifndef _LIBANDROID_UNWIND_ARM_UNWIND_H
#define _LIBANDROID_UNWIND_ARM_UNWIND_H

#include <stdint.h>

#include <deque>

#include "Arm.h"
#include "Memory.h"

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

class ArmUnwind {
 public:
  ArmUnwind(const StateArm& state, Memory* memory) : state_(state), memory_(memory) {}
  virtual ~ArmUnwind() {}

  bool ExtractEntry(arm_ptr_t entry);

  bool Decode();

  // Exposed for testing purposes.
  const StateArm& state() { return state_; }
  std::deque<uint8_t>* data() { return &data_; }

  void set_debug(bool debug) { debug_ = debug; }
  ArmStatus status() { return status_; }

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

  StateArm state_;
  std::deque<uint8_t> data_;
  bool debug_ = false;
  ArmStatus status_ = ARM_STATUS_NONE;

  Memory* memory_;
};

#endif  // _LIBANDROID_UNWIND_ARM_UNWIND_H
