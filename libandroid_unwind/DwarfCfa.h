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

#ifndef _LIBANDROID_UNWIND_DWARF_CFA_H
#define _LIBANDROID_UNWIND_DWARF_CFA_H

#include <stdint.h>
#include <inttypes.h>

#include <memory>
#include <stack>
#include <string>
#include <vector>
#include <unordered_map>

#include <android-base/stringprintf.h>

#include "DwarfError.h"
#include "DwarfLocation.h"
#include "DwarfMemory.h"
#include "DwarfStructs.h"
#include "Memory.h"
#include "Log.h"

enum DwarfCfaDisplayType : uint8_t {
  DWARF_DISPLAY_REGISTER,
  DWARF_DISPLAY_NUMBER,
  DWARF_DISPLAY_SIGNED_NUMBER,
  DWARF_DISPLAY_EVAL_BLOCK,
  DWARF_DISPLAY_ADDRESS,
};

struct DwarfCfaLogInfo {
  const char* name;
  uint8_t operands[2];
};
extern const DwarfCfaLogInfo g_cfa_info[64];

template <typename AddressType>
class DwarfCfa {
 public:
  DwarfCfa(DwarfMemory<AddressType>* memory, DwarfCIE* cie, DwarfFDE* fde)
    : memory_(memory), cie_(cie), fde_(fde) {}
  virtual ~DwarfCfa() = default;

  bool Eval(uint64_t pc, uint64_t start_offset, uint64_t end_offset) {
    if (cie_regs_) {
      regs_ = *cie_regs_;
    } else {
      regs_.clear();
    }
    last_error_ = DWARF_ERROR_NONE;

    memory_->set_cur_offset(start_offset);
    uint64_t cfa_offset;
    cur_pc_ = fde_->start_pc;
    while ((cfa_offset = memory_->cur_offset()) < end_offset && cur_pc_ <= pc) {
      operands_.clear();
      // Read the cfa information.
      uint8_t cfa_value;
      if (!memory_->ReadBytes(&cfa_value, 1)) {
        last_error_ = DWARF_ERROR_MEMORY_INVALID;
        return false;
      }
      // Check the 2 high bits.
      uint8_t cfa_low = cfa_value & 0x3f;
      switch (cfa_value >> 6) {
      case 1:
        if (g_LoggingFlags & LOGGING_FLAG_ENABLE_OP) {
          log("Raw Data: 0x%02x", cfa_value);
          log("DW_CFA_advance_loc %d", cfa_low);
        }
        cur_pc_ += cfa_low * cie_->code_alignment_factor;
        break;
      case 2:
      {
        uint64_t offset;
        if (!memory_->ReadULEB128(&offset)) {
          last_error_ = DWARF_ERROR_MEMORY_INVALID;
          return false;
        }
        if (g_LoggingFlags & LOGGING_FLAG_ENABLE_OP) {
          uint64_t cur_offset = memory_->cur_offset();
          memory_->set_cur_offset(cfa_offset);

          std::string raw_data = "Raw Data:";
          for (uint64_t i = cfa_offset; i < cur_offset; i++) {
            uint8_t value;
            if (!memory_->ReadBytes(&value, 1)) {
              last_error_ = DWARF_ERROR_MEMORY_INVALID;
              return false;
            }
            raw_data += android::base::StringPrintf(" 0x%02x", value);
          }
          log("%s", raw_data.c_str());
          log("DW_CFA_offset register(%d) %" PRId64, cfa_low, offset);
        }
        regs_[cfa_low] = { .type = DWARF_LOCATION_OFFSET, .value = offset * cie_->data_alignment_factor };
        break;
      }
      case 3:
      {
        if (g_LoggingFlags & LOGGING_FLAG_ENABLE_OP) {
          log("Raw Data: 0x%02x", cfa_value);
          log("DW_CFA_restore register(%d)", cfa_low);
        }
        if (cie_regs_ == nullptr) {
          log("restore while processing cie");
          last_error_ = DWARF_ERROR_ILLEGAL_STATE;
          return false;
        }

        auto reg_entry = cie_regs_->find(cfa_low);
        if (reg_entry == cie_regs_->end()) {
          regs_.erase(cfa_low);
        } else {
          regs_[cfa_low] = reg_entry->second;
        }
        break;
      }
      case 0:
        {
          const auto* cfa = &kCallbackTable[cfa_low];
          if (cfa->handle_func == nullptr) {
            if (g_LoggingFlags & LOGGING_FLAG_ENABLE_OP) {
              log("Raw Data: 0x%02x", cfa_value);
              log("Illegal");
            }
            last_error_ = DWARF_ERROR_ILLEGAL_VALUE;
            return false;
          }

          std::string log_string;
          if (g_LoggingFlags & LOGGING_FLAG_ENABLE_OP) {
            log_string = g_cfa_info[cfa_low].name;
          }

          for (size_t i = 0; i < cfa->num_operands; i++) {
            if (cfa->operands[i] == DW_EH_PE_block) {
              uint64_t block_length;
              if (!memory_->ReadULEB128(&block_length)) {
                last_error_ = DWARF_ERROR_MEMORY_INVALID;
                return false;
              }
              operands_.push_back(block_length);
              if (g_LoggingFlags & LOGGING_FLAG_ENABLE_OP) {
                log_string += " " + std::to_string(block_length);
              }
              memory_->set_cur_offset(memory_->cur_offset() + block_length);
              continue;
            }
            uint64_t value;
            if (!memory_->ReadEncodedValue(cfa->operands[i], &value)) {
              last_error_ = DWARF_ERROR_MEMORY_INVALID;
              return false;
            }
            if (g_LoggingFlags & LOGGING_FLAG_ENABLE_OP) {
              switch (g_cfa_info[cfa_low].operands[i]) {
              case DWARF_DISPLAY_REGISTER:
                log_string += " register(" + std::to_string(value) + ")";
                break;
              case DWARF_DISPLAY_SIGNED_NUMBER:
                log_string += " ";
                if (sizeof(AddressType) == 4) {
                  log_string += std::to_string(static_cast<int32_t>(value));
                } else {
                  log_string += std::to_string(static_cast<int64_t>(value));
                }
                break;
              case DWARF_DISPLAY_NUMBER:
                log_string += " " + std::to_string(value);
                break;
              case DWARF_DISPLAY_ADDRESS:
                if (sizeof(AddressType) == 4) {
                  log_string += android::base::StringPrintf(" 0x%" PRIx32,
                                                            static_cast<uint32_t>(value));
                } else {
                  log_string += android::base::StringPrintf(" 0x%" PRIx64,
                                                            static_cast<uint64_t>(value));
                }
                break;
              }
            }
            operands_.push_back(value);
          }

          if (g_LoggingFlags & LOGGING_FLAG_ENABLE_OP) {
            uint64_t cur_offset = memory_->cur_offset();

            std::string raw_data = "Raw Data:";
            memory_->set_cur_offset(cfa_offset);
            for (uint64_t i = 0; i < cur_offset - cfa_offset; i++) {
              uint8_t value;
              if (!memory_->ReadBytes(&value, 1)) {
                last_error_ = DWARF_ERROR_MEMORY_INVALID;
                return false;
              }

              if ((i % 10) == 0 && i != 0) {
                log("%s", raw_data.c_str());
                raw_data = "Raw Data:";
              }
              raw_data += android::base::StringPrintf(" 0x%02x", value);
            }
            if (((cur_offset - cfa_offset) % 10) != 0) {
              log("%s", raw_data.c_str());
            }
            log("%s", log_string.c_str());
          }
          if (!cfa->handle_func(this)) {
            return false;
          }
        }
        break;
      }
    }
    return true;
  }

