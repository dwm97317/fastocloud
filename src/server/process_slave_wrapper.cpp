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

#include "server/process_slave_wrapper.h"

#include <sys/prctl.h>
#include <sys/wait.h>

#include <dlfcn.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <common/file_system/file_system.h>
#include <common/file_system/string_path_utils.h>
#include <common/net/http_client.h>
#include <common/net/net.h>

#include "base/config_fields.h"
#include "base/inputs_outputs.h"

#include "gpu_stats/perf_monitor.h"

#include "pipe/pipe_client.h"

#include "server/child_stream.h"
#include "server/daemon/client.h"
#include "server/daemon/commands.h"
#include "server/daemon/commands_info/service/activate_info.h"
#include "server/daemon/commands_info/service/get_log_info.h"
#include "server/daemon/commands_info/service/prepare_info.h"
#include "server/daemon/commands_info/service/server_info.h"
#include "server/daemon/commands_info/service/stop_info.h"
#include "server/daemon/commands_info/service/sync_info.h"
#include "server/daemon/commands_info/stream/get_log_info.h"
#include "server/daemon/commands_info/stream/restart_info.h"
#include "server/daemon/commands_info/stream/start_info.h"
#include "server/daemon/commands_info/stream/stop_info.h"
#include "server/daemon/server.h"
#include "server/http/handler.h"
#include "server/http/server.h"
#include "server/options/options.h"
#include "server/stream_struct_utils.h"
#if defined(SUBSCRIBERS)
#include "server/subscribers/handler.h"
#include "server/subscribers/server.h"
#include "server/sync_finder.h"
#endif
#include "server/vods/handler.h"
#include "server/vods/server.h"

#include "stream/stream_wrapper.h"
#include "stream_commands/commands.h"

#include "utils/arg_converter.h"
#include "utils/m3u8_reader.h"
#include "utils/utils.h"

namespace {

bool GetHttpHostAndPort(const std::string& host, common::net::HostAndPort* out) {
  if (host.empty() || !out) {
    return false;
  }

  common::net::HostAndPort http_server;
  size_t del = host.find_last_of(':');
  if (del != std::string::npos) {
    http_server.SetHost(host.substr(0, del));
    std::string port_str = host.substr(del + 1);
    uint16_t lport;
    if (common::ConvertFromString(port_str, &lport)) {
      http_server.SetPort(lport);
    }
  } else {
    http_server.SetHost(host);
    http_server.SetPort(80);
  }
  *out = http_server;
  return true;
}

bool GetPostServerFromUrl(const common::uri::Url& url, common::net::HostAndPort* out) {
  if (!url.IsValid() || !out) {
    return false;
  }

  const std::string host_str = url.GetHost();
  return GetHttpHostAndPort(host_str, out);
}

common::Optional<common::file_system::ascii_file_string_path> MakeStreamLogPath(const std::string& feedback_dir) {
  common::file_system::ascii_directory_string_path dir(feedback_dir);
  return dir.MakeFileStringPath(LOGS_FILE_NAME);
}

common::Optional<common::file_system::ascii_file_string_path> MakeStreamPipelinePath(const std::string& feedback_dir) {
  common::file_system::ascii_directory_string_path dir(feedback_dir);
  return dir.MakeFileStringPath(DUMP_FILE_NAME);
}

common::Error PostHttpFile(const common::file_system::ascii_file_string_path& file_path, const common::uri::Url& url) {
  common::net::HostAndPort http_server_address;
  if (!GetPostServerFromUrl(url, &http_server_address)) {
    return common::make_error_inval();
  }

  common::net::HttpClient cl(http_server_address);
  common::ErrnoError errn = cl.Connect();
  if (errn) {
    return common::make_error_from_errno(errn);
  }

  const auto path = url.GetPath();
  common::Error err = cl.PostFile(path, file_path);
  if (err) {
    cl.Disconnect();
    return err;
  }

  common::http::HttpResponse lresp;
  err = cl.ReadResponse(&lresp);
  if (err) {
    cl.Disconnect();
    return err;
  }

  if (lresp.IsEmptyBody()) {
    cl.Disconnect();
    return common::make_error("Empty body");
  }

  cl.Disconnect();
  return common::Error();
}

common::ErrnoError CreatePipe(int* read_client_fd, int* write_client_fd) {
  if (!read_client_fd || !write_client_fd) {
    return common::make_errno_error_inval();
  }

  int pipefd[2] = {0};
  int res = pipe(pipefd);
  if (res == ERROR_RESULT_VALUE) {
    return common::make_errno_error(errno);
  }

  *read_client_fd = pipefd[0];
  *write_client_fd = pipefd[1];
  return common::ErrnoError();
}

}  // namespace

