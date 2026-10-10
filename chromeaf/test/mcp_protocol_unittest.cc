// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/mcp/mcp_protocol.h"

#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "chromeaf/browser/mcp/tool_registry.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeaf {
namespace {

class McpProtocolTest : public testing::Test {
 protected:
  McpProtocolTest() {
    registry_.Register(
        {"echo", "Echoes its text argument.",
         SchemaBuilder().String("text", "Text to echo.", true).Build(),
         base::BindRepeating([](const base::DictValue& args,
                                ToolCallback callback) {
           std::move(callback).Run(
               ToolResult::Text(*args.FindString("text")));
         })});
    registry_.Register(
        {"never", "Never answers.", SchemaBuilder().Build(),
         base::BindRepeating(
             [](ToolCallback* stash, const base::DictValue& args,
                ToolCallback callback) { *stash = std::move(callback); },
             &stashed_callback_)});
  }

  // Sends |request| and returns the parsed response, or nullopt if none.
  std::optional<base::DictValue> Send(const std::string& request) {
    base::test::TestFuture<std::optional<std::string>> future;
    protocol_.HandleMessage(request, future.GetCallback());
    std::optional<std::string> response = future.Take();
    if (!response) {
      return std::nullopt;
    }
    std::optional<base::DictValue> dict =
        base::JSONReader::ReadDict(*response, base::JSON_PARSE_RFC);
    EXPECT_TRUE(dict) << *response;
    return dict;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  ToolRegistry registry_;
  ToolCallback stashed_callback_;
  McpProtocol protocol_{&registry_, base::Seconds(5)};
};

TEST_F(McpProtocolTest, Initialize) {
  std::optional<base::DictValue> response = Send(
      R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
          "protocolVersion":"2025-03-26","capabilities":{},
          "clientInfo":{"name":"test","version":"1"}}})");
  ASSERT_TRUE(response);
  EXPECT_EQ(1, response->FindInt("id"));
  const base::DictValue* result = response->FindDict("result");
  ASSERT_TRUE(result);
  EXPECT_EQ("2025-03-26", *result->FindString("protocolVersion"));
  EXPECT_TRUE(result->FindDictByDottedPath("capabilities.tools"));
  EXPECT_EQ("chromeaf", *result->FindStringByDottedPath("serverInfo.name"));
}

TEST_F(McpProtocolTest, UnknownProtocolVersionGetsLatest) {
  std::optional<base::DictValue> response = Send(
      R"({"jsonrpc":"2.0","id":"a","method":"initialize","params":{
          "protocolVersion":"1999-01-01"}})");
  ASSERT_TRUE(response);
  EXPECT_EQ(McpProtocol::kLatestProtocolVersion,
            *response->FindStringByDottedPath("result.protocolVersion"));
  EXPECT_EQ("a", *response->FindString("id"));
}

TEST_F(McpProtocolTest, NotificationHasNoResponse) {
  EXPECT_FALSE(
      Send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
}

TEST_F(McpProtocolTest, ToolsList) {
  std::optional<base::DictValue> response =
      Send(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
  ASSERT_TRUE(response);
  const base::ListValue* tools =
      response->FindListByDottedPath("result.tools");
  ASSERT_TRUE(tools);
  ASSERT_EQ(2u, tools->size());
  const base::DictValue& echo = (*tools)[0].GetDict();
  EXPECT_EQ("echo", *echo.FindString("name"));
  EXPECT_EQ("object", *echo.FindStringByDottedPath("inputSchema.type"));
  EXPECT_EQ("string", *echo.FindStringByDottedPath(
                          "inputSchema.properties.text.type"));
}

TEST_F(McpProtocolTest, ToolsCall) {
  std::optional<base::DictValue> response = Send(
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call",
          "params":{"name":"echo","arguments":{"text":"hi"}}})");
  ASSERT_TRUE(response);
  const base::DictValue* result = response->FindDict("result");
  ASSERT_TRUE(result);
  EXPECT_EQ(false, result->FindBool("isError"));
  const base::ListValue* content = result->FindList("content");
  ASSERT_TRUE(content);
  ASSERT_EQ(1u, content->size());
  EXPECT_EQ("text", *(*content)[0].GetDict().FindString("type"));
  EXPECT_EQ("hi", *(*content)[0].GetDict().FindString("text"));
}

TEST_F(McpProtocolTest, MissingRequiredArgumentIsToolError) {
  std::optional<base::DictValue> response = Send(
      R"({"jsonrpc":"2.0","id":4,"method":"tools/call",
          "params":{"name":"echo","arguments":{}}})");
  ASSERT_TRUE(response);
  EXPECT_EQ(true, response->FindBoolByDottedPath("result.isError"));
}

