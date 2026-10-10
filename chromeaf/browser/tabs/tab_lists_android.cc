// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/tabs/tab_lists.h"

#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "components/tabs/public/tab_interface.h"

namespace chromeaf {

std::vector<TabListEntry> GetAllTabLists() {
  std::vector<TabListEntry> result;
  for (TabModel* model : TabModelList::models()) {
    if (model->IsOffTheRecord() && !model->IsActiveModel()) {
      continue;
    }
    TabListEntry entry{model, model->GetSessionId().id(),
                       model->IsActiveModel()};
    if (entry.is_active_window) {
      result.insert(result.begin(), entry);
    } else {
      result.push_back(entry);
    }
  }
  return result;
}

TabListInterface* GetTabListForTab(tabs::TabInterface* tab) {
  return TabModelList::GetTabModelForWebContents(tab->GetContents());
}

int32_t GetWindowIdForTab(tabs::TabInterface* tab) {
  TabModel* model = TabModelList::GetTabModelForWebContents(tab->GetContents());
  return model ? model->GetSessionId().id() : -1;
}

}  // namespace chromeaf