namespace fastocloud {
namespace {

common::ErrnoError MakeStreamInfo(const utils::ArgsMap& config_args,
                                  bool check_folders,
                                  StreamInfo* sha,
                                  std::string* feedback_dir,
                                  common::logging::LOG_LEVEL* logs_level) {
  if (!sha || !feedback_dir || !logs_level) {
    return common::make_errno_error_inval();
  }

  StreamInfo lsha;
  if (!utils::ArgsGetValue(config_args, ID_FIELD, &lsha.id)) {
    return common::make_errno_error("Define " ID_FIELD " variable and make it valid.", EAGAIN);
  }

  uint8_t type;
  if (!utils::ArgsGetValue(config_args, TYPE_FIELD, &type)) {
    return common::make_errno_error("Define " TYPE_FIELD " variable and make it valid.", EAGAIN);
  }
  lsha.type = static_cast<StreamType>(type);
  if (lsha.type == PROXY) {
    return common::make_errno_error("Proxy streams not handled for now", EINVAL);
  }

  std::string lfeedback_dir;
  if (!utils::ArgsGetValue(config_args, FEEDBACK_DIR_FIELD, &lfeedback_dir)) {
    return common::make_errno_error("Define " FEEDBACK_DIR_FIELD " variable and make it valid.", EAGAIN);
  }

  int llogs_level;
  if (!utils::ArgsGetValue(config_args, LOG_LEVEL_FIELD, &llogs_level)) {
    llogs_level = common::logging::LOG_LEVEL_DEBUG;
  }

  common::ErrnoError errn = utils::CreateAndCheckDir(lfeedback_dir);
  if (errn) {
    return errn;
  }

  input_t input;
  if (!read_input(config_args, &input)) {
    return common::make_errno_error("Define " INPUT_FIELD " variable and make it valid.", EAGAIN);
  }

  for (auto input_uri : input) {
    lsha.input.push_back(input_uri.GetID());
  }

  if (check_folders) {
    bool is_timeshift_rec_or_catchup = type == TIMESHIFT_RECORDER || type == CATCHUP;  // no outputs
    if (is_timeshift_rec_or_catchup) {
      std::string timeshift_dir;
      if (!utils::ArgsGetValue(config_args, TIMESHIFT_DIR_FIELD, &timeshift_dir)) {
        return common::make_errno_error("Define " TIMESHIFT_DIR_FIELD " variable and make it valid.", EAGAIN);
      }

      errn = utils::CreateAndCheckDir(timeshift_dir);
      if (errn) {
        return errn;
      }
    } else {
      output_t output;
      if (!read_output(config_args, &output)) {
        return common::make_errno_error("Define " OUTPUT_FIELD " variable and make it valid.", EAGAIN);
      }

      for (auto out_uri : output) {
        common::uri::Url ouri = out_uri.GetOutput();
        if (ouri.GetScheme() == common::uri::Url::http) {
          const common::file_system::ascii_directory_string_path http_root = out_uri.GetHttpRoot();
          const std::string http_root_str = http_root.GetPath();
          common::ErrnoError errn = utils::CreateAndCheckDir(http_root_str);
          if (errn) {
            return errn;
          }
        }
        lsha.output.push_back(out_uri.GetID());
      }
    }
  }

  *logs_level = static_cast<common::logging::LOG_LEVEL>(llogs_level);
  *feedback_dir = lfeedback_dir;
  *sha = lsha;
  return common::ErrnoError();
}
}  // namespace
namespace server {
namespace {
typedef VodsHandler CodsHandler;
typedef VodsServer CodsServer;

bool CheckIsFullVod(const common::file_system::ascii_file_string_path& file) {
  utils::M3u8Reader reader;
  if (!reader.Parse(file)) {
    return false;
  }

  common::file_system::ascii_directory_string_path dir(file.GetDirectory());
  const auto chunks = reader.GetChunks();
  for (const utils::ChunkInfo& chunk : chunks) {
    const auto chunk_path = dir.MakeFileStringPath(chunk.path);
    if (!chunk_path) {
      return false;
    }

    if (!common::file_system::is_file_exist(chunk_path->GetPath())) {
      return false;
    }
  }

  return true;
}
}  // namespace

struct ProcessSlaveWrapper::NodeStats {
  NodeStats() : prev(), prev_nshot(), gpu_load(0), timestamp(common::time::current_utc_mstime()) {}

  utils::CpuShot prev;
  utils::NetShot prev_nshot;
  int gpu_load;
  fastotv::timestamp_t timestamp;
};

ProcessSlaveWrapper::ProcessSlaveWrapper(const std::string& license_key, const Config& config)
    : config_(config),
      license_key_(license_key),
      process_argc_(0),
      process_argv_(nullptr),
      loop_(nullptr),
      http_server_(nullptr),
      http_handler_(nullptr),
      vods_server_(nullptr),
      vods_handler_(nullptr),
      cods_server_(nullptr),
      cods_handler_(nullptr),
#if defined(SUBSCRIBERS)
      subscribers_server_(nullptr),
      subscribers_handler_(nullptr),
      finder_(nullptr),
#endif
      ping_client_timer_(INVALID_TIMER_ID),
      node_stats_timer_(INVALID_TIMER_ID),
      cleanup_files_timer_(INVALID_TIMER_ID),
      quit_cleanup_timer_(INVALID_TIMER_ID),
      node_stats_(new NodeStats),
      stream_exec_func_(nullptr),
      vods_links_(),
      cods_links_() {
  loop_ = new DaemonServer(config.host, this);
  loop_->SetName("client_server");

  http_handler_ = new HttpHandler(this);
  http_server_ = new HttpServer(config.http_host, http_handler_);
  http_server_->SetName("http_server");

  vods_handler_ = new VodsHandler(this);
  vods_server_ = new VodsServer(config.vods_host, vods_handler_);
  vods_server_->SetName("vods_server");

  cods_handler_ = new CodsHandler(this);
  cods_server_ = new CodsServer(config.cods_host, cods_handler_);
  cods_server_->SetName("cods_server");

#if defined(SUBSCRIBERS)
  finder_ = new SyncFinder;
  subscribers_handler_ = new subscribers::SubscribersHandler(finder_, config.bandwidth_host);
  subscribers_server_ = new subscribers::SubscribersServer(config.subscribers_host, subscribers_handler_);
  subscribers_server_->SetName("subscribers_server");
#endif
}

int ProcessSlaveWrapper::SendStopDaemonRequest(const std::string& license) {
  if (license.empty()) {
    return EXIT_FAILURE;
  }

  const common::net::HostAndPort host = Config::GetDefaultHost();
  common::net::socket_info client_info;
  common::ErrnoError err = common::net::connect(host, common::net::ST_SOCK_STREAM, 0, &client_info);
  if (err) {
    return EXIT_FAILURE;
  }

  std::unique_ptr<ProtocoledDaemonClient> connection(new ProtocoledDaemonClient(nullptr, client_info));
  err = connection->StopMe(license);
  if (err) {
    connection->Close();
    return EXIT_FAILURE;
  }

  connection->Close();
  return EXIT_SUCCESS;
}

ProcessSlaveWrapper::~ProcessSlaveWrapper() {
#if defined(SUBSCRIBERS)
  destroy(&subscribers_server_);
  destroy(&subscribers_handler_);
  destroy(&finder_);
#endif
  destroy(&cods_server_);
  destroy(&cods_handler_);
  destroy(&vods_server_);
  destroy(&vods_handler_);
  destroy(&http_server_);
  destroy(&http_handler_);
  destroy(&loop_);
  destroy(&node_stats_);
}

int ProcessSlaveWrapper::Exec(int argc, char** argv) {
  const std::string absolute_source_dir = common::file_system::absolute_path_from_relative(RELATIVE_SOURCE_DIR);
  const std::string lib_full_path = common::file_system::make_path(absolute_source_dir, CORE_LIBRARY);
  void* handle = dlopen(lib_full_path.c_str(), RTLD_LAZY);
  if (!handle) {
    ERROR_LOG() << "Failed to load " CORE_LIBRARY " path: " << lib_full_path
                << ", error: " << common::common_strerror(errno);
    return EXIT_FAILURE;
  }

  stream_exec_func_ = reinterpret_cast<stream_exec_t>(dlsym(handle, "stream_exec"));
  char* error = dlerror();
  if (error) {
    ERROR_LOG() << "Failed to load start stream function error: " << error;
    dlclose(handle);
    return EXIT_FAILURE;
  }

  process_argc_ = argc;
  process_argv_ = argv;

  // gpu statistic monitor
  std::thread perf_thread;
  gpu_stats::IPerfMonitor* perf_monitor = gpu_stats::CreatePerfMonitor(&node_stats_->gpu_load);
  if (perf_monitor) {
    perf_thread = std::thread([perf_monitor] { perf_monitor->Exec(); });
  }

  HttpServer* http_server = static_cast<HttpServer*>(http_server_);
  std::thread http_thread = std::thread([http_server] {
    common::ErrnoError err = http_server->Bind(true);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    err = http_server->Listen(5);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    int res = http_server->Exec();
    UNUSED(res);
  });

  VodsServer* vods_server = static_cast<VodsServer*>(vods_server_);
  std::thread vods_thread = std::thread([vods_server] {
    common::ErrnoError err = vods_server->Bind(true);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    err = vods_server->Listen(5);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    int res = vods_server->Exec();
    UNUSED(res);
  });

  CodsServer* cods_server = static_cast<CodsServer*>(cods_server_);
  std::thread cods_thread = std::thread([cods_server] {
    common::ErrnoError err = cods_server->Bind(true);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    err = cods_server->Listen(5);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    int res = cods_server->Exec();
    UNUSED(res);
  });

#if defined(SUBSCRIBERS)
  subscribers::SubscribersServer* subscribers_server =
      static_cast<subscribers::SubscribersServer*>(subscribers_server_);
  std::thread subscribers_thread = std::thread([subscribers_server] {
    common::ErrnoError err = subscribers_server->Bind(true);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    err = subscribers_server->Listen(5);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      return;
    }

    int res = subscribers_server->Exec();
    UNUSED(res);
  });
#endif

