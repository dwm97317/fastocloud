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

#include <unistd.h>

#include <thread>

#include <gst/gst.h>

#include <common/file_system/file_system.h>
#include <common/sprintf.h>

#include "stream/configs_factory.h"
#include "stream/link_generator/streamlink.h"
#include "stream/streams/screen_stream.h"
#include "stream/streams_factory.h"

#include "base/config_fields.h"

#define STREAMS_TRY_COUNT 10

#define SCREEN_SLEEP_SEC 10
#define RELAY_SLEEP_SEC 10
#define RELAY_PLAYLIST_SLEEP_SEC 10
#define ENCODE_SLEEP_SEC 10
#define TIMESHIFT_REC_SLEEP_SEC 10
#define TIMESHIFT_PLAYLIST_SLEEP_SEC 10
#define MOSAIC_SLEEP_SEC 10
#define AD_SLEEP_SEC 10
#define RELAY_AUDIO_SLEEP_SEC 10
#define RELAY_VIDEO_SLEEP_SEC 10
#define STATIC_IMAGE_SLEEP_SEC 10
#define TEST_SLEEP_SEC 10
#define CATCHUP_SLEEP_SEC 20
#define M3U8LOG_SLEEP_SEC 10
#define DIFF_SEC 1

#define MODEL_GRAPH_PATH PROJECT_TEST_SOURCES_DIR "/data/graph_tinyyolov2_tensorflow.pb"
#define LABELS_GRAPH_PATH PROJECT_TEST_SOURCES_DIR "/data/labels.txt"

void* quit_job(fastocloud::stream::IBaseStream* job, uint32_t sleep_sec) {
  static unsigned int seed = time(NULL);
  int rand = rand_r(&seed) % (sleep_sec + DIFF_SEC);
  sleep(rand);
  job->Quit(fastocloud::stream::EXIT_SELF);
  return NULL;
}

void check_encoders() {
  const std::string yuri =
      R"({"urls": [{"id": 115, "uri": "https://www.youtube.com/watch?v=HVYJJirRADU", "stream_link": true}]})";
  const std::string iuri_str =
      R"({"urls": [{"id": 115, "uri": "http://live.qvcuk.simplestreamcdn.com/live/qvcuk_main_clean/bitrate1.isml/live.m3u8"}]})";
  const std::string enc_uri_str =
      R"({"urls": [{"id": 115, "uri": "tcp://localhost:1935"}]})";
#if defined(MACHINE_LEARNING)
  const std::string deep_learning_str =
      "{\"model_path\": \"" MODEL_GRAPH_PATH
      "\", \"backend\": 0, \"properties\": [{\"input\":\"input/Placeholder\"}, {\"output\":\"add_8\"}]}";
  const std::string deep_learning_overlay_str = "{\"labels_path\" : \"" LABELS_GRAPH_PATH "\"}";
#endif

  const fastocloud::utils::ArgsMap config_args = {
    {ID_FIELD, "encoding"},
    {TYPE_FIELD, "2"},
    {HAVE_AUDIO_FIELD, "false"},
    {FEEDBACK_DIR_FIELD, "~"},
    {VIDEO_CODEC_FIELD, "x264enc"},
    {AUDIO_CODEC_FIELD, "faac"},
    {SIZE_FIELD, "416x416"},
    {INPUT_FIELD, yuri},
    {OUTPUT_FIELD, enc_uri_str}
#if defined(MACHINE_LEARNING)
    ,
    {DEEP_LEARNING_FIELD, deep_learning_str},
    {DEEP_LEARNING_OVERLAY_FIELD, deep_learning_overlay_str}
#endif
  };

  std::string script_path;
  if (!common::file_system::find_file_in_path("streamlink", &script_path)) {
    return;
  }

  fastocloud::stream::link_generator::StreamLinkGenerator gena(script_path);

  for (size_t i = 0; i < STREAMS_TRY_COUNT; ++i) {
    fastocloud::StreamStruct encoding(
        fastocloud::StreamInfo{common::MemSPrintf("encoding_%llu", i), fastocloud::StreamType::ENCODE, {0}, {0}});
    fastocloud::stream::TimeShiftInfo tinf;
    fastocloud::stream::Config* lconfig = nullptr;
    common::Error err = fastocloud::stream::make_config(config_args, &lconfig);
    if (!err) {
      const std::unique_ptr<fastocloud::stream::Config> config_copy(
          fastocloud::stream::make_config_copy(lconfig, &gena));
      fastocloud::stream::IBaseStream* job_enc = fastocloud::stream::StreamsFactory::GetInstance().CreateStream(
          config_copy.get(), nullptr, &encoding, tinf, fastocloud::stream::invalid_chunk_index);
      std::thread th(quit_job, job_enc, ENCODE_SLEEP_SEC);
      CHECK(job_enc->GetType() == fastocloud::StreamType::ENCODE);
      job_enc->Exec();
      th.join();
      delete job_enc;
      delete lconfig;
    }
  }
}

int main(int argc, char** argv) {
  fastocloud::stream::streams_init(argc, argv);
  check_encoders();
  return 0;
}
