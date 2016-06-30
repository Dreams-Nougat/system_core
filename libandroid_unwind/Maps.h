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

#ifndef _LIBANDROID_UNWIND_MAPS_H
#define _LIBANDROID_UNWIND_MAPS_H

#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

class Elf;
class Memory;

struct MapInfo {
  uint64_t start;
  uint64_t end;
  uint64_t offset;
  uint16_t flags;
  std::string name;
  Elf* elf = nullptr;

  Memory* CreateMemory(pid_t pid);
};

class Maps {
 public:
  Maps() = default;
  virtual ~Maps();

  const MapInfo* Find(uint64_t pc);

  bool ParseLine(const char* line, MapInfo* map_info);

  virtual bool Parse();

  virtual std::string GetMapsFile() { return ""; }

  typedef std::vector<MapInfo>::iterator iterator;
  iterator begin() { return maps_.begin(); }
  iterator end() { return maps_.end(); }

  typedef std::vector<MapInfo>::const_iterator const_iterator;
  const_iterator begin() const { return maps_.begin(); }
  const_iterator end() const { return maps_.end(); }

  size_t Total() { return maps_.size(); }

  void ClearCache();

 protected:
  std::vector<MapInfo> maps_;
};

class MapsRemote : public Maps {
 public:
  MapsRemote(pid_t pid) : pid_(pid) {}
  virtual ~MapsRemote() = default;

  virtual std::string GetMapsFile() override;

 private:
  pid_t pid_;
};

class MapsLocal : public MapsRemote {
 public:
  MapsLocal() : MapsRemote(getpid()) {}
  virtual ~MapsLocal() = default;
};

class MapsBuffer : public Maps {
 public:
  MapsBuffer(const char* buffer) : buffer_(buffer) {}
  virtual ~MapsBuffer() = default;

  bool Parse() override;

 private:
  const char* buffer_;
};

class MapsFile : public Maps {
 public:
  MapsFile(std::string file) : file_(file) {}
  virtual ~MapsFile() = default;

  std::string GetMapsFile() override { return file_; }

 private:
  std::string file_;
};

#endif  // _LIBANDROID_UNWIND_MAPS_H