  int res = EXIT_FAILURE;
  DaemonServer* server = static_cast<DaemonServer*>(loop_);
  common::ErrnoError err = server->Bind(true);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    goto finished;
  }

  err = server->Listen(5);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    goto finished;
  }

  node_stats_->prev = utils::GetMachineCpuShot();
  node_stats_->prev_nshot = utils::GetMachineNetShot();
  node_stats_->timestamp = common::time::current_utc_mstime();
  res = server->Exec();

finished:
#if defined(SUBSCRIBERS)
  subscribers_thread.join();
#endif
  vods_thread.join();
  cods_thread.join();
  http_thread.join();
  if (perf_monitor) {
    perf_monitor->Stop();
  }
  if (perf_thread.joinable()) {
    perf_thread.join();
  }
  delete perf_monitor;
  stream_exec_func_ = nullptr;
  dlclose(handle);
  return res;
}

void ProcessSlaveWrapper::PreLooped(common::libev::IoLoop* server) {
  ping_client_timer_ = server->CreateTimer(ping_timeout_clients_seconds, true);
  node_stats_timer_ = server->CreateTimer(node_stats_send_seconds, true);
  cleanup_files_timer_ = server->CreateTimer(config_.ttl_files, true);
}

void ProcessSlaveWrapper::Accepted(common::libev::IoClient* client) {
  UNUSED(client);
}

void ProcessSlaveWrapper::Moved(common::libev::IoLoop* server, common::libev::IoClient* client) {
  UNUSED(server);
  UNUSED(client);
}

void ProcessSlaveWrapper::Closed(common::libev::IoClient* client) {
  UNUSED(client);
}

void ProcessSlaveWrapper::TimerEmited(common::libev::IoLoop* server, common::libev::timer_id_t id) {
  if (ping_client_timer_ == id) {
    std::vector<common::libev::IoClient*> online_clients = server->GetClients();
    for (size_t i = 0; i < online_clients.size(); ++i) {
      common::libev::IoClient* client = online_clients[i];
      ProtocoledDaemonClient* dclient = dynamic_cast<ProtocoledDaemonClient*>(client);
      if (dclient && dclient->IsVerified()) {
        common::ErrnoError err = dclient->Ping();
        if (err) {
          DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
          ignore_result(dclient->Close());
          delete dclient;
        } else {
          INFO_LOG() << "Sent ping to client[" << client->GetFormatedName() << "], from server["
                     << server->GetFormatedName() << "], " << online_clients.size() << " client(s) connected.";
        }
      }
    }
  } else if (node_stats_timer_ == id) {
    const std::string node_stats = MakeServiceStats(false);
    fastotv::protocol::request_t req;
    common::Error err_ser = StatisitcServiceBroadcast(node_stats, &req);
    if (err_ser) {
      return;
    }

    BroadcastClients(req);
  } else if (cleanup_files_timer_ == id) {
    for (auto it = vods_links_.begin(); it != vods_links_.end(); ++it) {
      utils::RemoveFilesByExtension((*it).first, CHUNK_EXT);
    }
  } else if (quit_cleanup_timer_ == id) {
#if defined(SUBSCRIBERS)
    subscribers_server_->Stop();
#endif
    vods_server_->Stop();
    cods_server_->Stop();
    http_server_->Stop();
    loop_->Stop();
  }
}

#if LIBEV_CHILD_ENABLE
void ProcessSlaveWrapper::Accepted(common::libev::IoChild* child) {
  UNUSED(child);
}

