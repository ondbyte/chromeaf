// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/tools/tool_util.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/timer/timer.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chromeaf/browser/tabs/tab_lists.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"

namespace chromeaf {

namespace {

class GuardedCallback {
 public:
  GuardedCallback(ToolCallback callback, std::string message)
      : callback_(std::move(callback)), message_(std::move(message)) {}
  GuardedCallback(const GuardedCallback&) = delete;
  GuardedCallback& operator=(const GuardedCallback&) = delete;
  ~GuardedCallback() {
    if (callback_) {
      std::move(callback_).Run(ToolResult::Error(std::move(message_)));
    }
  }

  void Run(ToolResult result) { std::move(callback_).Run(std::move(result)); }

 private:
  ToolCallback callback_;
  std::string message_;
};

// Self-deleting observer used by WaitForLoad().
class LoadWaiter : public content::WebContentsObserver {
 public:
  LoadWaiter(content::WebContents* contents,
             base::TimeDelta timeout,
             base::OnceCallback<void(bool)> callback)
      : content::WebContentsObserver(contents),
        callback_(std::move(callback)) {
    timeout_timer_.Start(FROM_HERE, timeout,
                         base::BindOnce(&LoadWaiter::Finish,
                                        base::Unretained(this), false));
    if (!contents->IsLoading()) {
      // Navigation may not have started loading yet; give it a moment.
      grace_timer_.Start(FROM_HERE, base::Milliseconds(500),
                         base::BindOnce(&LoadWaiter::Finish,
                                        base::Unretained(this), true));
    }
  }

  // content::WebContentsObserver:
  void DidStartLoading() override { grace_timer_.Stop(); }
  void DidStopLoading() override { Finish(true); }
  void WebContentsDestroyed() override { Finish(false); }

 private:
  ~LoadWaiter() override = default;

  void Finish(bool loaded) {
    base::OnceCallback<void(bool)> callback = std::move(callback_);
    delete this;
    std::move(callback).Run(loaded);
  }

  base::OnceCallback<void(bool)> callback_;
  base::OneShotTimer timeout_timer_;
  base::OneShotTimer grace_timer_;
};

}  // namespace

ToolCallback ErrorIfDropped(ToolCallback callback, std::string message) {
  return base::BindOnce(&GuardedCallback::Run,
                        base::Owned(std::make_unique<GuardedCallback>(
                            std::move(callback), std::move(message))));
}

base::expected<ResolvedTab, std::string> ResolveTab(
    const base::DictValue& args) {
  tabs::TabInterface* tab = nullptr;
  if (args.contains(kTabIdArg)) {
    std::optional<int> id = GetIntArg(args, kTabIdArg);
    if (!id) {
      return base::unexpected("tab_id must be an integer");
    }
    tab = tabs::TabHandle(*id).Get();
    if (!tab) {
      return base::unexpected(
          base::StrCat({"No tab with id ", base::NumberToString(*id)}));
    }
  } else {
    for (const TabListEntry& entry : GetAllTabLists()) {
      tab = entry.list->GetActiveTab();
      if (tab) {
        break;
      }
    }
    if (!tab) {
      return base::unexpected("No open tab");
    }
  }
  content::WebContents* contents = tab->GetContents();
  if (!contents) {
    return base::unexpected("Tab has no contents");
  }
  return ResolvedTab{tab, contents};
}

base::DictValue DescribeTab(tabs::TabInterface* tab) {
  content::WebContents* contents = tab->GetContents();
  TabListInterface* list = GetTabListForTab(tab);
  base::DictValue dict;
  dict.Set("tab_id", tab->GetHandle().raw_value());
  dict.Set("window_id", GetWindowIdForTab(tab));
  dict.Set("index", list ? list->GetIndexOfTab(tab->GetHandle()) : -1);
  dict.Set("active", tab->IsActivated());
  if (contents) {
    dict.Set("title", contents->GetTitle());
    dict.Set("url", contents->GetLastCommittedURL().possibly_invalid_spec());
    dict.Set("loading", contents->IsLoading());
  }
  return dict;
}

void WaitForLoad(content::WebContents* contents,
                 base::TimeDelta timeout,
                 base::OnceCallback<void(bool loaded)> callback) {
  new LoadWaiter(contents, timeout, std::move(callback));
}

}  // namespace chromeaf
