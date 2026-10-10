// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/mcp/mcp_protocol.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chromeaf/browser/mcp/tool_registry.h"

namespace chromeaf {

namespace {

constexpr const char* kSupportedProtocolVersions[] = {
    McpProtocol::kLatestProtocolVersion,
    "2025-03-26",
    "2024-11-05",
};

constexpr char kServerName[] = "chromeaf";
constexpr char kServerVersion[] = "1.0.0";

std::string Serialize(base::DictValue message) {
  return base::WriteJson(message).value_or("{}");
}

// Ensures the reply for a tools/call is sent exactly once, whether the tool
// finishes or the timeout fires first.
class PendingToolCall : public base::RefCounted<PendingToolCall> {
 public:
  PendingToolCall(base::Value id, McpProtocol::ResponseCallback callback)
      : id_(std::move(id)), callback_(std::move(callback)) {}

  void Finish(ToolResult result) {
    if (!callback_) {
      return;
    }
    std::move(callback_).Run(
        MakeJsonRpcResult(id_, base::Value(std::move(result).ToValue())));
  }

  void TimedOut(base::TimeDelta timeout) {
    Finish(ToolResult::Error(base::StrCat(
        {"Tool call timed out after ",
         base::NumberToString(timeout.InSeconds()), "s"})));
  }

 private:
  friend class base::RefCounted<PendingToolCall>;
  ~PendingToolCall() = default;

  const base::Value id_;
  McpProtocol::ResponseCallback callback_;
};

// JSON-RPC ids are strings or numbers.
bool IsValidId(const base::Value& id) {
  return id.is_string() || id.is_int() || id.is_double();
}

}  // namespace

std::string MakeJsonRpcResult(const base::Value& id, base::Value result) {
  return Serialize(base::DictValue()
                       .Set("jsonrpc", "2.0")
                       .Set("id", id.Clone())
                       .Set("result", std::move(result)));
}

std::string MakeJsonRpcError(const base::Value& id,
                             int code,
                             std::string_view message) {
  return Serialize(
      base::DictValue()
          .Set("jsonrpc", "2.0")
          .Set("id", id.Clone())
          .Set("error",
               base::DictValue().Set("code", code).Set("message", message)));
}

McpProtocol::McpProtocol(const ToolRegistry* registry,
                         base::TimeDelta tool_timeout)
    : registry_(registry), tool_timeout_(tool_timeout) {}

McpProtocol::~McpProtocol() = default;

// static
bool McpProtocol::IsInitializeRequest(std::string_view body) {
  std::optional<base::DictValue> message =
      base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
  if (!message) {
    return false;
  }
  const std::string* method = message->FindString("method");
  return method && *method == "initialize" && message->Find("id");
}

void McpProtocol::HandleMessage(std::string_view body,
                                ResponseCallback callback) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!parsed) {
    std::move(callback).Run(
        MakeJsonRpcError(base::Value(), kParseError, "Parse error"));
    return;
  }
  base::DictValue* message = parsed->GetIfDict();
  if (!message) {
    // JSON-RPC batches were removed from MCP in 2025-06-18.
    std::move(callback).Run(MakeJsonRpcError(
        base::Value(), kInvalidRequest, "Expected a single JSON-RPC object"));
    return;
  }

  const std::string* jsonrpc = message->FindString("jsonrpc");
  const std::string* method = message->FindString("method");
  base::Value* id = message->Find("id");

  if (!method) {
    // A response to a server->client request (we never send any) or junk.
    if (id && (message->Find("result") || message->Find("error"))) {
      std::move(callback).Run(std::nullopt);
      return;
    }
    std::move(callback).Run(MakeJsonRpcError(
        id ? id->Clone() : base::Value(), kInvalidRequest, "Missing method"));
    return;
  }
  if (!jsonrpc || *jsonrpc != "2.0") {
    std::move(callback).Run(MakeJsonRpcError(
        id ? id->Clone() : base::Value(), kInvalidRequest,
        "jsonrpc must be \"2.0\""));
    return;
  }

  if (!id) {
    // Notifications (notifications/initialized, notifications/cancelled, ...)
    // need no response.
    std::move(callback).Run(std::nullopt);
    return;
  }
  if (!IsValidId(*id)) {
    std::move(callback).Run(
        MakeJsonRpcError(base::Value(), kInvalidRequest, "Invalid id"));
    return;
  }

  const base::Value* params_value = message->Find("params");
  const base::DictValue* params =
      params_value ? params_value->GetIfDict() : nullptr;
  if (params_value && !params) {
    std::move(callback).Run(
        MakeJsonRpcError(*id, kInvalidParams, "params must be an object"));
    return;
  }

  HandleRequest(id->Clone(), *method, params, std::move(callback));
}

