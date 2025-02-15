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

#include "server/daemon/commands_info/service/sync_info.h"

#define SYNC_INFO_STREAMS_FIELD "streams"
#if defined(SUBSCRIBERS)
#define SYNC_INFO_USERS_FIELD "subscribers"
#endif

namespace fastocloud {
namespace server {
namespace service {

SyncInfo::SyncInfo()
    : base_class(),
      streams_()
#if defined(SUBSCRIBERS)
      ,
      users_()
#endif
{
}

SyncInfo::streams_t SyncInfo::GetStreams() const {
  return streams_;
}

#if defined(SUBSCRIBERS)
SyncInfo::users_t SyncInfo::GetUsers() const {
  return users_;
}
#endif

common::Error SyncInfo::SerializeFields(json_object* out) const {
  json_object* jstreams = json_object_new_array();
  for (const std::string& stream : streams_) {
    json_object* jstream = json_object_new_string(stream.c_str());
    json_object_array_add(jstreams, jstream);
  }
  json_object_object_add(out, SYNC_INFO_STREAMS_FIELD, jstreams);

#if defined(SUBSCRIBERS)
  json_object* jusers = json_object_new_array();
  for (const std::string& user : users_) {
    json_object* juser = json_object_new_string(user.c_str());
    json_object_array_add(jusers, juser);
  }
  json_object_object_add(out, SYNC_INFO_USERS_FIELD, jusers);
#endif
  return common::Error();
}

common::Error SyncInfo::DoDeSerialize(json_object* serialized) {
  SyncInfo inf;
  json_object* jstreams = nullptr;
  json_bool jstreams_exists = json_object_object_get_ex(serialized, SYNC_INFO_STREAMS_FIELD, &jstreams);
  if (jstreams_exists) {
    size_t len = json_object_array_length(jstreams);
    streams_t streams;
    for (size_t i = 0; i < len; ++i) {
      json_object* jstream = json_object_array_get_idx(jstreams, i);
      streams.push_back(json_object_get_string(jstream));
    }
    inf.streams_ = streams;
  }

#if defined(SUBSCRIBERS)
  json_object* jusers = nullptr;
  json_bool jusers_exists = json_object_object_get_ex(serialized, SYNC_INFO_USERS_FIELD, &jusers);
  if (jusers_exists) {
    size_t len = json_object_array_length(jusers);
    users_t users;
    for (size_t i = 0; i < len; ++i) {
      json_object* juser = json_object_array_get_idx(jusers, i);
      users.push_back(json_object_get_string(juser));
    }
    inf.users_ = users;
  }
#endif

  *this = inf;
  return common::Error();
}

}  // namespace service
}  // namespace server
}  // namespace fastocloud