TEST_F(McpProtocolTest, UnknownToolIsInvalidParams) {
  std::optional<base::DictValue> response = Send(
      R"({"jsonrpc":"2.0","id":5,"method":"tools/call",
          "params":{"name":"nope"}})");
  ASSERT_TRUE(response);
  EXPECT_EQ(McpProtocol::kInvalidParams,
            response->FindIntByDottedPath("error.code"));
}

TEST_F(McpProtocolTest, UnknownMethod) {
  std::optional<base::DictValue> response =
      Send(R"({"jsonrpc":"2.0","id":6,"method":"bogus"})");
  ASSERT_TRUE(response);
  EXPECT_EQ(McpProtocol::kMethodNotFound,
            response->FindIntByDottedPath("error.code"));
}

TEST_F(McpProtocolTest, ParseError) {
  std::optional<base::DictValue> response = Send("{not json");
  ASSERT_TRUE(response);
  EXPECT_EQ(McpProtocol::kParseError,
            response->FindIntByDottedPath("error.code"));
  ASSERT_TRUE(response->Find("id"));
  EXPECT_TRUE(response->Find("id")->is_none());
}

TEST_F(McpProtocolTest, BatchRejected) {
  std::optional<base::DictValue> response =
      Send(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])");
  ASSERT_TRUE(response);
  EXPECT_EQ(McpProtocol::kInvalidRequest,
            response->FindIntByDottedPath("error.code"));
}

TEST_F(McpProtocolTest, Ping) {
  std::optional<base::DictValue> response =
      Send(R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
  ASSERT_TRUE(response);
  EXPECT_TRUE(response->FindDict("result"));
}

TEST_F(McpProtocolTest, ToolTimeout) {
  base::test::TestFuture<std::optional<std::string>> future;
  protocol_.HandleMessage(
      R"({"jsonrpc":"2.0","id":8,"method":"tools/call",
          "params":{"name":"never"}})",
      future.GetCallback());
  EXPECT_FALSE(future.IsReady());
  task_environment_.FastForwardBy(base::Seconds(6));
  ASSERT_TRUE(future.IsReady());
  std::optional<base::DictValue> response =
      base::JSONReader::ReadDict(*future.Get(), base::JSON_PARSE_RFC);
  ASSERT_TRUE(response);
  EXPECT_EQ(true, response->FindBoolByDottedPath("result.isError"));

  // A late answer from the tool is ignored.
  std::move(stashed_callback_).Run(ToolResult::Text("late"));
}

TEST_F(McpProtocolTest, IsInitializeRequest) {
  EXPECT_TRUE(McpProtocol::IsInitializeRequest(
      R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"));
  EXPECT_FALSE(McpProtocol::IsInitializeRequest(
      R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
  EXPECT_FALSE(McpProtocol::IsInitializeRequest("garbage"));
}

TEST(ToolResultTest, JsonAddsStructuredContent) {
  base::DictValue dict = ToolResult::Json(base::Value(
                                              base::DictValue().Set("a", 1)))
                             .ToValue();
  EXPECT_EQ(1, dict.FindIntByDottedPath("structuredContent.a"));
  EXPECT_EQ(false, dict.FindBool("isError"));
}

TEST(ToolResultTest, Image) {
  base::DictValue dict = ToolResult::Image("AAAA", "image/png").ToValue();
  const base::ListValue* content = dict.FindList("content");
  ASSERT_TRUE(content);
  EXPECT_EQ("image", *(*content)[0].GetDict().FindString("type"));
  EXPECT_EQ("image/png", *(*content)[0].GetDict().FindString("mimeType"));
}

}  // namespace
}  // namespace chromeaf
