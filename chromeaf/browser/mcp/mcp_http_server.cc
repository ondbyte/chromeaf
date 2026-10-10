// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/mcp/mcp_http_server.h"

#include <cstdio>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/message_loop/message_pump_type.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread.h"
#include "base/uuid.h"
#include "chromeaf/browser/mcp/mcp_protocol.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/http/http_status_code.h"
#include "net/log/net_log_source.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"

namespace chromeaf {

namespace {

constexpr char kMcpPath[] = "/mcp";
constexpr char kSessionHeader[] = "Mcp-Session-Id";
constexpr int kListenBacklog = 16;
// Screenshots and full-page HTML can be large.
constexpr int32_t kSendBufferSize = 256 * 1024 * 1024;
constexpr int32_t kReceiveBufferSize = 16 * 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("chromeaf_mcp_server", R"(
      semantics {
        sender: "chromeaf MCP server"
        description:
          "Local Model Context Protocol endpoint that lets an AI agent or a "
          "script control this browser instance (tabs, page content, "
          "screenshots, synthetic input)."
        trigger:
          "A client connects to the port given by --chromeaf-mcp-port."
        data: "Page markup, text, screenshots and tab metadata."
        destination: LOCAL
      }
      policy {
        cookies_allowed: NO
        setting:
          "Only active when the browser is launched with "
          "--chromeaf-mcp-port."
        policy_exception_justification: "Opt-in local control interface."
      })");

// Rejects requests whose Host/Origin is not a loopback name or an IP literal,
// which defeats DNS-rebinding attacks from web pages.
bool IsAllowedHostName(std::string_view host) {
  if (base::EqualsCaseInsensitiveASCII(host, "localhost")) {
    return true;
  }
  net::IPAddress address;
  if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  return address.AssignFromIPLiteral(host);
}

bool IsRequestHostAllowed(const net::HttpServerRequestInfo& info) {
  std::string host = info.GetHeaderValue("host");
  if (host.empty()) {
    return true;  // HTTP/1.0 client.
  }
  GURL url("http://" + host);
  return url.is_valid() && IsAllowedHostName(url.host());
}

bool IsOriginAllowed(const net::HttpServerRequestInfo& info) {
  std::string origin = info.GetHeaderValue("origin");
  if (origin.empty()) {
    return true;  // Non-browser client.
  }
  GURL url(origin);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  return url.host() == "localhost" || url.HostIsIPAddress();
}

}  // namespace

// Core -----------------------------------------------------------------------

class McpHttpServer::Core : public net::HttpServer::Delegate {
 public:
  using MessageHandler =
      base::RepeatingCallback<void(std::string body, ReplyCallback reply)>;

  Core(Options options, MessageHandler handler)
      : options_(std::move(options)), handler_(std::move(handler)) {}
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;
  ~Core() override = default;

  void Start() {
    auto socket = std::make_unique<net::TCPServerSocket>(nullptr,
                                                         net::NetLogSource());
    int rv = socket->ListenWithAddressAndPort(options_.host, options_.port,
                                              kListenBacklog);
    if (rv != net::OK) {
      LOG(ERROR) << "chromeaf: cannot start MCP server on " << options_.host
                 << ":" << options_.port << ": " << net::ErrorToString(rv);
      return;
    }
    server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
    net::IPEndPoint address;
    if (server_->GetLocalAddress(&address) == net::OK) {
      fprintf(stderr, "\nchromeaf MCP server listening on http://%s%s\n",
              address.ToString().c_str(), kMcpPath);
      fflush(stderr);
    }
  }

