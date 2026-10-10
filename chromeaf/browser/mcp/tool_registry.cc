// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/mcp/tool_registry.h"

#include <cmath>
#include <utility>

#include "base/check.h"
#include "base/json/json_writer.h"

namespace chromeaf {

// ToolResult -----------------------------------------------------------------

ToolResult::ToolResult() = default;
ToolResult::ToolResult(ToolResult&&) = default;
ToolResult& ToolResult::operator=(ToolResult&&) = default;
ToolResult::~ToolResult() = default;

// static
ToolResult ToolResult::Text(std::string text) {
  ToolResult result;
  result.AddText(std::move(text));
  return result;
}

// static
ToolResult ToolResult::Json(base::Value value) {
  ToolResult result;
  result.AddText(
      base::WriteJsonWithOptions(value, base::OPTIONS_PRETTY_PRINT)
          .value_or("null"));
  if (value.is_dict()) {
    result.structured_content_ = std::move(value).TakeDict();
  }
  return result;
}

// static
ToolResult ToolResult::Image(std::string base64_data, std::string mime_type) {
  ToolResult result;
  result.content_.Append(base::DictValue()
                             .Set("type", "image")
                             .Set("data", std::move(base64_data))
                             .Set("mimeType", std::move(mime_type)));
  return result;
}

// static
ToolResult ToolResult::Error(std::string message) {
  ToolResult result = Text(std::move(message));
  result.is_error_ = true;
  return result;
}

ToolResult& ToolResult::AddText(std::string text) {
  content_.Append(
      base::DictValue().Set("type", "text").Set("text", std::move(text)));
  return *this;
}

base::DictValue ToolResult::ToValue() && {
  base::DictValue dict;
  dict.Set("content", std::move(content_));
  if (structured_content_) {
    dict.Set("structuredContent", std::move(*structured_content_));
  }
  dict.Set("isError", is_error_);
  return dict;
}

// ToolDefinition -------------------------------------------------------------

ToolDefinition::ToolDefinition() = default;
ToolDefinition::ToolDefinition(std::string name,
                               std::string description,
                               base::DictValue input_schema,
                               ToolHandler handler)
    : name(std::move(name)),
      description(std::move(description)),
      input_schema(std::move(input_schema)),
      handler(std::move(handler)) {}
ToolDefinition::ToolDefinition(ToolDefinition&&) = default;
ToolDefinition& ToolDefinition::operator=(ToolDefinition&&) = default;
ToolDefinition::~ToolDefinition() = default;

// SchemaBuilder --------------------------------------------------------------

SchemaBuilder::SchemaBuilder() = default;
SchemaBuilder::SchemaBuilder(SchemaBuilder&&) = default;
SchemaBuilder& SchemaBuilder::operator=(SchemaBuilder&&) = default;
SchemaBuilder::~SchemaBuilder() = default;

SchemaBuilder& SchemaBuilder::Add(std::string_view name,
                                  base::DictValue property,
                                  bool required) {
  properties_.Set(name, std::move(property));
  if (required) {
    required_.Append(name);
  }
  return *this;
}

SchemaBuilder& SchemaBuilder::Integer(std::string_view name,
                                      std::string_view description,
                                      bool required) {
  return Add(name,
             base::DictValue().Set("type", "integer").Set("description",
                                                          description),
             required);
}

SchemaBuilder& SchemaBuilder::Number(std::string_view name,
                                     std::string_view description,
                                     bool required) {
  return Add(name,
             base::DictValue().Set("type", "number").Set("description",
                                                         description),
             required);
}

SchemaBuilder& SchemaBuilder::String(std::string_view name,
                                     std::string_view description,
                                     bool required) {
  return Add(name,
             base::DictValue().Set("type", "string").Set("description",
                                                         description),
             required);
}

SchemaBuilder& SchemaBuilder::Boolean(std::string_view name,
                                      std::string_view description,
                                      bool required) {
  return Add(name,
             base::DictValue().Set("type", "boolean").Set("description",
                                                          description),
             required);
}

SchemaBuilder& SchemaBuilder::Enum(std::string_view name,
                                   std::string_view description,
                                   const std::vector<std::string_view>& values,
                                   bool required) {
  base::ListValue list;
  for (std::string_view value : values) {
    list.Append(value);
  }
  return Add(name,
             base::DictValue()
                 .Set("type", "string")
                 .Set("description", description)
                 .Set("enum", std::move(list)),
             required);
}

base::DictValue SchemaBuilder::Build() {
  base::DictValue schema;
  schema.Set("type", "object");
  schema.Set("properties", std::move(properties_));
  if (!required_.empty()) {
    schema.Set("required", std::move(required_));
  }
  properties_ = base::DictValue();
  required_ = base::ListValue();
  return schema;
}

// ToolRegistry ---------------------------------------------------------------

ToolRegistry::ToolRegistry() = default;
ToolRegistry::~ToolRegistry() = default;

void ToolRegistry::Register(ToolDefinition tool) {
  CHECK(!Find(tool.name)) << "Duplicate tool " << tool.name;
  CHECK(tool.handler);
  tools_.push_back(std::move(tool));
}

const ToolDefinition* ToolRegistry::Find(std::string_view name) const {
  for (const ToolDefinition& tool : tools_) {
    if (tool.name == name) {
      return &tool;
    }
  }
  return nullptr;
}

base::ListValue ToolRegistry::Describe() const {
  base::ListValue list;
  for (const ToolDefinition& tool : tools_) {
    list.Append(base::DictValue()
                    .Set("name", tool.name)
                    .Set("description", tool.description)
                    .Set("inputSchema", tool.input_schema.Clone()));
  }
  return list;
}

// Argument helpers -----------------------------------------------------------

std::optional<double> GetNumberArg(const base::DictValue& args,
                                   std::string_view key) {
  std::optional<double> value = args.FindDouble(key);
  if (value && !std::isfinite(*value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<int> GetIntArg(const base::DictValue& args,
                             std::string_view key) {
  std::optional<double> value = GetNumberArg(args, key);
  if (!value || *value < -2147483648.0 || *value > 2147483647.0) {
    return std::nullopt;
  }
  return static_cast<int>(*value);
}

std::optional<bool> GetBoolArg(const base::DictValue& args,
                               std::string_view key) {
  return args.FindBool(key);
}

const std::string* GetStringArg(const base::DictValue& args,
                                std::string_view key) {
  return args.FindString(key);
}

}  // namespace chromeaf
