// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/tabs/tab_lists.h"

#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "components/tabs/public/tab_interface.h"

namespace chromeaf {

std::vector<TabListEntry> GetAllTabLists() {
  std::vector<TabListEntry> result;
  BrowserWindowInterface* last_active =
      GlobalBrowserCollection::GetInstance()->GetLastActiveBrowser();
  // Activation order is only used to put the window the user last focused
  // first; every window is listed.
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->IsDeleteScheduled() ||
            browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
          return true;
        }
        if (TabListInterface* list = TabListInterface::From(browser)) {
          result.push_back(
              {list, browser->GetSessionID().id(), browser == last_active});
        }
        return true;
      });
  return result;
}

TabListInterface* GetTabListForTab(tabs::TabInterface* tab) {
  BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
  return browser ? TabListInterface::From(browser) : nullptr;
}

int32_t GetWindowIdForTab(tabs::TabInterface* tab) {
  BrowserWindowInterface* browser = tab->GetBrowserWindowInterface();
  return browser ? browser->GetSessionID().id() : -1;
}

}  // namespace chromeaf
