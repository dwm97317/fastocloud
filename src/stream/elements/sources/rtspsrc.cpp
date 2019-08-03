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

#include "stream/elements/sources/rtspsrc.h"

#include <string>

namespace fastocloud {
namespace stream {
namespace elements {
namespace sources {

void ElementRTSPSrc::SetLocation(const std::string& location) {
  SetProperty("location", location);
}

gboolean ElementRTSPSrc::RegisterPadAddedCallback(pad_added_callback_t cb, gpointer user_data) {
  return RegisterCallback("pad-added", G_CALLBACK(cb), user_data);
}

ElementRTSPSrc* make_rtsp_src(const std::string& location, element_id_t input_id) {
  ElementRTSPSrc* rtsp_src = make_sources<ElementRTSPSrc>(input_id);
  rtsp_src->SetLocation(location);
  return rtsp_src;
}

}  // namespace sources
}  // namespace elements
}  // namespace stream
}  // namespace fastocloud