  // net::HttpServer::Delegate:
  void OnConnect(int connection_id) override {
    server_->SetSendBufferSize(connection_id, kSendBufferSize);
    server_->SetReceiveBufferSize(connection_id, kReceiveBufferSize);
  }

  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override {
    if (!IsRequestHostAllowed(info) || !IsOriginAllowed(info)) {
      SendError(connection_id, net::HTTP_FORBIDDEN, "Forbidden origin/host");
      return;
    }
    if (!options_.token.empty() &&
        info.GetHeaderValue("authorization") !=
            base::StrCat({"Bearer ", options_.token})) {
      net::HttpServerResponseInfo response(net::HTTP_UNAUTHORIZED);
      response.AddHeader("WWW-Authenticate", "Bearer");
      response.SetBody("Unauthorized", "text/plain");
      server_->SendResponse(connection_id, response, kTrafficAnnotation);
      return;
    }

    std::string_view path = info.path;
    path = path.substr(0, path.find('?'));

    if (path == "/" && info.method == "GET") {
      server_->Send200(connection_id,
                       R"({"name":"chromeaf","mcp_endpoint":"/mcp"})",
                       "application/json", kTrafficAnnotation);
      return;
    }
    if (path != kMcpPath) {
      server_->Send404(connection_id, kTrafficAnnotation);
      return;
    }

    if (info.method == "POST") {
      bool is_initialize = McpProtocol::IsInitializeRequest(info.data);
      handler_.Run(info.data,
                   base::BindPostTaskToCurrentDefault(base::BindOnce(
                       &Core::SendReply, weak_factory_.GetWeakPtr(),
                       connection_id, is_initialize)));
      return;
    }
    if (info.method == "DELETE") {
      // Session termination; sessions carry no server-side state.
      server_->Send200(connection_id, "", "text/plain", kTrafficAnnotation);
      return;
    }
    // No server-initiated SSE stream is offered (allowed by the spec).
    net::HttpServerResponseInfo response(net::HTTP_METHOD_NOT_ALLOWED);
    response.AddHeader("Allow", "POST, DELETE");
    response.SetBody("", "text/plain");
    server_->SendResponse(connection_id, response, kTrafficAnnotation);
  }

  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override {
    server_->Close(connection_id);
  }
  void OnWebSocketMessage(int connection_id, std::string data) override {}
  void OnClose(int connection_id) override {}

 private:
  void SendReply(int connection_id,
                 bool is_initialize,
                 std::optional<std::string> body) {
    if (!server_) {
      return;
    }
    if (!body) {
      net::HttpServerResponseInfo response(net::HTTP_ACCEPTED);
      response.SetBody("", "text/plain");
      server_->SendResponse(connection_id, response, kTrafficAnnotation);
      return;
    }
    net::HttpServerResponseInfo response(net::HTTP_OK);
    if (is_initialize) {
      response.AddHeader(kSessionHeader,
                         base::Uuid::GenerateRandomV4().AsLowercaseString());
    }
    response.AddHeader("Cache-Control", "no-store");
    response.SetBody(*body, "application/json");
    server_->SendResponse(connection_id, response, kTrafficAnnotation);
  }

  void SendError(int connection_id,
                 net::HttpStatusCode code,
                 std::string_view message) {
    net::HttpServerResponseInfo response(code);
    response.SetBody(message, "text/plain");
    server_->SendResponse(connection_id, response, kTrafficAnnotation);
  }

  const Options options_;
  const MessageHandler handler_;
  std::unique_ptr<net::HttpServer> server_;
  base::WeakPtrFactory<Core> weak_factory_{this};
};

// McpHttpServer --------------------------------------------------------------

McpHttpServer::McpHttpServer(Options options, McpProtocol* protocol)
    : options_(std::move(options)),
      protocol_(protocol),
      core_(nullptr, base::OnTaskRunnerDeleter(nullptr)) {}

McpHttpServer::~McpHttpServer() {
  if (!thread_) {
    return;
  }
  core_.reset();  // Deleted on |thread_|.
  // Joining the thread blocks, which is not allowed on the UI thread.
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::WithBaseSyncPrimitives(), base::TaskPriority::BEST_EFFORT},
      base::DoNothingWithBoundArgs(std::move(thread_)));
}

void McpHttpServer::Start() {
  CHECK(!thread_);
  thread_ = std::make_unique<base::Thread>("chromeaf_mcp_server");
  base::Thread::Options thread_options(base::MessagePumpType::IO, 0);
  if (!thread_->StartWithOptions(std::move(thread_options))) {
    LOG(ERROR) << "chromeaf: failed to start MCP server thread";
    thread_.reset();
    return;
  }

  // Messages hop to the UI thread; replies hop back via BindPostTask in Core.
  Core::MessageHandler handler = base::BindPostTaskToCurrentDefault(
      base::BindRepeating(&McpHttpServer::HandleMessageOnUI,
                          weak_factory_.GetWeakPtr()));
  scoped_refptr<base::SingleThreadTaskRunner> runner = thread_->task_runner();
  core_ = std::unique_ptr<Core, base::OnTaskRunnerDeleter>(
      new Core(options_, std::move(handler)),
      base::OnTaskRunnerDeleter(runner));
  runner->PostTask(FROM_HERE, base::BindOnce(&Core::Start,
                                             base::Unretained(core_.get())));
}

void McpHttpServer::HandleMessageOnUI(std::string body, ReplyCallback reply) {
  protocol_->HandleMessage(body, std::move(reply));
}

}  // namespace chromeaf