  DwarfError last_error() { return last_error_; }
  AddressType cur_pc() { return cur_pc_; }

  const DwarfLocation& cfa_location() { return cfa_location_; }
  AddressType cfa_offset() { return cfa_offset_; }

  void set_cfa_location(DwarfLocation location) { cfa_location_ = location; }
  void set_cfa_offset(AddressType offset) { cfa_offset_ = offset; }

  const dwarf_regs_t& regs() { return regs_; }

  void set_cie_regs(const dwarf_regs_t* cie_regs) { cie_regs_ = cie_regs; }

 private:
  DwarfError last_error_;
  DwarfMemory<AddressType>* memory_;
  DwarfCIE* cie_;
  DwarfFDE* fde_;

  AddressType cur_pc_;
  DwarfLocation cfa_location_ = { .type = DWARF_LOCATION_UNDEFINED, .value = 0 };
  AddressType cfa_offset_ = 0;
  dwarf_regs_t regs_;
  const dwarf_regs_t* cie_regs_ = nullptr;
  std::vector<AddressType> operands_;
  std::stack<dwarf_regs_t> reg_state_;

  // Static data.
  static bool cfa_nop(void*) {
    return true;
  }

  static bool cfa_set_loc(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType cur_pc = cfa->cur_pc_;
    AddressType new_pc = cfa->operands_[0];
    if (new_pc < cur_pc) {
      if (sizeof(AddressType) == 4) {
        log("Warning: PC is moving backwards: old 0x%" PRIx32 " new 0x%" PRIx32, cur_pc, new_pc);
      } else {
        log("Warning: PC is moving backwards: old 0x%" PRIx64 " new 0x%" PRIx64, cur_pc, new_pc);
      }
    }
    cfa->cur_pc_ = new_pc;
    return true;
  }

