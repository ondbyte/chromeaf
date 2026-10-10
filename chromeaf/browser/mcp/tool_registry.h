// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_MCP_TOOL_REGISTRY_H_
#define CHROMEAF_BROWSER_MCP_TOOL_REGISTRY_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/values.h"

namespace chromeaf {

// The result of a tool invocation, shaped like an MCP CallToolResult.
class ToolResult {
 public:
  static ToolResult Text(std::string text);
  // Serializes |value| as pretty JSON text content and, if it is a dict, also
  // attaches it as `structuredContent`.
  static ToolResult Json(base::Value value);
  static ToolResult Image(std::string base64_data, std::string mime_type);
  static ToolResult Error(std::string message);

  ToolResult();
  ToolResult(ToolResult&&);
  ToolResult& operator=(ToolResult&&);
  ~ToolResult();

  ToolResult& AddText(std::string text);
  bool is_error() const { return is_error_; }
  const base::ListValue& content() const { return content_; }

  base::DictValue ToValue() &&;

 private:
  base::ListValue content_;
  std::optional<base::DictValue> structured_content_;
  bool is_error_ = false;
};

using ToolCallback = base::OnceCallback<void(ToolResult)>;
using ToolHandler =
    base::RepeatingCallback<void(const base::DictValue& arguments,
                                 ToolCallback callback)>;

struct ToolDefinition {
  ToolDefinition();
  ToolDefinition(std::string name,
                 std::string description,
                 base::DictValue input_schema,
                 ToolHandler handler);
  ToolDefinition(ToolDefinition&&);
  ToolDefinition& operator=(ToolDefinition&&);
  ~ToolDefinition();

  std::string name;
  std::string description;
  // JSON Schema (type "object") describing the arguments.
  base::DictValue input_schema;
  ToolHandler handler;
};

// Builds a JSON Schema "object" for tool arguments.
class SchemaBuilder {
 public:
  SchemaBuilder();
  SchemaBuilder(SchemaBuilder&&);
  SchemaBuilder& operator=(SchemaBuilder&&);
  ~SchemaBuilder();

  SchemaBuilder& Integer(std::string_view name,
                         std::string_view description,
                         bool required = false);
  SchemaBuilder& Number(std::string_view name,
                        std::string_view description,
                        bool required = false);
  SchemaBuilder& String(std::string_view name,
                        std::string_view description,
                        bool required = false);
  SchemaBuilder& Boolean(std::string_view name,
                         std::string_view description,
                         bool required = false);
  SchemaBuilder& Enum(std::string_view name,
                      std::string_view description,
                      const std::vector<std::string_view>& values,
                      bool required = false);

  base::DictValue Build();

 private:
  SchemaBuilder& Add(std::string_view name,
                     base::DictValue property,
                     bool required);

  base::DictValue properties_;
  base::ListValue required_;
};

// Ordered set of tools exposed over MCP. Lives on the UI thread.
class ToolRegistry {
 public:
  ToolRegistry();
  ToolRegistry(const ToolRegistry&) = delete;
  ToolRegistry& operator=(const ToolRegistry&) = delete;
  ~ToolRegistry();

  void Register(ToolDefinition tool);
  const ToolDefinition* Find(std::string_view name) const;
  // The `tools` array of a tools/list response.
  base::ListValue Describe() const;
  size_t size() const { return tools_.size(); }

 private:
  std::vector<ToolDefinition> tools_;
};

// Argument helpers shared by tool implementations. Numbers are accepted as
// either JSON ints or doubles.
std::optional<double> GetNumberArg(const base::DictValue& args,
                                   std::string_view key);
std::optional<int> GetIntArg(const base::DictValue& args, std::string_view key);
std::optional<bool> GetBoolArg(const base::DictValue& args,
                               std::string_view key);
const std::string* GetStringArg(const base::DictValue& args,
                                std::string_view key);

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_MCP_TOOL_REGISTRY_H_
