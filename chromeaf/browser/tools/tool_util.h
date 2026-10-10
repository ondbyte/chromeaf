// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_TOOLS_TOOL_UTIL_H_
#define CHROMEAF_BROWSER_TOOLS_TOOL_UTIL_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chromeaf/browser/mcp/tool_registry.h"

namespace content {
class WebContents;
}

namespace tabs {
class TabInterface;
}

namespace chromeaf {

inline constexpr char kTabIdArg[] = "tab_id";
inline constexpr char kTabIdDescription[] =
    "Target tab id from get-tabs. Defaults to the active tab of the "
    "focused window.";

// Wraps |callback| so that, if it is destroyed without being run (renderer
// crashed, tab closed mid-operation, ...), an error result with |message| is
// delivered instead.
ToolCallback ErrorIfDropped(ToolCallback callback, std::string message);

struct ResolvedTab {
  raw_ptr<tabs::TabInterface> tab;
  raw_ptr<content::WebContents> contents;
};

// Resolves the optional "tab_id" argument, falling back to the active tab.
base::expected<ResolvedTab, std::string> ResolveTab(
    const base::DictValue& args);

// tab_id, window_id, index, active, title, url, loading.
base::DictValue DescribeTab(tabs::TabInterface* tab);

// Runs |callback| with true once |contents| finishes loading, or with false
// on timeout or if the WebContents goes away. If nothing is loading and no
// load starts shortly, reports true.
void WaitForLoad(content::WebContents* contents,
                 base::TimeDelta timeout,
                 base::OnceCallback<void(bool loaded)> callback);

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_TOOLS_TOOL_UTIL_H_
