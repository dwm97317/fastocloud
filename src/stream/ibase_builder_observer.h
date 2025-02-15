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

namespace fastocloud {
namespace stream {

namespace pad {
class Pad;
}

class IBaseBuilderObserver {
 public:
  virtual void OnInpudSrcPadCreated(common::uri::Url::scheme scheme, pad::Pad* src_pad, element_id_t id) = 0;
  virtual void OnOutputSinkPadCreated(common::uri::Url::scheme scheme, pad::Pad* sink_pad, element_id_t id) = 0;

  virtual ~IBaseBuilderObserver();
};

}  // namespace stream
}  // namespace fastocloud