  static bool cfa_advance_loc(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    cfa->cur_pc_ += cfa->operands_[0] * cfa->cie_->code_alignment_factor;
    return true;
  }

  static bool cfa_offset(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    cfa->regs_[reg] = { .type = DWARF_LOCATION_OFFSET, .value = cfa->operands_[1] };
    return true;
  }

  static bool cfa_restore(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    if (cfa->cie_regs_ == nullptr) {
      log("restore while processing cie");
      cfa->last_error_ = DWARF_ERROR_ILLEGAL_STATE;
      return false;
    }
    auto reg_entry = cfa->cie_regs_->find(reg);
    if (reg_entry == cfa->cie_regs_->end()) {
      cfa->regs_.erase(reg);
    } else {
      cfa->regs_[reg].type = reg_entry->second.type;
      cfa->regs_[reg].value = reg_entry->second.value;
    }
    return true;
  }

  static bool cfa_undefined(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    auto reg_entry = cfa->regs_.find(reg);
    if (reg_entry != cfa->regs_.end()) {
      cfa->regs_.erase(reg);
    }
    return true;
  }

  static bool cfa_same_value(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    cfa->regs_[reg] = { .type = DWARF_LOCATION_SAME };
    return true;
  }

  static bool cfa_register(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    AddressType reg_dst = cfa->operands_[1];
    cfa->regs_[reg] = { .type = DWARF_LOCATION_REGISTER, .value = reg_dst };
    return true;
  }

  static bool cfa_remember_state(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    cfa->reg_state_.push(cfa->regs_);
    return true;
  }

  static bool cfa_restore_state(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    if (cfa->reg_state_.size() == 0) {
      log("Warning: Attempt to restore without remember.");
      return true;
    }
    cfa->regs_ = cfa->reg_state_.top();
    cfa->reg_state_.pop();
    return true;
  }

  static bool cfa_def_cfa(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    cfa->cfa_location_ = { .type = DWARF_LOCATION_REGISTER, .value = reg };
    cfa->cfa_offset_ = cfa->operands_[1];
    return true;
  }

  static bool cfa_def_cfa_register(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    cfa->cfa_location_ = { .type = DWARF_LOCATION_REGISTER, .value = cfa->operands_[0] };
    return true;
  }

  static bool cfa_def_cfa_offset(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    // Changing the offset if this is not a register is illegal.
    if (cfa->cfa_location_.type != DWARF_LOCATION_REGISTER) {
      log("Attempt to set offset, but cfa is not set to a register.");
      cfa->last_error_ = DWARF_ERROR_ILLEGAL_STATE;
      return false;
    }
    cfa->cfa_offset_ = cfa->operands_[0];
    return true;
  }

  static bool cfa_def_cfa_expression(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    // TODO: Add information about op data.
    cfa->cfa_location_ = { .type = DWARF_LOCATION_EXPRESSION };
    return true;
  }

  static bool cfa_expression(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    // TODO: Add information op data.
    cfa->regs_[reg] = { .type = DWARF_LOCATION_EXPRESSION };
    return true;
  }

  static bool cfa_offset_extended_sf(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    if (sizeof(AddressType) == 4) {
      int32_t value = cfa->operands_[1] * cfa->cie_->data_alignment_factor;
      cfa->regs_[reg] = { .type = DWARF_LOCATION_OFFSET, .value = static_cast<uint64_t>(value) };
    } else {
      int64_t value = cfa->operands_[1] * cfa->cie_->data_alignment_factor;
      cfa->regs_[reg] = { .type = DWARF_LOCATION_OFFSET, .value = static_cast<uint64_t>(value) };
    }
    return true;
  }

  static bool cfa_def_cfa_sf(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    cfa->cfa_location_ = { .type = DWARF_LOCATION_REGISTER, .value = reg };
    if (sizeof(AddressType) == 4) {
      cfa->cfa_offset_ = static_cast<int32_t>(cfa->operands_[1]) * cfa->cie_->data_alignment_factor;
    } else {
      cfa->cfa_offset_ = static_cast<int64_t>(cfa->operands_[1]) * cfa->cie_->data_alignment_factor;
    }
    return true;
  }

