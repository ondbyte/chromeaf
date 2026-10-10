// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_CHROMEAF_MAIN_PARTS_H_
#define CHROMEAF_BROWSER_CHROMEAF_MAIN_PARTS_H_

#include <memory>

class ChromeBrowserMainExtraParts;

namespace chromeaf {

// The single entry point upstream code calls (from
// ChromeBrowserMainParts::Create). Starts the MCP control server after the
// browser has started, if --chromeaf-mcp-port is given.
std::unique_ptr<ChromeBrowserMainExtraParts> CreateChromeafMainExtraParts();

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_CHROMEAF_MAIN_PARTS_H_
