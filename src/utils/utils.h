/*  Copyright (C) 2014-2019 FastoGT. All right reserved.
    This file is part of fastocloud.
    fastocloud is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    fastocloud is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with fastocloud.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <string>
#include <vector>

#include <common/error.h>
#include <common/file_system/path.h>

namespace fastocloud {
namespace utils {

struct CpuShot {
  CpuShot();

  uint64_t user;
  uint64_t nice;
  uint64_t system;
  uint64_t idle;
  uint64_t iowait;
  uint64_t irq;
  uint64_t softirq;
  uint64_t steal;
  uint64_t guest;
  uint64_t guest_nice;
};

long double GetCpuMachineLoad(const CpuShot& prev, const CpuShot& next);
CpuShot GetMachineCpuShot();

struct MemoryShot {
  MemoryShot();

  long double GetAvailable() const;

  uint64_t total_bytes_ram;
  uint64_t free_bytes_ram;
  uint64_t avail_bytes_ram;
};

MemoryShot GetMachineMemoryShot();

struct HddShot {
  HddShot();

  uint64_t hdd_bytes_total;
  uint64_t hdd_bytes_free;
};

HddShot GetMachineHddShot();

struct NetShot {
  NetShot();

  uint64_t bytes_recv;
  uint64_t bytes_send;
};

NetShot GetMachineNetShot();

struct SysinfoShot {
  SysinfoShot();

  unsigned long loads[3];
  time_t uptime;
};

SysinfoShot GetMachineSysinfoShot();

common::ErrnoError CreateAndCheckDir(const std::string& directory_path);
void RemoveOldFilesByTime(const common::file_system::ascii_directory_string_path& dir,
                          common::utctime_t max_life_secs,
                          const char* ext);
void RemoveFilesByExtension(const common::file_system::ascii_directory_string_path& dir, const char* ext);

}  // namespace utils
}  // namespace fastocloud
