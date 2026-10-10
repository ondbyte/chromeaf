// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "chromeaf/browser/mcp/tool_registry.h"
#include "chromeaf/browser/page_agent_host.h"
#include "chromeaf/browser/tools/tool_util.h"
#include "chromeaf/browser/tools/tools.h"
#include "chromeaf/common/page_agent.mojom.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace chromeaf {

namespace {

constexpr uint32_t kDefaultMaxHtmlBytes = 4 * 1024 * 1024;
constexpr uint32_t kDefaultMaxTextChars = 1024 * 1024;
constexpr uint32_t kDefaultMaxClickables = 500;
constexpr char kAgentGone[] =
    "The page went away before it could answer (navigation or renderer "
    "crash). Retry.";

// Resolves the target tab and its page agent, or reports an error.
mojom::PageAgent* GetAgentOrReply(const base::DictValue& args,
                                  ToolCallback& callback) {
  base::expected<ResolvedTab, std::string> resolved = ResolveTab(args);
  if (!resolved.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved.error()));
    return nullptr;
  }
  mojom::PageAgent* agent = PageAgentHost::GetAgent(resolved->contents);
  if (!agent) {
    std::move(callback).Run(
        ToolResult::Error("The tab's page is not loaded (renderer not live)"));
  }
  return agent;
}

uint32_t GetLimit(const base::DictValue& args,
                  std::string_view key,
                  uint32_t fallback) {
  std::optional<int> value = GetIntArg(args, key);
  return value && *value > 0 ? static_cast<uint32_t>(*value) : fallback;
}

void GetHtml(const base::DictValue& args, ToolCallback callback) {
  mojom::PageAgent* agent = GetAgentOrReply(args, callback);
  if (!agent) {
    return;
  }
  agent->GetHtml(
      GetLimit(args, "max_bytes", kDefaultMaxHtmlBytes),
      base::BindOnce(
          [](ToolCallback callback, const std::string& html, bool truncated) {
            ToolResult result = ToolResult::Text(html);
            if (truncated) {
              result.AddText(
                  "[truncated: raise max_bytes to get the full document]");
            }
            std::move(callback).Run(std::move(result));
          },
          ErrorIfDropped(std::move(callback), kAgentGone)));
}

void GetText(const base::DictValue& args, ToolCallback callback) {
  mojom::PageAgent* agent = GetAgentOrReply(args, callback);
  if (!agent) {
    return;
  }
  agent->GetText(
      GetLimit(args, "max_chars", kDefaultMaxTextChars),
      base::BindOnce(
          [](ToolCallback callback, const std::string& text) {
            std::move(callback).Run(ToolResult::Text(text));
          },
          ErrorIfDropped(std::move(callback), kAgentGone)));
}

base::DictValue RectToDict(const gfx::Rect& rect) {
  return base::DictValue()
      .Set("x", rect.x())
      .Set("y", rect.y())
      .Set("width", rect.width())
      .Set("height", rect.height());
}

void OnClickables(ToolCallback callback,
                  std::vector<mojom::ClickablePtr> items,
                  const gfx::Size& viewport) {
  base::ListValue list;
  for (const mojom::ClickablePtr& item : items) {
    base::DictValue dict;
    dict.Set("id", item->id);
    dict.Set("tag", item->tag);
    if (!item->role.empty()) {
      dict.Set("role", item->role);
    }
    if (!item->text.empty()) {
      dict.Set("text", item->text);
    }
    if (!item->href.empty()) {
      dict.Set("href", item->href);
    }
    if (!item->type.empty()) {
      dict.Set("type", item->type);
    }
    if (!item->dom_id.empty()) {
      dict.Set("dom_id", item->dom_id);
    }
    dict.Set("bounds", RectToDict(item->bounds));
    gfx::Point center = item->bounds.CenterPoint();
    dict.Set("center", base::DictValue().Set("x", center.x()).Set("y",
                                                                  center.y()));
    dict.Set("in_viewport", item->in_viewport);
    list.Append(std::move(dict));
  }
  std::move(callback).Run(ToolResult::Json(base::Value(
      base::DictValue()
          .Set("viewport", base::DictValue()
                               .Set("width", viewport.width())
                               .Set("height", viewport.height()))
          .Set("count", static_cast<int>(list.size()))
          .Set("clickables", std::move(list)))));
}

void GetClickables(const base::DictValue& args, ToolCallback callback) {
  mojom::PageAgent* agent = GetAgentOrReply(args, callback);
  if (!agent) {
    return;
  }
  agent->GetClickables(
      GetBoolArg(args, "include_offscreen").value_or(false),
      GetLimit(args, "max_count", kDefaultMaxClickables),
      base::BindOnce(&OnClickables,
                     ErrorIfDropped(std::move(callback), kAgentGone)));
}

}  // namespace

void RegisterPageTools(ToolRegistry& registry) {
  registry.Register(
      {"get-whole-page-as-html",
       "Returns the current DOM of the tab's page serialized as HTML "
       "(reflects script changes, not just the original source).",
       SchemaBuilder()
           .Integer(kTabIdArg, kTabIdDescription)
           .Integer("max_bytes", "Truncate after this many bytes (default 4MB).")
           .Build(),
       base::BindRepeating(&GetHtml)});

  registry.Register(
      {"get-page-text",
       "Returns the visible text of the page (cheaper than HTML for reading).",
       SchemaBuilder()
           .Integer(kTabIdArg, kTabIdDescription)
           .Integer("max_chars", "Truncate after this many characters.")
           .Build(),
       base::BindRepeating(&GetText)});

  registry.Register(
      {"get-clickables",
       "Lists elements a user could click (links, buttons, inputs, ARIA "
       "widgets, elements with click handlers or pointer cursor) with their "
       "bounds and center in viewport coordinates. Pass `id` as clickable_id "
       "to click/move-cursor, or use the center coordinates.",
       SchemaBuilder()
           .Integer(kTabIdArg, kTabIdDescription)
           .Boolean("include_offscreen",
                    "Also list elements outside the visible viewport.")
           .Integer("max_count", "Maximum number of elements (default 500).")
           .Build(),
       base::BindRepeating(&GetClickables)});
}

}  // namespace chromeaf
