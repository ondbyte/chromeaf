// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_PAGE_AGENT_HOST_H_
#define CHROMEAF_BROWSER_PAGE_AGENT_HOST_H_

#include "chromeaf/common/page_agent.mojom.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "mojo/public/cpp/bindings/associated_remote.h"

namespace chromeaf {

// Owns the connection to the renderer-side PageAgent of a tab's primary main
// frame, re-establishing it after navigations and renderer crashes. Pending
// calls are dropped (and their callbacks destroyed) when the page changes.
class PageAgentHost : public content::WebContentsObserver,
                      public content::WebContentsUserData<PageAgentHost> {
 public:
  PageAgentHost(const PageAgentHost&) = delete;
  PageAgentHost& operator=(const PageAgentHost&) = delete;
  ~PageAgentHost() override;

  // Returns the agent for |contents|' primary main frame, or nullptr if the
  // renderer is not live.
  static mojom::PageAgent* GetAgent(content::WebContents* contents);

 private:
  friend class content::WebContentsUserData<PageAgentHost>;
  explicit PageAgentHost(content::WebContents* contents);

  mojom::PageAgent* GetOrBind();

  // content::WebContentsObserver:
  void PrimaryPageChanged(content::Page& page) override;
  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override;
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;

  mojo::AssociatedRemote<mojom::PageAgent> agent_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_PAGE_AGENT_HOST_H_