void ProcessSlaveWrapper::Moved(common::libev::IoLoop* server, common::libev::IoChild* child) {
  UNUSED(server);
  UNUSED(child);
}

void ProcessSlaveWrapper::ChildStatusChanged(common::libev::IoChild* child, int status) {
  ChildStream* channel = static_cast<ChildStream*>(child);
  const auto sid = channel->GetStreamID();

  INFO_LOG() << "Successful finished children id: " << sid;
  int stabled_status = EXIT_SUCCESS;
  int signal_number = 0;

  if (WIFEXITED(status)) {
    stabled_status = WEXITSTATUS(status);
  } else {
    stabled_status = EXIT_FAILURE;
  }
  if (WIFSIGNALED(status)) {
    signal_number = WTERMSIG(status);
  }
  INFO_LOG() << "Stream id: " << sid << ", exit with status: " << (stabled_status ? "FAILURE" : "SUCCESS")
             << ", signal: " << signal_number;

  loop_->UnRegisterChild(child);

  StreamStruct* mem = channel->GetMem();
  FreeSharedStreamStruct(&mem);
  DCHECK(!channel->GetClient()) << "In this place client should be nulled.";
  delete channel;

  stream::QuitStatusInfo ch_status_info(sid, !stabled_status, signal_number);  // reverse status
  fastotv::protocol::request_t req;
  common::Error err_ser = QuitStatusStreamBroadcast(ch_status_info, &req);
  if (err_ser) {
    return;
  }

  BroadcastClients(req);
}
#endif

Child* ProcessSlaveWrapper::FindChildByID(stream_id_t cid) const {
  auto childs = loop_->GetChilds();
  for (auto* child : childs) {
    Child* channel = static_cast<Child*>(child);
    if (channel->GetStreamID() == cid) {
      return channel;
    }
  }

  return nullptr;
}

void ProcessSlaveWrapper::BroadcastClients(const fastotv::protocol::request_t& req) {
  std::vector<common::libev::IoClient*> clients = loop_->GetClients();
  for (size_t i = 0; i < clients.size(); ++i) {
    ProtocoledDaemonClient* dclient = dynamic_cast<ProtocoledDaemonClient*>(clients[i]);
    if (dclient && dclient->IsVerified()) {
      common::ErrnoError err = dclient->WriteRequest(req);
      if (err) {
        WARNING_LOG() << "BroadcastClients error: " << err->GetDescription();
      }
    }
  }
}

common::ErrnoError ProcessSlaveWrapper::DaemonDataReceived(ProtocoledDaemonClient* dclient) {
  CHECK(loop_->IsLoopThread());
  std::string input_command;
  common::ErrnoError err = dclient->ReadCommand(&input_command);
  if (err) {
    return err;  // i don't want handle spam, comand must be foramated according
                 // protocol
  }

  fastotv::protocol::request_t* req = nullptr;
  fastotv::protocol::response_t* resp = nullptr;
  common::Error err_parse = common::protocols::json_rpc::ParseJsonRPC(input_command, &req, &resp);
  if (err_parse) {
    const std::string err_str = err_parse->GetDescription();
    return common::make_errno_error(err_str, EAGAIN);
  }

  if (req) {
    DEBUG_LOG() << "Received daemon request: " << input_command;
    err = HandleRequestServiceCommand(dclient, req);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    delete req;
  } else if (resp) {
    DEBUG_LOG() << "Received daemon responce: " << input_command;
    err = HandleResponceServiceCommand(dclient, resp);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    delete resp;
  } else {
    DNOTREACHED();
    return common::make_errno_error("Invalid command type.", EINVAL);
  }

  return common::ErrnoError();
}

common::ErrnoError ProcessSlaveWrapper::PipeDataReceived(pipe::ProtocoledPipeClient* pipe_client) {
  CHECK(loop_->IsLoopThread());
  std::string input_command;
  common::ErrnoError err = pipe_client->ReadCommand(&input_command);
  if (err) {
    return err;  // i don't want handle spam, command must be foramated according
                 // protocol
  }

  fastotv::protocol::request_t* req = nullptr;
  fastotv::protocol::response_t* resp = nullptr;
  common::Error err_parse = common::protocols::json_rpc::ParseJsonRPC(input_command, &req, &resp);
  if (err_parse) {
    const std::string err_str = err_parse->GetDescription();
    return common::make_errno_error(err_str, EAGAIN);
  }

  if (req) {
    INFO_LOG() << "Received stream responce: " << input_command;
    err = HandleRequestStreamsCommand(pipe_client, req);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    delete req;
  } else if (resp) {
    INFO_LOG() << "Received stream responce: " << input_command;
    err = HandleResponceStreamsCommand(pipe_client, resp);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    delete resp;
  } else {
    NOTREACHED();
    return common::make_errno_error("Invalid command type.", EINVAL);
  }
  return common::ErrnoError();
}

void ProcessSlaveWrapper::DataReceived(common::libev::IoClient* client) {
  if (ProtocoledDaemonClient* dclient = dynamic_cast<ProtocoledDaemonClient*>(client)) {
    common::ErrnoError err = DaemonDataReceived(dclient);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      dclient->Close();
      delete dclient;
    }
  } else if (pipe::ProtocoledPipeClient* pipe_client = dynamic_cast<pipe::ProtocoledPipeClient*>(client)) {
    common::ErrnoError err = PipeDataReceived(pipe_client);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      auto childs = loop_->GetChilds();
      for (auto* child : childs) {
        ChildStream* channel = static_cast<ChildStream*>(child);
        if (pipe_client == channel->GetClient()) {
          channel->SetClient(nullptr);
          break;
        }
      }

      pipe_client->Close();
      delete pipe_client;
    }
  } else {
    NOTREACHED();
  }
}

void ProcessSlaveWrapper::DataReadyToWrite(common::libev::IoClient* client) {
  UNUSED(client);
}

