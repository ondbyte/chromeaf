// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <tuple>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chromeaf/browser/mcp/tool_registry.h"
#include "chromeaf/browser/tabs/tab_lists.h"
#include "chromeaf/browser/tools/tool_util.h"
#include "chromeaf/browser/tools/tools.h"
#include "components/tabs/public/tab_interface.h"
#include "components/url_formatter/url_fixer.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace chromeaf {

namespace {

constexpr char kNewTabUrl[] = "chrome://newtab/";
constexpr base::TimeDelta kDefaultLoadTimeout = base::Seconds(20);

base::TimeDelta GetLoadTimeout(const base::DictValue& args) {
  std::optional<int> ms = GetIntArg(args, "timeout_ms");
  return ms && *ms > 0 ? base::Milliseconds(*ms) : kDefaultLoadTimeout;
}

bool ShouldWait(const base::DictValue& args) {
  return GetBoolArg(args, "wait_for_load").value_or(true);
}

GURL FixupUrl(const std::string& input) {
  return url_formatter::FixupURL(input, std::string());
}

// Replies with the tab's description, optionally after it finished loading.
void ReplyWithTab(tabs::TabInterface* tab,
                  bool wait,
                  base::TimeDelta timeout,
                  ToolCallback callback) {
  if (!wait || !tab->GetContents()) {
    std::move(callback).Run(ToolResult::Json(base::Value(DescribeTab(tab))));
    return;
  }
  WaitForLoad(tab->GetContents(), timeout,
              base::BindOnce(
                  [](tabs::TabHandle handle, ToolCallback callback,
                     bool loaded) {
                    tabs::TabInterface* tab = handle.Get();
                    if (!tab) {
                      std::move(callback).Run(
                          ToolResult::Error("Tab closed while loading"));
                      return;
                    }
                    base::DictValue dict = DescribeTab(tab);
                    dict.Set("load_complete", loaded);
                    std::move(callback).Run(
                        ToolResult::Json(base::Value(std::move(dict))));
                  },
                  tab->GetHandle(), std::move(callback)));
}

void GetTabs(const base::DictValue& args, ToolCallback callback) {
  base::ListValue windows;
  for (const TabListEntry& entry : GetAllTabLists()) {
    base::ListValue tabs;
    for (tabs::TabInterface* tab : entry.list->GetAllTabs()) {
      tabs.Append(DescribeTab(tab));
    }
    windows.Append(base::DictValue()
                       .Set("window_id", entry.window_id)
                       .Set("focused", entry.is_active_window)
                       .Set("tabs", std::move(tabs)));
  }
  std::move(callback).Run(ToolResult::Json(
      base::Value(base::DictValue().Set("windows", std::move(windows)))));
}

void CreateTab(const base::DictValue& args, ToolCallback callback) {
  std::vector<TabListEntry> lists = GetAllTabLists();
  if (lists.empty()) {
    std::move(callback).Run(ToolResult::Error("No browser window is open"));
    return;
  }
  TabListInterface* list = lists.front().list;
  if (std::optional<int> window_id = GetIntArg(args, "window_id")) {
    list = nullptr;
    for (const TabListEntry& entry : lists) {
      if (entry.window_id == *window_id) {
        list = entry.list;
      }
    }
    if (!list) {
      std::move(callback).Run(ToolResult::Error(base::StrCat(
          {"No window with id ", base::NumberToString(*window_id)})));
      return;
    }
  }

  const std::string* url_arg = GetStringArg(args, "url");
  GURL url = url_arg && !url_arg->empty() ? FixupUrl(*url_arg)
                                          : GURL(kNewTabUrl);
  if (!url.is_valid()) {
    std::move(callback).Run(ToolResult::Error("Invalid url"));
    return;
  }
  bool foreground = GetBoolArg(args, "foreground").value_or(true);
  tabs::TabInterface* tab = list->OpenTab(url, /*index=*/-1, foreground);
  if (!tab) {
    std::move(callback).Run(ToolResult::Error("Could not open tab"));
    return;
  }
  ReplyWithTab(tab, ShouldWait(args), GetLoadTimeout(args),
               std::move(callback));
}

void CloseTab(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved_or = ResolveTab(args);
  if (!resolved_or.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved_or.error()));
    return;
  }
  const ResolvedTab& resolved = resolved_or.value();
  TabListInterface* list = GetTabListForTab(resolved.tab);
  if (!list) {
    std::move(callback).Run(ToolResult::Error("Tab is not in a window"));
    return;
  }
  int32_t id = resolved.tab->GetHandle().raw_value();
  list->CloseTab(resolved.tab->GetHandle());
  std::move(callback).Run(ToolResult::Json(base::Value(
      base::DictValue().Set("closed_tab_id", id))));
}

void ActivateTab(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved_or = ResolveTab(args);
  if (!resolved_or.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved_or.error()));
    return;
  }
  const ResolvedTab& resolved = resolved_or.value();
  TabListInterface* list = GetTabListForTab(resolved.tab);
  if (!list) {
    std::move(callback).Run(ToolResult::Error("Tab is not in a window"));
    return;
  }
  list->ActivateTab(resolved.tab->GetHandle());
  std::move(callback).Run(
      ToolResult::Json(base::Value(DescribeTab(resolved.tab))));
}

