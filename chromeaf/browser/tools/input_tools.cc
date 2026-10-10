// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chromeaf/browser/input/human_mouse_path.h"
#include "chromeaf/browser/input/input_injector.h"
#include "chromeaf/browser/mcp/tool_registry.h"
#include "chromeaf/browser/page_agent_host.h"
#include "chromeaf/browser/tools/tool_util.h"
#include "chromeaf/browser/tools/tools.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"

namespace chromeaf {

namespace {

constexpr char kClickableIdArg[] = "clickable_id";

// Where to point, resolved either from explicit coordinates or from a
// clickable returned by get-clickables.
struct Target {
  gfx::PointF point;
  double width = 24.0;  // For Fitts's law.
  std::optional<gfx::Rect> element_bounds;
};

using TargetCallback =
    base::OnceCallback<void(base::expected<Target, std::string>)>;

std::string Describe(const gfx::PointF& point) {
  return base::StrCat({"(", base::NumberToString(point.x()), ", ",
                       base::NumberToString(point.y()), ")"});
}

// People don't hit the exact center; aim somewhere in the middle area.
gfx::PointF HumanAimPoint(const gfx::Rect& bounds) {
  gfx::PointF center = gfx::RectF(bounds).CenterPoint();
  auto spread = [](int size) {
    double half = size / 2.0;
    double amount = std::min(half * 0.35, 12.0);
    double r = (base::RandDouble() + base::RandDouble()) - 1.0;  // [-1, 1]
    return static_cast<float>(r * amount);
  };
  center.Offset(spread(bounds.width()), spread(bounds.height()));
  return center;
}

base::expected<gfx::PointF, std::string> ValidateInViewport(
    const gfx::PointF& point,
    const gfx::Size& viewport) {
  if (point.x() < 0 || point.y() < 0 || point.x() >= viewport.width() ||
      point.y() >= viewport.height()) {
    return base::unexpected(base::StrCat(
        {"Point ", Describe(point), " is outside the viewport (",
         base::NumberToString(viewport.width()), "x",
         base::NumberToString(viewport.height()),
         "). Scroll it into view first."}));
  }
  return point;
}

void ResolveTarget(content::WebContents* contents,
                   const base::DictValue& args,
                   TargetCallback callback) {
  gfx::Size viewport = InputInjector::For(contents)->viewport_size();

  if (std::optional<int> clickable_id = GetIntArg(args, kClickableIdArg)) {
    mojom::PageAgent* agent = PageAgentHost::GetAgent(contents);
    if (!agent) {
      std::move(callback).Run(
          base::unexpected("The tab's page is not loaded (renderer not live)"));
      return;
    }
    // If the renderer goes away the callback is dropped; report that.
    auto on_bounds = base::BindOnce(
        [](gfx::Size viewport, int id, TargetCallback callback,
           const std::optional<gfx::Rect>& bounds) {
          if (!bounds) {
            std::move(callback).Run(base::unexpected(base::StrCat(
                {"Clickable ", base::NumberToString(id),
                 " no longer exists; call get-clickables again."})));
            return;
          }
          gfx::Rect visible = *bounds;
          visible.Intersect(gfx::Rect(viewport));
          if (visible.IsEmpty()) {
            std::move(callback).Run(base::unexpected(base::StrCat(
                {"Clickable ", base::NumberToString(id),
                 " is outside the viewport (top=",
                 base::NumberToString(bounds->y()), ", viewport height=",
                 base::NumberToString(viewport.height()),
                 "). Use scroll first."})));
            return;
          }
          Target target;
          target.point = HumanAimPoint(visible);
          target.width = std::max(1, std::min(visible.width(),
                                              visible.height()));
          target.element_bounds = *bounds;
          std::move(callback).Run(std::move(target));
        },
        viewport, *clickable_id, std::move(callback));
    agent->GetClickableBounds(*clickable_id, std::move(on_bounds));
    return;
  }

  std::optional<double> x = GetNumberArg(args, "x");
  std::optional<double> y = GetNumberArg(args, "y");
  if (!x || !y) {
    std::move(callback).Run(
        base::unexpected("Provide either clickable_id or both x and y"));
    return;
  }
  gfx::PointF point(static_cast<float>(*x), static_cast<float>(*y));
  base::expected<gfx::PointF, std::string> valid =
      ValidateInViewport(point, viewport);
  if (!valid.has_value()) {
    std::move(callback).Run(base::unexpected(valid.error()));
    return;
  }
  Target target;
  target.point = point;
  std::move(callback).Run(std::move(target));
}

HumanMousePathOptions PathOptions(const base::DictValue& args,
                                  const Target& target) {
  HumanMousePathOptions options;
  options.target_width = target.width;
  if (std::optional<int> ms = GetIntArg(args, "duration_ms")) {
    options.duration = base::Milliseconds(std::clamp(*ms, 0, 10000));
  }
  return options;
}

// Turns the injector's completion into a tool result describing where the
// cursor ended up.
InputInjector::DoneCallback ReplyWhenDone(ToolCallback callback,
                                          tabs::TabHandle tab,
                                          std::string action,
                                          std::optional<gfx::Rect> element) {
  return base::BindOnce(
      [](ToolCallback callback, tabs::TabHandle tab, std::string action,
         std::optional<gfx::Rect> element, std::string error) {
        if (!error.empty()) {
          std::move(callback).Run(ToolResult::Error(error));
          return;
        }
        base::DictValue result;
        result.Set("action", action);
        if (tabs::TabInterface* t = tab.Get(); t && t->GetContents()) {
          gfx::PointF cursor =
              InputInjector::For(t->GetContents())->cursor_position();
          result.Set("cursor", base::DictValue()
                                   .Set("x", cursor.x())
                                   .Set("y", cursor.y()));
        }
        if (element) {
          result.Set("element_bounds", base::DictValue()
                                           .Set("x", element->x())
                                           .Set("y", element->y())
                                           .Set("width", element->width())
                                           .Set("height", element->height()));
        }
        std::move(callback).Run(ToolResult::Json(base::Value(std::move(result))));
      },
      std::move(callback), tab, std::move(action), std::move(element));
}

enum class PointerAction { kMove, kClick, kRightClick, kDoubleClick };

void PointerTool(PointerAction action,
                 const base::DictValue& args,
                 ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved = ResolveTab(args);
  if (!resolved.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved.error()));
    return;
  }
  callback = ErrorIfDropped(std::move(callback),
                            "The tab went away during the input action");
  tabs::TabHandle handle = resolved->tab->GetHandle();
  ResolveTarget(
      resolved->contents, args,
      base::BindOnce(
          [](PointerAction action, base::DictValue args,
             tabs::TabHandle handle, ToolCallback callback,
             base::expected<Target, std::string> target) {
            if (!target.has_value()) {
              std::move(callback).Run(ToolResult::Error(target.error()));
              return;
            }
            tabs::TabInterface* tab = handle.Get();
            if (!tab || !tab->GetContents()) {
              std::move(callback).Run(ToolResult::Error("Tab closed"));
              return;
            }
            InputInjector* injector = InputInjector::For(tab->GetContents());
            HumanMousePathOptions options = PathOptions(args, *target);
            switch (action) {
              case PointerAction::kMove:
                injector->MoveTo(target->point, options,
                                 ReplyWhenDone(std::move(callback), handle,
                                               "move-cursor",
                                               target->element_bounds));
                return;
              case PointerAction::kClick: {
                InputInjector::Button button = InputInjector::Button::kLeft;
                if (const std::string* name = args.FindString("button")) {
                  if (*name == "right") {
                    button = InputInjector::Button::kRight;
                  } else if (*name == "middle") {
                    button = InputInjector::Button::kMiddle;
                  }
                }
                injector->Click(target->point, button, 1, options,
                                ReplyWhenDone(std::move(callback), handle,
                                              "click",
                                              target->element_bounds));
                return;
              }
              case PointerAction::kRightClick:
                injector->Click(target->point, InputInjector::Button::kRight,
                                1, options,
                                ReplyWhenDone(std::move(callback), handle,
                                              "rightclick",
                                              target->element_bounds));
                return;
              case PointerAction::kDoubleClick:
                injector->Click(target->point, InputInjector::Button::kLeft,
                                2, options,
                                ReplyWhenDone(std::move(callback), handle,
                                              "double-click",
                                              target->element_bounds));
                return;
            }
          },
          action, args.Clone(), handle, std::move(callback)));
}