  static bool cfa_def_cfa_offset_sf(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    // Changing the offset if this is not a register is illegal.
    if (cfa->cfa_location_.type != DWARF_LOCATION_REGISTER) {
      log("Attempt to set offset, but cfa is not set to a register.");
      cfa->last_error_ = DWARF_ERROR_ILLEGAL_STATE;
      return false;
    }
    if (sizeof(AddressType) == 4) {
      cfa->cfa_offset_ = static_cast<int32_t>(cfa->operands_[0]) * cfa->cie_->data_alignment_factor;
    } else {
      cfa->cfa_offset_ = static_cast<int64_t>(cfa->operands_[0]) * cfa->cie_->data_alignment_factor;
    }
    return true;
  }

  static bool cfa_val_offset(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    cfa->regs_[reg] = { .type = DWARF_LOCATION_VAL_OFFSET, .value = cfa->operands_[1] * cfa->cie_->data_alignment_factor };
    return true;
  }

  static bool cfa_val_offset_sf(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    if (sizeof(AddressType) == 4) {
      int32_t value = static_cast<int32_t>(cfa->operands_[1]) * cfa->cie_->data_alignment_factor;
      cfa->regs_[reg] = { .type = DWARF_LOCATION_VAL_OFFSET, .value = static_cast<uint64_t>(value) };
    } else {
      int64_t value = static_cast<int64_t>(cfa->operands_[1]) * cfa->cie_->data_alignment_factor;
      cfa->regs_[reg] = { .type = DWARF_LOCATION_VAL_OFFSET, .value = static_cast<uint64_t>(value) };
    }
    return true;
  }

  static bool cfa_val_expression(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    // TODO: Add information op data.
    cfa->regs_[reg] = { .type = DWARF_LOCATION_VAL_EXPRESSION };
    return true;
  }

  static bool cfa_gnu_negative_offset_extended(void* ptr) {
    DwarfCfa* cfa = reinterpret_cast<DwarfCfa*>(ptr);
    AddressType reg = cfa->operands_[0];
    if (sizeof(AddressType) == 4) {
      int32_t value = -cfa->operands_[1];
      cfa->regs_[reg] = { .type = DWARF_LOCATION_OFFSET, .value = static_cast<uint64_t>(value) };
    } else {
      int64_t value = -cfa->operands_[1];
      cfa->regs_[reg] = { .type = DWARF_LOCATION_OFFSET, .value = static_cast<uint64_t>(value) };
    }
    return true;
  }