void Navigate(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved_or = ResolveTab(args);
  if (!resolved_or.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved_or.error()));
    return;
  }
  const ResolvedTab& resolved = resolved_or.value();
  GURL url = FixupUrl(*GetStringArg(args, "url"));
  if (!url.is_valid()) {
    std::move(callback).Run(ToolResult::Error("Invalid url"));
    return;
  }
  // Same transition as typing into the omnibox.
  content::NavigationController::LoadURLParams params(url);
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  resolved.contents->GetController().LoadURLWithParams(params);
  ReplyWithTab(resolved.tab, ShouldWait(args), GetLoadTimeout(args),
               std::move(callback));
}

enum class HistoryAction { kBack, kForward, kReload };

void History(HistoryAction action,
             const base::DictValue& args,
             ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved_or = ResolveTab(args);
  if (!resolved_or.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved_or.error()));
    return;
  }
  const ResolvedTab& resolved = resolved_or.value();
  content::NavigationController& controller =
      resolved.contents->GetController();
  switch (action) {
    case HistoryAction::kBack:
      if (!controller.CanGoBack()) {
        std::move(callback).Run(ToolResult::Error("Cannot go back"));
        return;
      }
      std::ignore = controller.GoBack();
      break;
    case HistoryAction::kForward:
      if (!controller.CanGoForward()) {
        std::move(callback).Run(ToolResult::Error("Cannot go forward"));
        return;
      }
      std::ignore = controller.GoForward();
      break;
    case HistoryAction::kReload:
      controller.Reload(content::ReloadType::NORMAL,
                        /*check_for_repost=*/false);
      break;
  }
  ReplyWithTab(resolved.tab, ShouldWait(args), GetLoadTimeout(args),
               std::move(callback));
}

void WaitForLoadTool(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved_or = ResolveTab(args);
  if (!resolved_or.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved_or.error()));
    return;
  }
  const ResolvedTab& resolved = resolved_or.value();
  ReplyWithTab(resolved.tab, /*wait=*/true, GetLoadTimeout(args),
               std::move(callback));
}

SchemaBuilder& AddWaitArgs(SchemaBuilder& builder) {
  return builder
      .Boolean("wait_for_load",
               "Wait until the page finishes loading (default true).")
      .Integer("timeout_ms", "Maximum time to wait for the load.");
}

}  // namespace

void RegisterTabTools(ToolRegistry& registry) {
  registry.Register(
      {"get-tabs",
       "Lists all browser windows and their tabs (id, title, url, active, "
       "loading).",
       SchemaBuilder().Build(), base::BindRepeating(&GetTabs)});

  {
    SchemaBuilder schema;
    schema.String("url", "URL to open. Defaults to the new tab page.")
        .Boolean("foreground", "Activate the new tab (default true).")
        .Integer("window_id", "Window to open the tab in (from get-tabs).");
    AddWaitArgs(schema);
    registry.Register({"create-tab", "Opens a new tab.", schema.Build(),
                       base::BindRepeating(&CreateTab)});
  }

  registry.Register(
      {"close-tab", "Closes a tab.",
       SchemaBuilder().Integer(kTabIdArg, "Tab to close.", true).Build(),
       base::BindRepeating(&CloseTab)});

  registry.Register(
      {"activate-tab", "Switches to a tab, making it the active one.",
       SchemaBuilder().Integer(kTabIdArg, "Tab to activate.", true).Build(),
       base::BindRepeating(&ActivateTab)});

  {
    SchemaBuilder schema;
    schema.String("url", "URL (or host name) to load.", true)
        .Integer(kTabIdArg, kTabIdDescription);
    AddWaitArgs(schema);
    registry.Register(
        {"navigate", "Loads a URL in a tab, as if typed into the address bar.",
         schema.Build(), base::BindRepeating(&Navigate)});
  }

  struct {
    const char* name;
    const char* description;
    HistoryAction action;
  } history_tools[] = {
      {"go-back", "Goes back in the tab's history.", HistoryAction::kBack},
      {"go-forward", "Goes forward in the tab's history.",
       HistoryAction::kForward},
      {"reload", "Reloads the tab.", HistoryAction::kReload},
  };
  for (const auto& tool : history_tools) {
    SchemaBuilder schema;
    schema.Integer(kTabIdArg, kTabIdDescription);
    AddWaitArgs(schema);
    registry.Register({tool.name, tool.description, schema.Build(),
                       base::BindRepeating(&History, tool.action)});
  }

  registry.Register(
      {"wait-for-load", "Waits until the tab has finished loading.",
       SchemaBuilder()
           .Integer(kTabIdArg, kTabIdDescription)
           .Integer("timeout_ms", "Maximum time to wait.")
           .Build(),
       base::BindRepeating(&WaitForLoadTool)});
}

void RegisterAllTools(ToolRegistry& registry) {
  RegisterTabTools(registry);
  RegisterPageTools(registry);
  RegisterScreenshotTool(registry);
  RegisterInputTools(registry);
}

}  // namespace chromeaf
