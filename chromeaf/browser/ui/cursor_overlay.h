// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_UI_CURSOR_OVERLAY_H_
#define CHROMEAF_BROWSER_UI_CURSOR_OVERLAY_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/timer/timer.h"
#include "ui/gfx/geometry/point_f.h"

namespace content {
class WebContents;
}

namespace views {
class Widget;
}

namespace chromeaf {

// Draws a pointer at the position of synthetic mouse input so a person
// watching can follow what the agent does. It is a transparent child widget
// stacked over the tab's contents: browser UI, not part of the page, so sites
// cannot observe it, and it ignores events so it never intercepts input.
class CursorOverlay {
 public:
  explicit CursorOverlay(content::WebContents* contents);
  CursorOverlay(const CursorOverlay&) = delete;
  CursorOverlay& operator=(const CursorOverlay&) = delete;
  ~CursorOverlay();

  // |point| is in DIPs relative to the tab's viewport.
  void MoveTo(const gfx::PointF& point);
  void SetPressed(bool pressed);

 private:
  class PointerView;

  void EnsureWidget();
  void Hide();

  const raw_ptr<content::WebContents> contents_;
  std::unique_ptr<views::Widget> widget_;
  raw_ptr<PointerView> view_ = nullptr;
  base::OneShotTimer hide_timer_;
};

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_UI_CURSOR_OVERLAY_H_
