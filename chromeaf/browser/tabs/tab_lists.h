// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_TABS_TAB_LISTS_H_
#define CHROMEAF_BROWSER_TABS_TAB_LISTS_H_

#include <cstdint>
#include <vector>

#include "base/memory/raw_ptr.h"

class TabListInterface;

namespace tabs {
class TabInterface;
}

namespace chromeaf {

// A window's worth of tabs. On desktop this is a browser window's tab strip,
// on Android a TabModel. Both implement the cross-platform TabListInterface,
// so everything above this file is platform independent.
struct TabListEntry {
  raw_ptr<TabListInterface> list;
  int32_t window_id;
  bool is_active_window;
};

// All tab lists, the active (last focused) window first.
std::vector<TabListEntry> GetAllTabLists();

// The tab list that owns |tab|, or nullptr.
TabListInterface* GetTabListForTab(tabs::TabInterface* tab);

// The window id reported for |tab|'s list, or -1.
int32_t GetWindowIdForTab(tabs::TabInterface* tab);

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_TABS_TAB_LISTS_H_
