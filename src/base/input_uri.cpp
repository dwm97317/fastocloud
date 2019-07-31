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

#include "base/input_uri.h"

#include <string>

#include <json-c/json_object.h>
#include <json-c/json_tokener.h>

#include "base/constants.h"

#define INPUT_ID_FIELD "id"
#define INPUT_URI_FIELD "uri"
#define USER_AGENT_FIELD "user_agent"
#define STREAMLINK_URL_FIELD "stream_link"

namespace fastocloud {

InputUri::InputUri() : InputUri(0, common::uri::Url()) {}

InputUri::InputUri(uri_id_t id, const common::uri::Url& input, user_agent_t ua)
    : base_class(), id_(id), input_(input), user_agent_(ua) {}

InputUri::uri_id_t InputUri::GetID() const {
  return id_;
}

void InputUri::SetID(uri_id_t id) {
  id_ = id;
}

common::uri::Url InputUri::GetInput() const {
  return input_;
}

void InputUri::SetInput(const common::uri::Url& uri) {
  input_ = uri;
}

InputUri::user_agent_t InputUri::GetUserAgent() const {
  return user_agent_;
}

void InputUri::SetUserAgent(user_agent_t agent) {
  user_agent_ = agent;
}

bool InputUri::GetStreamLink() const {
  return stream_url_;
}

void InputUri::SetStreamLink(bool stream) {
  stream_url_ = stream;
}

bool InputUri::Equals(const InputUri& inf) const {
  return id_ == inf.id_ && input_ == inf.input_;
}

common::Error InputUri::DoDeSerialize(json_object* serialized) {
  InputUri res;
  json_object* jid = nullptr;
  json_bool jid_exists = json_object_object_get_ex(serialized, INPUT_ID_FIELD, &jid);
  if (jid_exists) {
    res.SetID(json_object_get_int64(jid));
  }

  json_object* juri = nullptr;
  json_bool juri_exists = json_object_object_get_ex(serialized, INPUT_URI_FIELD, &juri);
  if (juri_exists) {
    res.SetInput(common::uri::Url(json_object_get_string(juri)));
  }

  json_object* juser_agent = nullptr;
  json_bool juser_agent_exists = json_object_object_get_ex(serialized, USER_AGENT_FIELD, &juser_agent);
  if (juser_agent_exists) {
    user_agent_t agent = static_cast<user_agent_t>(json_object_get_int(juser_agent));
    res.SetUserAgent(agent);
  }

  json_object* jstream_url = nullptr;
  json_bool jstream_url_exists = json_object_object_get_ex(serialized, STREAMLINK_URL_FIELD, &jstream_url);
  if (jstream_url_exists) {
    bool sl = json_object_get_boolean(jstream_url);
    res.SetStreamLink(sl);
  }

  *this = res;
  return common::Error();
}

common::Error InputUri::SerializeFields(json_object* out) const {
  json_object_object_add(out, INPUT_ID_FIELD, json_object_new_int64(GetID()));
  std::string url_str = common::ConvertToString(GetInput());
  json_object_object_add(out, INPUT_URI_FIELD, json_object_new_string(url_str.c_str()));
  json_object_object_add(out, USER_AGENT_FIELD, json_object_new_int(user_agent_));
  json_object_object_add(out, STREAMLINK_URL_FIELD, json_object_new_boolean(stream_url_));

  return common::Error();
}

bool IsTestInputUrl(const InputUri& url) {
  return url.GetInput() == common::uri::Url(TEST_URL);
}

}  // namespace fastocloud