void Scroll(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved = ResolveTab(args);
  if (!resolved.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved.error()));
    return;
  }
  InputInjector* injector = InputInjector::For(resolved->contents);
  gfx::Size viewport = injector->viewport_size();
  double dx = GetNumberArg(args, "delta_x").value_or(0);
  double dy = GetNumberArg(args, "delta_y").value_or(0);
  if (dx == 0 && dy == 0) {
    std::move(callback).Run(
        ToolResult::Error("Provide delta_x and/or delta_y (pixels)"));
    return;
  }
  // Scroll where the pointer is, unless asked otherwise; keep it inside the
  // viewport so the wheel lands on the page.
  gfx::PointF at = injector->cursor_position();
  if (std::optional<double> x = GetNumberArg(args, "x")) {
    at.set_x(static_cast<float>(*x));
  }
  if (std::optional<double> y = GetNumberArg(args, "y")) {
    at.set_y(static_cast<float>(*y));
  }
  at.SetToMax(gfx::PointF(1, 1));
  at.SetToMin(gfx::PointF(std::max(1, viewport.width() - 2),
                          std::max(1, viewport.height() - 2)));
  injector->Scroll(
      at,
      gfx::Vector2dF(static_cast<float>(std::clamp(dx, -20000.0, 20000.0)),
                     static_cast<float>(std::clamp(dy, -20000.0, 20000.0))),
      ReplyWhenDone(ErrorIfDropped(std::move(callback), "Tab went away"),
                    resolved->tab->GetHandle(), "scroll", std::nullopt));
}

