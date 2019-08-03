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

#include "stream/streams/builders/encoding/rtsp_stream_builder.h"

#include <string>

#include <common/sprintf.h>

#include "base/input_uri.h"  // for InputUri

#include "stream/pad/pad.h"  // for Pad

#include "stream/elements/element.h"  // for Element
#include "stream/elements/sources/rtspsrc.h"

namespace fastocloud {
namespace stream {
namespace streams {
namespace builders {

RtspEncodingStreamBuilder::RtspEncodingStreamBuilder(const EncodeConfig* api, SrcDecodeBinStream* observer)
    : EncodingStreamBuilder(api, observer) {}

elements::Element* RtspEncodingStreamBuilder::BuildInputSrc() {
  const Config* config = GetConfig();
  input_t prepared = config->GetInput();
  InputUri uri = prepared[0];
  const common::uri::Url url = uri.GetInput();
  elements::Element* src = elements::sources::make_rtsp_src(url.GetUrl(), 0);
  pad::Pad* src_pad = src->StaticPad("src");
  if (src_pad->IsValid()) {
    HandleInputSrcPadCreated(url.GetScheme(), src_pad, 0);
  }
  delete src_pad;
  ElementAdd(src);
  return src;
}

Connector RtspEncodingStreamBuilder::BuildInput() {
  const EncodeConfig* config = static_cast<const EncodeConfig*>(GetConfig());
  elements::Element* video = nullptr;
  if (config->HaveVideo()) {
    elements::ElementDecodebin* decodebin = new elements::ElementDecodebin(common::MemSPrintf(DECODEBIN_NAME_1U, 0));
    ElementAdd(decodebin);
    ElementLink(video, decodebin);
    HandleDecodebinCreated(decodebin);
    video = decodebin;
  }

  elements::Element* audio = nullptr;
  if (config->HaveAudio()) {
    elements::ElementDecodebin* decodebin = new elements::ElementDecodebin(common::MemSPrintf(DECODEBIN_NAME_1U, 1));
    ElementAdd(decodebin);
    ElementLink(audio, decodebin);
    HandleDecodebinCreated(decodebin);
    audio = decodebin;
  }
  return {nullptr, nullptr};
}

}  // namespace builders
}  // namespace streams
}  // namespace stream
}  // namespace fastocloud
