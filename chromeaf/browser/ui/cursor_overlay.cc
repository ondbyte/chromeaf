// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/ui/cursor_overlay.h"

#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "cc/paint/paint_flags.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace chromeaf {

namespace {

// The widget is larger than the arrow so the "pressed" ring fits around the
// hotspot.
constexpr int kWidgetSize = 40;
constexpr int kHotspot = 12;  // Arrow tip offset inside the widget.
constexpr base::TimeDelta kHideAfter = base::Seconds(8);

}  // namespace

class CursorOverlay::PointerView : public views::View {
 public:
  PointerView() {
    SetCanProcessEventsWithinSubtree(false);
  }

  void SetPressed(bool pressed) {
    if (pressed_ != pressed) {
      pressed_ = pressed;
      SchedulePaint();
    }
  }

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override {
    if (pressed_) {
      cc::PaintFlags ring;
      ring.setAntiAlias(true);
      ring.setStyle(cc::PaintFlags::kFill_Style);
      ring.setColor(SkColorSetARGB(0x70, 0xFF, 0x52, 0x52));
      canvas->DrawCircle(gfx::PointF(kHotspot, kHotspot), 11.0f, ring);
    }

    // Classic arrow pointer with its tip at (kHotspot, kHotspot).
    const float x = kHotspot;
    const float y = kHotspot;
    SkPath arrow = SkPathBuilder()
                       .moveTo(x, y)
                       .lineTo(x, y + 17)
                       .lineTo(x + 4, y + 13)
                       .lineTo(x + 7, y + 20)
                       .lineTo(x + 10, y + 19)
                       .lineTo(x + 7, y + 12)
                       .lineTo(x + 12, y + 12)
                       .close()
                       .detach();

    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setStyle(cc::PaintFlags::kFill_Style);
    fill.setColor(SK_ColorWHITE);
    canvas->DrawPath(arrow, fill);

    cc::PaintFlags outline;
    outline.setAntiAlias(true);
    outline.setStyle(cc::PaintFlags::kStroke_Style);
    outline.setStrokeWidth(1.5f);
    outline.setColor(SK_ColorBLACK);
    canvas->DrawPath(arrow, outline);
  }

 private:
  bool pressed_ = false;
};

CursorOverlay::CursorOverlay(content::WebContents* contents)
    : contents_(contents) {}

CursorOverlay::~CursorOverlay() = default;

void CursorOverlay::EnsureWidget() {
  if (widget_) {
    return;
  }
  gfx::NativeView parent = contents_->GetNativeView();
  if (!parent) {
    return;
  }
  widget_ = std::make_unique<views::Widget>();
  views::Widget::InitParams params(
      views::Widget::InitParams::CLIENT_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_CONTROL);
  params.name = "ChromeafCursorOverlay";
  params.parent = parent;
  params.child = true;
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  params.accept_events = false;
  params.activatable = views::Widget::InitParams::Activatable::kNo;
  params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
  params.bounds = gfx::Rect(0, 0, kWidgetSize, kWidgetSize);
  widget_->Init(std::move(params));
  view_ = widget_->SetContentsView(std::make_unique<PointerView>());
}

void CursorOverlay::MoveTo(const gfx::PointF& point) {
  EnsureWidget();
  if (!widget_) {
    return;
  }
  widget_->SetBounds(gfx::Rect(static_cast<int>(std::round(point.x())) -
                                   kHotspot,
                               static_cast<int>(std::round(point.y())) -
                                   kHotspot,
                               kWidgetSize, kWidgetSize));
  // A navigation may have added a new contents view on top of us.
  widget_->StackAtTop();
  if (!widget_->IsVisible()) {
    widget_->ShowInactive();
  }
  hide_timer_.Start(FROM_HERE, kHideAfter,
                    base::BindOnce(&CursorOverlay::Hide,
                                   base::Unretained(this)));
}

void CursorOverlay::SetPressed(bool pressed) {
  if (view_) {
    view_->SetPressed(pressed);
  }
}

void CursorOverlay::Hide() {
  if (widget_) {
    widget_->Hide();
  }
}

}  // namespace chromeaf
