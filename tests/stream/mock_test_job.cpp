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

#include <gmock/gmock.h>

#include <gst/gst.h>

#include <thread>

#include "base/constants.h"

#include "stream/configs_factory.h"
#include "stream/streams/screen_stream.h"

using testing::_;

class FakeObserver : public fastocloud::stream::IBaseStream::IStreamClient {
 public:
  MOCK_METHOD2(OnStatusChanged, void(fastocloud::stream::IBaseStream*, fastocloud::StreamStatus));
  MOCK_METHOD1(OnPipelineEOS, void(fastocloud::stream::IBaseStream*));
  MOCK_METHOD1(OnTimeoutUpdated, void(fastocloud::stream::IBaseStream*));
  MOCK_METHOD2(OnSyncMessageReceived, void(fastocloud::stream::IBaseStream*, GstMessage*));
  MOCK_METHOD2(OnASyncMessageReceived, void(fastocloud::stream::IBaseStream*, GstMessage*));
  void OnInputChanged(const fastocloud::InputUri& uri) override { UNUSED(uri); }
  GstPadProbeInfo* OnCheckReveivedOutputData(fastocloud::stream::IBaseStream* job,
                                             fastocloud::stream::Probe* probe,
                                             GstPadProbeInfo* info) override {
    UNUSED(job);
    UNUSED(probe);
    return info;
  }
  GstPadProbeInfo* OnCheckReveivedData(fastocloud::stream::IBaseStream* job,
                                       fastocloud::stream::Probe* probe,
                                       GstPadProbeInfo* info) override {
    UNUSED(job);
    UNUSED(probe);
    return info;
  }
  MOCK_METHOD3(OnProbeEvent, void(fastocloud::stream::IBaseStream*, fastocloud::stream::Probe*, GstEvent*));
  MOCK_METHOD1(OnPipelineCreated, void(fastocloud::stream::IBaseStream*));
};

void* quit_job(fastocloud::stream::IBaseStream* job) {
  sleep(5);
  job->Quit(fastocloud::stream::EXIT_SELF);
  return NULL;
}

TEST(Job, Status) {
  fastocloud::output_t ouri;
  fastocloud::OutputUri uri;
  uri.SetOutput(common::uri::Url("screen"));
  ouri.push_back(uri);

  FakeObserver cl;
  fastocloud::stream::streams_init(0, NULL);
  fastocloud::StreamStruct st(fastocloud::StreamInfo{"screen", fastocloud::SCREEN, {}, {0}});
  fastocloud::stream::Config bconf(fastocloud::SCREEN, 10, {fastocloud::InputUri(0, common::uri::Url(SCREEN_URL))},
                                   {fastocloud::OutputUri(0, common::uri::Url(TEST_URL))});
  fastocloud::stream::streams::AudioVideoConfig conf(bconf);
  fastocloud::stream::IBaseStream* job = new fastocloud::stream::streams::ScreenStream(&conf, &cl, &st);
  std::thread th(&quit_job, job);
  EXPECT_CALL(cl, OnStatusChanged(job, _)).Times(5);
  EXPECT_CALL(cl, OnTimeoutUpdated(job)).Times(::testing::AnyNumber());
  EXPECT_CALL(cl, OnSyncMessageReceived(job, _)).Times(::testing::AnyNumber());
  EXPECT_CALL(cl, OnProbeEvent(job, _, _)).Times(::testing::AnyNumber());
  job->Exec();
  th.join();
  delete job;
}