void McpProtocol::HandleRequest(base::Value id,
                                const std::string& method,
                                const base::DictValue* params,
                                ResponseCallback callback) {
  if (method == "initialize") {
    std::string version = kLatestProtocolVersion;
    if (params) {
      if (const std::string* requested =
              params->FindString("protocolVersion")) {
        for (const char* supported : kSupportedProtocolVersions) {
          if (*requested == supported) {
            version = *requested;
            break;
          }
        }
      }
    }
    base::DictValue result;
    result.Set("protocolVersion", version);
    result.Set("capabilities",
               base::DictValue().Set(
                   "tools", base::DictValue().Set("listChanged", false)));
    result.Set("serverInfo", base::DictValue()
                                 .Set("name", kServerName)
                                 .Set("title", "chromeaf browser")
                                 .Set("version", kServerVersion));
    result.Set("instructions",
               "Controls a running chromeaf browser. Coordinates are CSS/DIP "
               "pixels relative to the top-left of the tab's viewport. Use "
               "get-clickables to find targets and their centers, then "
               "move-cursor/click. Tools act on the active tab unless tab_id "
               "is given (see get-tabs).");
    std::move(callback).Run(
        MakeJsonRpcResult(id, base::Value(std::move(result))));
    return;
  }

  if (method == "ping") {
    std::move(callback).Run(
        MakeJsonRpcResult(id, base::Value(base::DictValue())));
    return;
  }

  if (method == "tools/list") {
    std::move(callback).Run(MakeJsonRpcResult(
        id, base::Value(base::DictValue().Set("tools", registry_->Describe()))));
    return;
  }

  if (method == "tools/call") {
    HandleToolsCall(std::move(id), params, std::move(callback));
    return;
  }

  // Optional MCP features we don't provide; answer with empty lists so
  // generic clients don't treat the server as broken.
  if (method == "resources/list") {
    std::move(callback).Run(MakeJsonRpcResult(
        id,
        base::Value(base::DictValue().Set("resources", base::ListValue()))));
    return;
  }
  if (method == "prompts/list") {
    std::move(callback).Run(MakeJsonRpcResult(
        id, base::Value(base::DictValue().Set("prompts", base::ListValue()))));
    return;
  }

  std::move(callback).Run(MakeJsonRpcError(
      id, kMethodNotFound, base::StrCat({"Method not found: ", method})));
}

void McpProtocol::HandleToolsCall(base::Value id,
                                  const base::DictValue* params,
                                  ResponseCallback callback) {
  const std::string* name = params ? params->FindString("name") : nullptr;
  if (!name) {
    std::move(callback).Run(
        MakeJsonRpcError(id, kInvalidParams, "Missing tool name"));
    return;
  }
  const ToolDefinition* tool = registry_->Find(*name);
  if (!tool) {
    std::move(callback).Run(MakeJsonRpcError(
        id, kInvalidParams, base::StrCat({"Unknown tool: ", *name})));
    return;
  }

  base::DictValue arguments;
  if (const base::Value* args_value = params->Find("arguments")) {
    if (!args_value->is_dict()) {
      std::move(callback).Run(
          MakeJsonRpcError(id, kInvalidParams, "arguments must be an object"));
      return;
    }
    arguments = args_value->GetDict().Clone();
  }

  if (const base::ListValue* required =
          tool->input_schema.FindList("required")) {
    for (const base::Value& key : *required) {
      if (key.is_string() && !arguments.contains(key.GetString())) {
        std::move(callback).Run(MakeJsonRpcResult(
            id, base::Value(ToolResult::Error(base::StrCat(
                                {"Missing required argument: ",
                                 key.GetString()}))
                                .ToValue())));
        return;
      }
    }
  }

  auto pending =
      base::MakeRefCounted<PendingToolCall>(std::move(id), std::move(callback));
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&PendingToolCall::TimedOut, pending, tool_timeout_),
      tool_timeout_);
  tool->handler.Run(arguments,
                    base::BindOnce(&PendingToolCall::Finish, pending));
}

}  // namespace chromeaf
