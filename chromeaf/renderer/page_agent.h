// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_RENDERER_PAGE_AGENT_H_
#define CHROMEAF_RENDERER_PAGE_AGENT_H_

#include <vector>

#include "chromeaf/common/page_agent.mojom.h"
#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "third_party/blink/public/web/web_element.h"

namespace content {
class RenderFrame;
}

namespace chromeaf {

// Answers the browser's questions about a main frame's document using only
// Blink's public C++ API: it reads the live DOM and layout, never runs script
// in the page and is invisible to it.
class PageAgent : public content::RenderFrameObserver,
                  public mojom::PageAgent {
 public:
  // Attaches an agent to |render_frame| if it is a main frame. The agent
  // deletes itself when the frame goes away.
  static void MaybeCreate(content::RenderFrame* render_frame);

  PageAgent(const PageAgent&) = delete;
  PageAgent& operator=(const PageAgent&) = delete;

  // mojom::PageAgent:
  void GetHtml(uint32_t max_bytes, GetHtmlCallback callback) override;
  void GetText(uint32_t max_chars, GetTextCallback callback) override;
  void GetClickables(bool include_offscreen,
                     uint32_t max_count,
                     GetClickablesCallback callback) override;
  void GetClickableBounds(int32_t id,
                          GetClickableBoundsCallback callback) override;

 private:
  explicit PageAgent(content::RenderFrame* render_frame);
  ~PageAgent() override;

  // content::RenderFrameObserver:
  void OnDestruct() override;
  void DidCommitProvisionalLoad(ui::PageTransition transition) override;

  void BindReceiver(
      mojo::PendingAssociatedReceiver<mojom::PageAgent> receiver);

  mojo::AssociatedReceiver<mojom::PageAgent> receiver_{this};
  // Elements returned by the last GetClickables(), indexed by id.
  std::vector<blink::WebElement> clickables_;
};

}  // namespace chromeaf

#endif  // CHROMEAF_RENDERER_PAGE_AGENT_H_