  constexpr static Callback kCallbackTable[64] = {
    {                           // 0x00 DW_CFA_nop
      cfa_nop,
      2,
      0,
      {},
    },
    {                           // 0x01 DW_CFA_set_loc
      cfa_set_loc,
      2,
      1,
      { DW_EH_PE_absptr },
    },
    {                           // 0x02 DW_CFA_advance_loc1
      cfa_advance_loc,
      2,
      1,
      { DW_EH_PE_udata1 },
    },
    {                           // 0x03 DW_CFA_advance_loc2
      cfa_advance_loc,
      2,
      1,
      { DW_EH_PE_udata2 },
    },
    {                           // 0x04 DW_CFA_advance_loc4
      cfa_advance_loc,
      2,
      1,
      { DW_EH_PE_udata4 },
    },
    {                           // 0x05 DW_CFA_offset_extended
      cfa_offset,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_uleb128 },
    },
    {                           // 0x06 DW_CFA_restore_extended
      cfa_restore,
      2,
      1,
      { DW_EH_PE_uleb128 },
    },
    {                           // 0x07 DW_CFA_undefined
      cfa_undefined,
      2,
      1,
      { DW_EH_PE_uleb128 },
    },
    {                           // 0x08 DW_CFA_same_value
      cfa_same_value,
      2,
      1,
      { DW_EH_PE_uleb128 },
    },
    {                           // 0x09 DW_CFA_register
      cfa_register,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_uleb128 },
    },
    {                           // 0x0a DW_CFA_remember_state
      cfa_remember_state,
      2,
      0,
      {},
    },
    {                           // 0x0b DW_CFA_restore_state
      cfa_restore_state,
      2,
      0,
      {},
    },
    {                           // 0x0c DW_CFA_def_cfa
      cfa_def_cfa,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_uleb128 },
    },
    {                           // 0x0d DW_CFA_def_cfa_register
      cfa_def_cfa_register,
      2,
      1,
      { DW_EH_PE_uleb128 },
    },
    {                           // 0x0e DW_CFA_def_cfa_offset
      cfa_def_cfa_offset,
      2,
      1,
      { DW_EH_PE_uleb128 },
    },
    {                           // 0x0f DW_CFA_def_cfa_expression
      cfa_def_cfa_expression,
      2,
      1,
      { DW_EH_PE_block },
    },
    {                           // 0x10 DW_CFA_expression
      cfa_expression,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_block },
    },
    {                           // 0x11 DW_CFA_offset_extended_sf
      cfa_offset_extended_sf,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_sleb128 },
    },
    {                           // 0x12 DW_CFA_def_cfa_sf
      cfa_def_cfa_sf,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_sleb128 },
    },
    {                           // 0x13 DW_CFA_def_cfa_offset_sf
      cfa_def_cfa_offset_sf,
      2,
      1,
      { DW_EH_PE_sleb128 },
    },
    {                           // 0x14 DW_CFA_val_offset
      cfa_val_offset,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_uleb128 },
    },
    {                           // 0x15 DW_CFA_val_offset_sf
      cfa_val_offset_sf,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_sleb128 },
    },
    {                           // 0x16 DW_CFA_val_expression
      cfa_val_expression,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_block },
    },
    { nullptr, 0, 0, {} },      // 0x17 illegal cfa
    { nullptr, 0, 0, {} },      // 0x18 illegal cfa
    { nullptr, 0, 0, {} },      // 0x19 illegal cfa
    { nullptr, 0, 0, {} },      // 0x1a illegal cfa
    { nullptr, 0, 0, {} },      // 0x1b illegal cfa
    { nullptr, 0, 0, {} },      // 0x1c DW_CFA_lo_user (Treat this as illegal)
    { nullptr, 0, 0, {} },      // 0x1d illegal cfa
    { nullptr, 0, 0, {} },      // 0x1e illegal cfa
    { nullptr, 0, 0, {} },      // 0x1f illegal cfa
    { nullptr, 0, 0, {} },      // 0x20 illegal cfa
    { nullptr, 0, 0, {} },      // 0x21 illegal cfa
    { nullptr, 0, 0, {} },      // 0x22 illegal cfa
    { nullptr, 0, 0, {} },      // 0x23 illegal cfa
    { nullptr, 0, 0, {} },      // 0x24 illegal cfa
    { nullptr, 0, 0, {} },      // 0x25 illegal cfa
    { nullptr, 0, 0, {} },      // 0x26 illegal cfa
    { nullptr, 0, 0, {} },      // 0x27 illegal cfa
    { nullptr, 0, 0, {} },      // 0x28 illegal cfa
    { nullptr, 0, 0, {} },      // 0x29 illegal cfa
    { nullptr, 0, 0, {} },      // 0x2a illegal cfa
    { nullptr, 0, 0, {} },      // 0x2b illegal cfa
    { nullptr, 0, 0, {} },      // 0x2c illegal cfa
    { nullptr, 0, 0, {} },      // 0x2d DW_CFA_GNU_window_save (Treat this as illegal)
    {                           // 0x2e DW_CFA_GNU_args_size
      cfa_nop,
      2,
      1,
      { DW_EH_PE_uleb128 },
    },
    {                           // 0x2f DW_CFA_GNU_negative_offset_extended
      cfa_gnu_negative_offset_extended,
      2,
      2,
      { DW_EH_PE_uleb128, DW_EH_PE_uleb128 },
    },
    { nullptr, 0, 0, {} },      // 0x30 illegal cfa
    { nullptr, 0, 0, {} },      // 0x31 illegal cfa
    { nullptr, 0, 0, {} },      // 0x32 illegal cfa
    { nullptr, 0, 0, {} },      // 0x33 illegal cfa
    { nullptr, 0, 0, {} },      // 0x34 illegal cfa
    { nullptr, 0, 0, {} },      // 0x35 illegal cfa
    { nullptr, 0, 0, {} },      // 0x36 illegal cfa
    { nullptr, 0, 0, {} },      // 0x37 illegal cfa
    { nullptr, 0, 0, {} },      // 0x38 illegal cfa
    { nullptr, 0, 0, {} },      // 0x39 illegal cfa
    { nullptr, 0, 0, {} },      // 0x3a illegal cfa
    { nullptr, 0, 0, {} },      // 0x3b illegal cfa
    { nullptr, 0, 0, {} },      // 0x3c illegal cfa
    { nullptr, 0, 0, {} },      // 0x3d illegal cfa
    { nullptr, 0, 0, {} },      // 0x3e illegal cfa
    { nullptr, 0, 0, {} },      // 0x3f DW_CFA_hi_user (Treat this as illegal)
  };
};
template<typename AddressType> constexpr Callback DwarfCfa<AddressType>::kCallbackTable[64];

#endif  // _LIBANDROID_UNWIND_DWARF_CFA_H
