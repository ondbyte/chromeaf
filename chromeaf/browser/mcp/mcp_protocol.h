// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_MCP_MCP_PROTOCOL_H_
#define CHROMEAF_BROWSER_MCP_MCP_PROTOCOL_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"

namespace chromeaf {

class ToolRegistry;

// Implements the Model Context Protocol server side on top of JSON-RPC 2.0:
// initialize, notifications/initialized, ping, tools/list and tools/call.
// Transport-agnostic and single-threaded (runs on the UI thread).
class McpProtocol {
 public:
  // The newest protocol revision we speak. Older revisions requested by a
  // client in `initialize` are echoed back if supported.
  static constexpr char kLatestProtocolVersion[] = "2025-06-18";

  // JSON-RPC error codes.
  static constexpr int kParseError = -32700;
  static constexpr int kInvalidRequest = -32600;
  static constexpr int kMethodNotFound = -32601;
  static constexpr int kInvalidParams = -32602;
  static constexpr int kInternalError = -32603;

  // Called with the serialized JSON-RPC response, or std::nullopt when the
  // message was a notification (or a response) and needs no reply.
  using ResponseCallback =
      base::OnceCallback<void(std::optional<std::string> response)>;

  McpProtocol(const ToolRegistry* registry, base::TimeDelta tool_timeout);
  McpProtocol(const McpProtocol&) = delete;
  McpProtocol& operator=(const McpProtocol&) = delete;
  ~McpProtocol();

  void HandleMessage(std::string_view body, ResponseCallback callback);

  // Whether |body| is an `initialize` request (the transport issues a session
  // id for those).
  static bool IsInitializeRequest(std::string_view body);

 private:
  void HandleRequest(base::Value id,
                     const std::string& method,
                     const base::DictValue* params,
                     ResponseCallback callback);
  void HandleToolsCall(base::Value id,
                       const base::DictValue* params,
                       ResponseCallback callback);

  const raw_ptr<const ToolRegistry> registry_;
  const base::TimeDelta tool_timeout_;
  base::WeakPtrFactory<McpProtocol> weak_factory_{this};
};

// Serialization helpers (exposed for tests).
std::string MakeJsonRpcResult(const base::Value& id, base::Value result);
std::string MakeJsonRpcError(const base::Value& id,
                             int code,
                             std::string_view message);

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_MCP_MCP_PROTOCOL_H_
