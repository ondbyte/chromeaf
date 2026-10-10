// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/page_agent_host.h"

#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"

namespace chromeaf {

PageAgentHost::PageAgentHost(content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<PageAgentHost>(*contents) {}

PageAgentHost::~PageAgentHost() = default;

// static
mojom::PageAgent* PageAgentHost::GetAgent(content::WebContents* contents) {
  return PageAgentHost::GetOrCreateForWebContents(contents)->GetOrBind();
}

mojom::PageAgent* PageAgentHost::GetOrBind() {
  if (agent_.is_bound() && agent_.is_connected()) {
    return agent_.get();
  }
  agent_.reset();
  content::RenderFrameHost* frame = web_contents()->GetPrimaryMainFrame();
  if (!frame || !frame->IsRenderFrameLive()) {
    return nullptr;
  }
  frame->GetRemoteAssociatedInterfaces()->GetInterface(&agent_);
  return agent_.get();
}

void PageAgentHost::PrimaryPageChanged(content::Page& page) {
  agent_.reset();
}

void PageAgentHost::RenderFrameHostChanged(
    content::RenderFrameHost* old_host,
    content::RenderFrameHost* new_host) {
  if (new_host && new_host->IsInPrimaryMainFrame()) {
    agent_.reset();
  }
}

void PageAgentHost::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  agent_.reset();
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(PageAgentHost);

}  // namespace chromeaf