void ProcessSlaveWrapper::PostLooped(common::libev::IoLoop* server) {
  if (cleanup_files_timer_ != INVALID_TIMER_ID) {
    server->RemoveTimer(cleanup_files_timer_);
    cleanup_files_timer_ = INVALID_TIMER_ID;
  }

  if (ping_client_timer_ != INVALID_TIMER_ID) {
    server->RemoveTimer(ping_client_timer_);
    ping_client_timer_ = INVALID_TIMER_ID;
  }

  if (node_stats_timer_ != INVALID_TIMER_ID) {
    server->RemoveTimer(node_stats_timer_);
    node_stats_timer_ = INVALID_TIMER_ID;
  }
}

void ProcessSlaveWrapper::OnHttpRequest(common::libev::http::HttpClient* client, const file_path_t& file) {
  if (client->GetServer() == vods_server_) {
    const std::string ext = file.GetExtension();
    if (common::EqualsASCII(ext, M3U8_EXTENSION, false)) {
      loop_->ExecInLoopThread([this, file]() {
        const common::file_system::ascii_directory_string_path http_root(file.GetDirectory());
        auto it = vods_links_.find(http_root);
        if (it == vods_links_.end()) {
          return;
        }

        bool is_full_vod = CheckIsFullVod(file);
        if (!is_full_vod) {
          const serialized_stream_t config = it->second;
          CreateChildStream(config);
        }
      });
    }
  } else if (client->GetServer() == cods_server_) {
    const std::string ext = file.GetExtension();
    if (common::EqualsASCII(ext, M3U8_EXTENSION, false)) {
      loop_->ExecInLoopThread([this, file]() {
        const common::file_system::ascii_directory_string_path http_root(file.GetDirectory());
        auto it = cods_links_.find(http_root);
        if (it == cods_links_.end()) {
          return;
        }

        const serialized_stream_t config = it->second;
        CreateChildStream(config);
      });

      if (!common::file_system::is_file_exist(file.GetPath())) {
        sleep(5);  // may be fix :)
      }
    }
  }
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientStopService(ProtocoledDaemonClient* dclient,
                                                                       fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jstop = json_tokener_parse(params_ptr);
    if (!jstop) {
      return common::make_errno_error_inval();
    }

    service::StopInfo stop_info;
    common::Error err_des = stop_info.DeSerialize(jstop);
    json_object_put(jstop);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    bool is_verified_request = stop_info.GetLicense() == license_key_ || dclient->IsVerified();
    if (!is_verified_request) {
      return common::make_errno_error_inval();
    }

    if (quit_cleanup_timer_ != INVALID_TIMER_ID) {
      // in progress
      return dclient->StopFail(req->id, common::make_error("Stop service in progress..."));
    }

    auto childs = loop_->GetChilds();
    for (auto* child : childs) {
      ChildStream* channel = static_cast<ChildStream*>(child);
      channel->Stop();
    }

    quit_cleanup_timer_ = loop_->CreateTimer(cleanup_seconds, false);
    return dclient->StopSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleResponcePingService(ProtocoledDaemonClient* dclient,
                                                                  fastotv::protocol::response_t* resp) {
  UNUSED(dclient);
  CHECK(loop_->IsLoopThread());
  if (resp->IsMessage()) {
    const char* params_ptr = resp->message->result.c_str();
    json_object* jclient_ping = json_tokener_parse(params_ptr);
    if (!jclient_ping) {
      return common::make_errno_error_inval();
    }

    service::ClientPingInfo client_ping_info;
    common::Error err_des = client_ping_info.DeSerialize(jclient_ping);
    json_object_put(jclient_ping);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }
    return common::ErrnoError();
  }
  return common::ErrnoError();
}

common::ErrnoError ProcessSlaveWrapper::CreateChildStream(const std::string& config) {
  CHECK(loop_->IsLoopThread());
  utils::ArgsMap config_args = options::ValidateConfig(config);
  return CreateChildStream(config_args);
}

common::ErrnoError ProcessSlaveWrapper::CreateChildStream(const serialized_stream_t& config_args) {
  CHECK(loop_->IsLoopThread());
  StreamInfo sha;
  std::string feedback_dir;
  common::logging::LOG_LEVEL logs_level;
  common::ErrnoError err = MakeStreamInfo(config_args, true, &sha, &feedback_dir, &logs_level);
  if (err) {
    return err;
  }

  Child* stream = FindChildByID(sha.id);
  if (stream) {
    NOTICE_LOG() << "Skip request to start stream id: " << sha.id;
    return common::make_errno_error(common::MemSPrintf("Stream with id: %s exist, skip request.", sha.id), EINVAL);
  }

  StreamStruct* mem = nullptr;
  err = AllocSharedStreamStruct(sha, &mem);
  if (err) {
    return err;
  }

  int read_command_client = 0;
  int write_requests_client = 0;
  err = CreatePipe(&read_command_client, &write_requests_client);
  if (err) {
    FreeSharedStreamStruct(&mem);
    return err;
  }

  int read_responce_client = 0;
  int write_responce_client = 0;
  err = CreatePipe(&read_responce_client, &write_responce_client);
  if (err) {
    FreeSharedStreamStruct(&mem);
    return err;
  }

#if !defined(TEST)
  pid_t pid = fork();
#else
  pid_t pid = 0;
#endif
  if (pid == 0) {  // child
    const struct cmd_args client_args = {feedback_dir.c_str(), logs_level, config_.streamlink_path.c_str()};
    const std::string new_process_name = common::MemSPrintf(STREAMER_NAME "_%s", sha.id);
    for (int i = 0; i < process_argc_; ++i) {
      memset(process_argv_[i], 0, strlen(process_argv_[i]));
    }
    const char* new_name = new_process_name.c_str();
    char* app_name = process_argv_[0];
    strncpy(app_name, new_name, new_process_name.length());
    app_name[new_process_name.length()] = 0;
    prctl(PR_SET_NAME, new_name);

#if !defined(TEST)
    // close not needed pipes
    common::ErrnoError errn = common::file_system::close_descriptor(read_responce_client);
    if (errn) {
      DEBUG_MSG_ERROR(errn, common::logging::LOG_LEVEL_WARNING);
    }
    errn = common::file_system::close_descriptor(write_requests_client);
    if (errn) {
      DEBUG_MSG_ERROR(errn, common::logging::LOG_LEVEL_WARNING);
    }
#endif

    pipe::ProtocoledPipeClient* client =
        new pipe::ProtocoledPipeClient(nullptr, read_command_client, write_responce_client);
    client->SetName(sha.id);
    int res = stream_exec_func_(new_name, &client_args, &config_args, client, mem);
    client->Close();
    delete client;
    _exit(res);
  } else if (pid < 0) {
    NOTICE_LOG() << "Failed to start children!";
  } else {
    // close not needed pipes
    common::ErrnoError errn = common::file_system::close_descriptor(read_command_client);
    if (errn) {
      DEBUG_MSG_ERROR(errn, common::logging::LOG_LEVEL_WARNING);
    }
    errn = common::file_system::close_descriptor(write_responce_client);
    if (err) {
      DEBUG_MSG_ERROR(errn, common::logging::LOG_LEVEL_WARNING);
    }

    pipe::ProtocoledPipeClient* pipe_client =
        new pipe::ProtocoledPipeClient(loop_, read_responce_client, write_requests_client);
    pipe_client->SetName(sha.id);
    loop_->RegisterClient(pipe_client);
    ChildStream* new_channel = new ChildStream(loop_, mem);
    new_channel->SetClient(pipe_client);
    loop_->RegisterChild(new_channel, pid);
  }

  return common::ErrnoError();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestChangedSourcesStream(pipe::ProtocoledPipeClient* pclient,
                                                                          fastotv::protocol::request_t* req) {
  UNUSED(pclient);
  CHECK(loop_->IsLoopThread());
  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jrequest_changed_sources = json_tokener_parse(params_ptr);
    if (!jrequest_changed_sources) {
      return common::make_errno_error_inval();
    }

    ChangedSouresInfo ch_sources_info;
    common::Error err_des = ch_sources_info.DeSerialize(jrequest_changed_sources);
    json_object_put(jrequest_changed_sources);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    fastotv::protocol::request_t req;
    common::Error err_ser = ChangedSourcesStreamBroadcast(ch_sources_info, &req);
    if (err_ser) {
      const std::string err_str = err_ser->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    BroadcastClients(req);
    return common::ErrnoError();
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestStatisticStream(pipe::ProtocoledPipeClient* pclient,
                                                                     fastotv::protocol::request_t* req) {
  UNUSED(pclient);
  CHECK(loop_->IsLoopThread());
  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jrequest_stat = json_tokener_parse(params_ptr);
    if (!jrequest_stat) {
      return common::make_errno_error_inval();
    }

    StatisticInfo stat;
    common::Error err_des = stat.DeSerialize(jrequest_stat);
    json_object_put(jrequest_stat);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    fastotv::protocol::request_t req;
    common::Error err_ser = StatisitcStreamBroadcast(stat, &req);
    if (err_ser) {
      const std::string err_str = err_ser->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    BroadcastClients(req);
    return common::ErrnoError();
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientStartStream(ProtocoledDaemonClient* dclient,
                                                                       fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jstart_info = json_tokener_parse(params_ptr);
    if (!jstart_info) {
      return common::make_errno_error_inval();
    }

    stream::StartInfo start_info;
    common::Error err_des = start_info.DeSerialize(jstart_info);
    json_object_put(jstart_info);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    common::ErrnoError err = CreateChildStream(start_info.GetConfig());
    if (err) {
      dclient->StartStreamFail(req->id, common::make_error_from_errno(err));
      return err;
    }

    return dclient->StartStreamSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientStopStream(ProtocoledDaemonClient* dclient,
                                                                      fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jstop_info = json_tokener_parse(params_ptr);
    if (!jstop_info) {
      return common::make_errno_error_inval();
    }

    stream::StopInfo stop_info;
    common::Error err_des = stop_info.DeSerialize(jstop_info);
    json_object_put(jstop_info);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    Child* chan = FindChildByID(stop_info.GetStreamID());
    if (!chan) {
      return dclient->StopFail(req->id, common::make_error("Stream not found"));
    }

    chan->Stop();
    return dclient->StopStreamSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientRestartStream(ProtocoledDaemonClient* dclient,
                                                                         fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jrestart_info = json_tokener_parse(params_ptr);
    if (!jrestart_info) {
      return common::make_errno_error_inval();
    }

    stream::RestartInfo restart_info;
    common::Error err_des = restart_info.DeSerialize(jrestart_info);
    json_object_put(jrestart_info);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    Child* chan = FindChildByID(restart_info.GetStreamID());
    if (!chan) {
      return dclient->ReStartStreamFail(req->id, common::make_error("Stream not found"));
    }

    chan->Restart();
    return dclient->ReStartStreamSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientGetLogStream(ProtocoledDaemonClient* dclient,
                                                                        fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jgetlog_info = json_tokener_parse(params_ptr);
    if (!jgetlog_info) {
      return common::make_errno_error_inval();
    }

    stream::GetLogInfo log_info;
    common::Error err_des = log_info.DeSerialize(jgetlog_info);
    json_object_put(jgetlog_info);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    const auto remote_log_path = log_info.GetLogPath();
    if (remote_log_path.GetScheme() == common::uri::Url::http) {
      const auto stream_log_file = MakeStreamLogPath(log_info.GetFeedbackDir());
      if (stream_log_file) {
        PostHttpFile(*stream_log_file, remote_log_path);
      }
    } else if (remote_log_path.GetScheme() == common::uri::Url::https) {
    }
    return dclient->GetLogStreamSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientGetPipelineStream(ProtocoledDaemonClient* dclient,
                                                                             fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jgetlog_info = json_tokener_parse(params_ptr);
    if (!jgetlog_info) {
      return common::make_errno_error_inval();
    }

    stream::GetLogInfo log_info;
    common::Error err_des = log_info.DeSerialize(jgetlog_info);
    json_object_put(jgetlog_info);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    const auto remote_log_path = log_info.GetLogPath();
    if (remote_log_path.GetScheme() == common::uri::Url::http) {
      const auto stream_log_file = MakeStreamPipelinePath(log_info.GetFeedbackDir());
      if (stream_log_file) {
        PostHttpFile(*stream_log_file, remote_log_path);
      }
    } else if (remote_log_path.GetScheme() == common::uri::Url::https) {
    }
    return dclient->GetLogStreamSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientPrepareService(ProtocoledDaemonClient* dclient,
                                                                          fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jservice_state = json_tokener_parse(params_ptr);
    if (!jservice_state) {
      return common::make_errno_error_inval();
    }

    service::PrepareInfo state_info;
    common::Error err_des = state_info.DeSerialize(jservice_state);
    json_object_put(jservice_state);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    const auto http_root = HttpHandler::http_directory_path_t(state_info.GetHlsDirectory());
    static_cast<HttpHandler*>(http_handler_)->SetHttpRoot(http_root);

    const auto vods_root = VodsHandler::http_directory_path_t(state_info.GetVodsDirectory());
    static_cast<VodsHandler*>(vods_handler_)->SetHttpRoot(vods_root);

    const auto cods_root = CodsHandler::http_directory_path_t(state_info.GetCodsDirectory());
    static_cast<CodsHandler*>(cods_handler_)->SetHttpRoot(cods_root);

    service::Directories dirs(state_info);
    std::string resp_str = service::MakeDirectoryResponce(dirs);
    return dclient->StateServiceSuccess(req->id, resp_str);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientSyncService(ProtocoledDaemonClient* dclient,
                                                                       fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jservice_state = json_tokener_parse(params_ptr);
    if (!jservice_state) {
      return common::make_errno_error_inval();
    }

    service::SyncInfo sync_info;
    common::Error err_des = sync_info.DeSerialize(jservice_state);
    json_object_put(jservice_state);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    // refresh vods
    vods_links_.clear();
    cods_links_.clear();
    for (const std::string& config : sync_info.GetStreams()) {
      AddStreamLine(config);
    }

#if defined(SUBSCRIBERS)
    // refresh subscribers
    SyncFinder* sfinder = static_cast<SyncFinder*>(finder_);
    sfinder->Clear();
    for (const std::string& user : sync_info.GetUsers()) {
      subscribers::commands_info::UserInfo uinf;
      common::Error err = uinf.DeSerializeFromString(user);
      if (!err) {
        sfinder->AddUser(uinf);
      }
    }
#endif

    return dclient->SyncServiceSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

void ProcessSlaveWrapper::AddStreamLine(const std::string& config) {
  CHECK(loop_->IsLoopThread());
  serialized_stream_t config_args = options::ValidateConfig(config);
  StreamInfo sha;
  std::string feedback_dir;
  common::logging::LOG_LEVEL logs_level;
  common::ErrnoError err = MakeStreamInfo(config_args, false, &sha, &feedback_dir, &logs_level);
  if (err) {
    return;
  }

  if (sha.type == VOD_ENCODE || sha.type == VOD_RELAY) {
    output_t output;
    if (read_output(config_args, &output)) {
      for (const OutputUri& out_uri : output) {
        common::uri::Url ouri = out_uri.GetOutput();
        if (ouri.GetScheme() == common::uri::Url::http) {
          const common::file_system::ascii_directory_string_path http_root = out_uri.GetHttpRoot();
          config_args[CLEANUP_TS_FIELD] = common::ConvertToString(false);
          vods_links_[http_root] = config_args;
        }
      }
    }
  } else if (sha.type == COD_ENCODE || sha.type == COD_RELAY) {
    output_t output;
    if (read_output(config_args, &output)) {
      for (const OutputUri& out_uri : output) {
        common::uri::Url ouri = out_uri.GetOutput();
        if (ouri.GetScheme() == common::uri::Url::http) {
          const common::file_system::ascii_directory_string_path http_root = out_uri.GetHttpRoot();
          cods_links_[http_root] = config_args;
        }
      }
    }
  }
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientActivate(ProtocoledDaemonClient* dclient,
                                                                    fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jactivate = json_tokener_parse(params_ptr);
    if (!jactivate) {
      return common::make_errno_error_inval();
    }

    service::ActivateInfo activate_info;
    common::Error err_des = activate_info.DeSerialize(jactivate);
    json_object_put(jactivate);
    if (err_des) {
      ignore_result(dclient->ActivateFail(req->id, err_des));
      return common::make_errno_error(err_des->GetDescription(), EAGAIN);
    }

    bool is_active = activate_info.GetLicense() == license_key_;
    if (!is_active) {
      common::Error err = common::make_error("Wrong license key");
      ignore_result(dclient->ActivateFail(req->id, err));
      return common::make_errno_error(err->GetDescription(), EINVAL);
    }

    const std::string node_stats = MakeServiceStats(true);
    common::ErrnoError err_ser = dclient->ActivateSuccess(req->id, node_stats);
    if (err_ser) {
      return err_ser;
    }

    dclient->SetVerified(true);
    return common::ErrnoError();
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientPingService(ProtocoledDaemonClient* dclient,
                                                                       fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jstop = json_tokener_parse(params_ptr);
    if (!jstop) {
      return common::make_errno_error_inval();
    }

    service::ClientPingInfo client_ping_info;
    common::Error err_des = client_ping_info.DeSerialize(jstop);
    json_object_put(jstop);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    return dclient->Pong(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientGetLogService(ProtocoledDaemonClient* dclient,
                                                                         fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jlog = json_tokener_parse(params_ptr);
    if (!jlog) {
      return common::make_errno_error_inval();
    }

    service::GetLogInfo get_log_info;
    common::Error err_des = get_log_info.DeSerialize(jlog);
    json_object_put(jlog);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    const auto remote_log_path = get_log_info.GetLogPath();
    if (remote_log_path.GetScheme() == common::uri::Url::http) {
      PostHttpFile(common::file_system::ascii_file_string_path(config_.log_path), remote_log_path);
    } else if (remote_log_path.GetScheme() == common::uri::Url::https) {
    }

    return dclient->GetLogServiceSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestServiceCommand(ProtocoledDaemonClient* dclient,
                                                                    fastotv::protocol::request_t* req) {
  if (req->method == DAEMON_START_STREAM) {
    return HandleRequestClientStartStream(dclient, req);
  } else if (req->method == DAEMON_STOP_STREAM) {
    return HandleRequestClientStopStream(dclient, req);
  } else if (req->method == DAEMON_RESTART_STREAM) {
    return HandleRequestClientRestartStream(dclient, req);
  } else if (req->method == DAEMON_GET_LOG_STREAM) {
    return HandleRequestClientGetLogStream(dclient, req);
  } else if (req->method == DAEMON_GET_PIPELINE_STREAM) {
    return HandleRequestClientGetPipelineStream(dclient, req);
  } else if (req->method == DAEMON_PREPARE_SERVICE) {
    return HandleRequestClientPrepareService(dclient, req);
  } else if (req->method == DAEMON_SYNC_SERVICE) {
    return HandleRequestClientSyncService(dclient, req);
  } else if (req->method == DAEMON_STOP_SERVICE) {
    return HandleRequestClientStopService(dclient, req);
  } else if (req->method == DAEMON_ACTIVATE) {
    return HandleRequestClientActivate(dclient, req);
  } else if (req->method == DAEMON_PING_SERVICE) {
    return HandleRequestClientPingService(dclient, req);
  } else if (req->method == DAEMON_GET_LOG_SERVICE) {
    return HandleRequestClientGetLogService(dclient, req);
  }

  WARNING_LOG() << "Received unknown method: " << req->method;
  return common::ErrnoError();
}

common::ErrnoError ProcessSlaveWrapper::HandleResponceServiceCommand(ProtocoledDaemonClient* dclient,
                                                                     fastotv::protocol::response_t* resp) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  fastotv::protocol::request_t req;
  if (dclient->PopRequestByID(resp->id, &req)) {
    if (req.method == DAEMON_SERVER_PING) {
      HandleResponcePingService(dclient, resp);
    } else {
      WARNING_LOG() << "HandleResponceServiceCommand not handled command: " << req.method;
    }
  }

  return common::ErrnoError();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestStreamsCommand(pipe::ProtocoledPipeClient* pclient,
                                                                    fastotv::protocol::request_t* req) {
  if (req->method == CHANGED_SOURCES_STREAM) {
    return HandleRequestChangedSourcesStream(pclient, req);
  } else if (req->method == STATISTIC_STREAM) {
    return HandleRequestStatisticStream(pclient, req);
  }

  WARNING_LOG() << "Received unknown command: " << req->method;
  return common::ErrnoError();
}

common::ErrnoError ProcessSlaveWrapper::HandleResponceStreamsCommand(pipe::ProtocoledPipeClient* pclient,
                                                                     fastotv::protocol::response_t* resp) {
  fastotv::protocol::request_t req;
  if (pclient->PopRequestByID(resp->id, &req)) {
    if (req.method == STOP_STREAM) {
    } else if (req.method == RESTART_STREAM) {
    } else {
      WARNING_LOG() << "HandleResponceStreamsCommand not handled command: " << req.method;
    }
  }
  return common::ErrnoError();
}

std::string ProcessSlaveWrapper::MakeServiceStats(bool full_stat) const {
  utils::CpuShot next = utils::GetMachineCpuShot();
  long double cpu_load = utils::GetCpuMachineLoad(node_stats_->prev, next);
  node_stats_->prev = next;

  utils::NetShot next_nshot = utils::GetMachineNetShot();
  uint64_t bytes_recv = (next_nshot.bytes_recv - node_stats_->prev_nshot.bytes_recv);
  uint64_t bytes_send = (next_nshot.bytes_send - node_stats_->prev_nshot.bytes_send);
  node_stats_->prev_nshot = next_nshot;

  utils::MemoryShot mem_shot = utils::GetMachineMemoryShot();
  utils::HddShot hdd_shot = utils::GetMachineHddShot();
  utils::SysinfoShot sshot = utils::GetMachineSysinfoShot();
  std::string uptime_str = common::MemSPrintf("%lu %lu %lu", sshot.loads[0], sshot.loads[1], sshot.loads[2]);
  fastotv::timestamp_t current_time = common::time::current_utc_mstime();
  fastotv::timestamp_t ts_diff = (current_time - node_stats_->timestamp) / 1000;
  if (ts_diff == 0) {
    ts_diff = 1;  // divide by zero
  }
  node_stats_->timestamp = current_time;

  size_t daemons_client_count = 0;
  std::vector<common::libev::IoClient*> clients = loop_->GetClients();
  for (size_t i = 0; i < clients.size(); ++i) {
    ProtocoledDaemonClient* dclient = dynamic_cast<ProtocoledDaemonClient*>(clients[i]);
    if (dclient && dclient->IsVerified()) {
      daemons_client_count++;
    }
  }
  service::OnlineUsers online(daemons_client_count, static_cast<HttpHandler*>(http_handler_)->GetOnlineClients(),
                              static_cast<HttpHandler*>(vods_handler_)->GetOnlineClients(),
                              static_cast<HttpHandler*>(cods_handler_)->GetOnlineClients());
#if defined(SUBSCRIBERS)
  online.SetSubscribers(static_cast<HttpHandler*>(subscribers_handler_)->GetOnlineClients());
#endif
  service::ServerInfo stat(cpu_load * 100, node_stats_->gpu_load, uptime_str, mem_shot, hdd_shot, bytes_recv / ts_diff,
                           bytes_send / ts_diff, sshot, current_time, online);

  std::string node_stats;
  if (full_stat) {
    service::FullServiceInfo fstat(config_.http_host, config_.vods_host, config_.cods_host, stat);
#if defined(SUBSCRIBERS)
    fstat.SetSubscribersHost(config_.subscribers_host);
    fstat.SetBandwidthHost(config_.bandwidth_host);
#endif
    common::Error err_ser = fstat.SerializeToString(&node_stats);
    if (err_ser) {
      const std::string err_str = err_ser->GetDescription();
      WARNING_LOG() << "Failed to generate node full statistic: " << err_str;
    }
  } else {
    common::Error err_ser = stat.SerializeToString(&node_stats);
    if (err_ser) {
      const std::string err_str = err_ser->GetDescription();
      WARNING_LOG() << "Failed to generate node statistic: " << err_str;
    }
  }
  return node_stats;
}

}  // namespace server
}  // namespace fastocloud