void FocusContents(content::WebContents* contents) {
  content::RenderWidgetHostView* view = contents->GetRenderWidgetHostView();
  if (view && !view->HasFocus()) {
    contents->Focus();
  }
}

void TypeText(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved = ResolveTab(args);
  if (!resolved.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved.error()));
    return;
  }
  const std::string* text = GetStringArg(args, "text");
  int min_ms = std::clamp(GetIntArg(args, "min_delay_ms").value_or(45), 0,
                          2000);
  int max_ms = std::clamp(GetIntArg(args, "max_delay_ms").value_or(160),
                          min_ms, 4000);
  FocusContents(resolved->contents);
  InputInjector::For(resolved->contents)
      ->TypeText(base::UTF8ToUTF16(*text), base::Milliseconds(min_ms),
                 base::Milliseconds(max_ms),
                 ReplyWhenDone(ErrorIfDropped(std::move(callback),
                                              "Tab went away"),
                               resolved->tab->GetHandle(), "type-text",
                               std::nullopt));
}

void PressKey(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved = ResolveTab(args);
  if (!resolved.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved.error()));
    return;
  }
  FocusContents(resolved->contents);
  InputInjector::For(resolved->contents)
      ->PressKey(*GetStringArg(args, "key"),
                 ReplyWhenDone(ErrorIfDropped(std::move(callback),
                                              "Tab went away"),
                               resolved->tab->GetHandle(), "press-key",
                               std::nullopt));
}

