// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/chromeaf_main_parts.h"

#include <string>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/chrome_browser_main_extra_parts.h"
#include "chromeaf/browser/mcp/mcp_http_server.h"
#include "chromeaf/browser/mcp/mcp_protocol.h"
#include "chromeaf/browser/mcp/tool_registry.h"
#include "chromeaf/browser/tools/tools.h"
#include "chromeaf/common/switches.h"

namespace chromeaf {

namespace {

constexpr base::TimeDelta kToolTimeout = base::Seconds(60);

class ChromeafMainExtraParts : public ChromeBrowserMainExtraParts {
 public:
  ChromeafMainExtraParts() = default;
  ChromeafMainExtraParts(const ChromeafMainExtraParts&) = delete;
  ChromeafMainExtraParts& operator=(const ChromeafMainExtraParts&) = delete;
  ~ChromeafMainExtraParts() override = default;

  // ChromeBrowserMainExtraParts:
  void PostBrowserStart() override {
    const base::CommandLine& command_line =
        *base::CommandLine::ForCurrentProcess();
    if (!command_line.HasSwitch(switches::kMcpPort)) {
      return;
    }
    unsigned port = 0;
    if (!base::StringToUint(
            command_line.GetSwitchValueASCII(switches::kMcpPort), &port) ||
        port > 65535) {
      LOG(ERROR) << "chromeaf: invalid --" << switches::kMcpPort;
      return;
    }

    McpHttpServer::Options options;
    options.port = static_cast<uint16_t>(port);
    if (command_line.HasSwitch(switches::kMcpHost)) {
      options.host = command_line.GetSwitchValueASCII(switches::kMcpHost);
    }
    options.token = command_line.GetSwitchValueASCII(switches::kMcpToken);
    if (options.host != "127.0.0.1" && options.host != "::1" &&
        options.host != "localhost" && options.token.empty()) {
      LOG(WARNING) << "chromeaf: MCP server bound to " << options.host
                   << " without --" << switches::kMcpToken
                   << "; anyone who can reach it controls this browser.";
    }

    registry_ = std::make_unique<ToolRegistry>();
    RegisterAllTools(*registry_);
    protocol_ = std::make_unique<McpProtocol>(registry_.get(), kToolTimeout);
    server_ = std::make_unique<McpHttpServer>(options, protocol_.get());
    server_->Start();
  }

  void PostMainMessageLoopRun() override {
    server_.reset();
    protocol_.reset();
    registry_.reset();
  }

 private:
  std::unique_ptr<ToolRegistry> registry_;
  std::unique_ptr<McpProtocol> protocol_;
  std::unique_ptr<McpHttpServer> server_;
};

}  // namespace

std::unique_ptr<ChromeBrowserMainExtraParts> CreateChromeafMainExtraParts() {
  return std::make_unique<ChromeafMainExtraParts>();
}

}  // namespace chromeaf
