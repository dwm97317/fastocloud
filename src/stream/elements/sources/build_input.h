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

#include <common/uri/url.h>

#include "stream/stypes.h"

#include "stream/elements/element.h"

#include "base/input_uri.h"

namespace common {
class Uri;
}

namespace fastocloud {
namespace stream {
namespace elements {
namespace sources {

Element* make_src(const InputUri& uri, element_id_t input_id, gint timeout_secs);

}  // namespace sources
}  // namespace elements
}  // namespace stream
}  // namespace fastocloud