void GetCursorPosition(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved = ResolveTab(args);
  if (!resolved.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved.error()));
    return;
  }
  InputInjector* injector = InputInjector::For(resolved->contents);
  gfx::PointF cursor = injector->cursor_position();
  gfx::Size viewport = injector->viewport_size();
  std::move(callback).Run(ToolResult::Json(base::Value(
      base::DictValue()
          .Set("x", cursor.x())
          .Set("y", cursor.y())
          .Set("viewport", base::DictValue()
                               .Set("width", viewport.width())
                               .Set("height", viewport.height())))));
}

SchemaBuilder PointerSchema() {
  SchemaBuilder schema;
  schema.Number("x", "Viewport x in CSS/DIP pixels.")
      .Number("y", "Viewport y in CSS/DIP pixels.")
      .Integer(kClickableIdArg,
               "Target an element from get-clickables instead of x/y.")
      .Integer("duration_ms",
               "Override the movement duration (default: human-like, from "
               "distance and target size).")
      .Integer(kTabIdArg, kTabIdDescription);
  return schema;
}

}  // namespace

void RegisterInputTools(ToolRegistry& registry) {
  registry.Register(
      {"move-cursor",
       "Moves the mouse pointer to a point or element like a person would "
       "(curved path, natural acceleration, slight tremor). Fires real "
       "mousemove/hover events.",
       PointerSchema().Build(),
       base::BindRepeating(&PointerTool, PointerAction::kMove)});

  {
    SchemaBuilder schema = PointerSchema();
    schema.Enum("button", "Mouse button (default left).",
                {"left", "right", "middle"});
    registry.Register(
        {"click",
         "Moves the pointer to the target and clicks it (press, short hold, "
         "release).",
         schema.Build(),
         base::BindRepeating(&PointerTool, PointerAction::kClick)});
  }

  registry.Register(
      {"rightclick",
       "Moves the pointer to the target and right-clicks (opens the context "
       "menu if the page doesn't handle it).",
       PointerSchema().Build(),
       base::BindRepeating(&PointerTool, PointerAction::kRightClick)});

  registry.Register(
      {"double-click", "Moves the pointer to the target and double-clicks.",
       PointerSchema().Build(),
       base::BindRepeating(&PointerTool, PointerAction::kDoubleClick)});

  registry.Register(
      {"scroll",
       "Scrolls with the mouse wheel. Positive delta_y scrolls down. The "
       "wheel is turned where the pointer is unless x/y are given.",
       SchemaBuilder()
           .Number("delta_x", "Horizontal pixels (positive = right).")
           .Number("delta_y", "Vertical pixels (positive = down).")
           .Number("x", "Pointer x for the wheel events.")
           .Number("y", "Pointer y for the wheel events.")
           .Integer(kTabIdArg, kTabIdDescription)
           .Build(),
       base::BindRepeating(&Scroll)});

  registry.Register(
      {"type-text",
       "Types text into the focused element one key at a time with human "
       "timing. Click the field first to focus it. \\n presses Enter.",
       SchemaBuilder()
           .String("text", "Text to type.", true)
           .Integer("min_delay_ms", "Minimum delay between keys (default 45).")
           .Integer("max_delay_ms",
                    "Maximum delay between keys (default 160).")
           .Integer(kTabIdArg, kTabIdDescription)
           .Build(),
       base::BindRepeating(&TypeText)});

  registry.Register(
      {"press-key",
       "Presses a key or chord, e.g. \"Enter\", \"Escape\", \"ArrowDown\", "
       "\"Tab\", \"Backspace\", \"Control+a\", \"Shift+Tab\". Names are DOM "
       "KeyboardEvent.key values.",
       SchemaBuilder()
           .String("key", "Key or chord.", true)
           .Integer(kTabIdArg, kTabIdDescription)
           .Build(),
       base::BindRepeating(&PressKey)});

  registry.Register(
      {"get-cursor-position",
       "Returns the pointer position used for synthetic input in the tab.",
       SchemaBuilder().Integer(kTabIdArg, kTabIdDescription).Build(),
       base::BindRepeating(&GetCursorPosition)});
}

}  // namespace chromeaf
