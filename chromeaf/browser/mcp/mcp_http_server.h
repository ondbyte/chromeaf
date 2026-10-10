// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_MCP_MCP_HTTP_SERVER_H_
#define CHROMEAF_BROWSER_MCP_MCP_HTTP_SERVER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"

namespace base {
class Thread;
}

namespace chromeaf {

class McpProtocol;

// Serves MCP "Streamable HTTP" (POST /mcp, JSON responses) using //net's
// minimal HttpServer on a dedicated IO thread. Requests are handed to the
// McpProtocol on the UI thread. Created, used and destroyed on the UI thread.
class McpHttpServer {
 public:
  struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    // When non-empty, requests must carry "Authorization: Bearer <token>".
    std::string token;
  };

  McpHttpServer(Options options, McpProtocol* protocol);
  McpHttpServer(const McpHttpServer&) = delete;
  McpHttpServer& operator=(const McpHttpServer&) = delete;
  ~McpHttpServer();

  // Starts listening asynchronously.
  void Start();

 private:
  class Core;

  using ReplyCallback =
      base::OnceCallback<void(std::optional<std::string> body)>;

  // Bound to the UI thread; called by Core for each POSTed JSON-RPC message.
  void HandleMessageOnUI(std::string body, ReplyCallback reply);

  const Options options_;
  const raw_ptr<McpProtocol> protocol_;
  std::unique_ptr<base::Thread> thread_;
  // Lives on |thread_|.
  std::unique_ptr<Core, base::OnTaskRunnerDeleter> core_;
  base::WeakPtrFactory<McpHttpServer> weak_factory_{this};
};

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_MCP_MCP_HTTP_SERVER_H_
