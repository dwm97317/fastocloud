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

#include <common/libev/pipe_client.h>

#include <fastotv/protocol/protocol.h>

namespace fastocloud {
namespace server {
namespace pipe {

class ProtocoledPipeClient : public fastotv::protocol::ProtocolClient<common::libev::IoClient> {
 public:
  typedef fastotv::protocol::ProtocolClient<common::libev::IoClient> base_class;
  ~ProtocoledPipeClient() override;

  const char* ClassName() const override;

  ProtocoledPipeClient(common::libev::IoLoop* server, descriptor_t read_fd, descriptor_t write_fd);

 protected:
  common::ErrnoError SingleWrite(const void* data, size_t size, size_t* nwrite_out) override;
  common::ErrnoError SingleRead(void* out, size_t max_size, size_t* nread) override;

  descriptor_t GetFd() const override;

 private:
  common::ErrnoError DoClose() override;

  common::libev::PipeReadClient* pipe_read_client_;
  common::libev::PipeWriteClient* pipe_write_client_;
  const descriptor_t read_fd_;

  DISALLOW_COPY_AND_ASSIGN(ProtocoledPipeClient);
};

}  // namespace pipe
}  // namespace server
}  // namespace fastocloud
