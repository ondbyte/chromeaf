// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/input/input_injector.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/notreached.h"
#include "base/rand_util.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/strings/utf_string_conversions.h"
#include "chromeaf/common/switches.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"

#if defined(USE_AURA)
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/event_source.h"
#endif

#if defined(TOOLKIT_VIEWS) && defined(USE_AURA)
#include "chromeaf/browser/ui/cursor_overlay.h"
#define CHROMEAF_HAS_CURSOR_OVERLAY 1
#endif

namespace chromeaf {

namespace {

using blink::WebInputEvent;

constexpr int kWheelNotch = 120;

base::TimeDelta RandomDelay(int min_ms, int max_ms) {
  return base::Milliseconds(base::RandIntInclusive(min_ms, max_ms));
}

#if defined(USE_AURA)
int AuraButtonFlag(InputInjector::Button button) {
  switch (button) {
    case InputInjector::Button::kLeft:
      return ui::EF_LEFT_MOUSE_BUTTON;
    case InputInjector::Button::kMiddle:
      return ui::EF_MIDDLE_MOUSE_BUTTON;
    case InputInjector::Button::kRight:
      return ui::EF_RIGHT_MOUSE_BUTTON;
  }
  NOTREACHED();
}
#else
int BlinkButtonModifier(InputInjector::Button button) {
  switch (button) {
    case InputInjector::Button::kLeft:
      return WebInputEvent::kLeftButtonDown;
    case InputInjector::Button::kMiddle:
      return WebInputEvent::kMiddleButtonDown;
    case InputInjector::Button::kRight:
      return WebInputEvent::kRightButtonDown;
  }
  NOTREACHED();
}
#endif

std::string NormalizeKeyName(std::string_view name) {
  struct Alias {
    const char* alias;
    const char* key;
  };
  static constexpr Alias kAliases[] = {
      {"ctrl", "Control"},  {"control", "Control"}, {"cmd", "Meta"},
      {"command", "Meta"},  {"meta", "Meta"},       {"super", "Meta"},
      {"win", "Meta"},      {"option", "Alt"},      {"alt", "Alt"},
      {"shift", "Shift"},   {"esc", "Escape"},      {"escape", "Escape"},
      {"return", "Enter"},  {"enter", "Enter"},     {"del", "Delete"},
      {"delete", "Delete"}, {"backspace", "Backspace"},
      {"tab", "Tab"},       {"up", "ArrowUp"},      {"down", "ArrowDown"},
      {"left", "ArrowLeft"}, {"right", "ArrowRight"},
      {"pageup", "PageUp"}, {"pagedown", "PageDown"},
      {"home", "Home"},     {"end", "End"},         {"insert", "Insert"},
  };
  for (const Alias& alias : kAliases) {
    if (base::EqualsCaseInsensitiveASCII(name, alias.alias)) {
      return alias.key;
    }
  }
  if (base::EqualsCaseInsensitiveASCII(name, "space")) {
    return " ";
  }
  return std::string(name);
}

int ModifierForKey(ui::DomKey key) {
  if (key == ui::DomKey::SHIFT) {
    return WebInputEvent::kShiftKey;
  }
  if (key == ui::DomKey::CONTROL) {
    return WebInputEvent::kControlKey;
  }
  if (key == ui::DomKey::ALT) {
    return WebInputEvent::kAltKey;
  }
  if (key == ui::DomKey::META) {
    return WebInputEvent::kMetaKey;
  }
  return 0;
}

}  // namespace

InputInjector::Step::Step(base::TimeDelta delay, base::OnceClosure closure)
    : delay(delay), closure(std::move(closure)) {}
InputInjector::Step::Step(Step&&) = default;
InputInjector::Step& InputInjector::Step::operator=(Step&&) = default;
InputInjector::Step::~Step() = default;

InputInjector::InputInjector(content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<InputInjector>(*contents) {
#if defined(CHROMEAF_HAS_CURSOR_OVERLAY)
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kMcpNoCursorOverlay)) {
    overlay_ = std::make_unique<CursorOverlay>(contents);
  }
#endif
}

InputInjector::~InputInjector() = default;

// static
InputInjector* InputInjector::For(content::WebContents* contents) {
  return InputInjector::GetOrCreateForWebContents(contents);
}

gfx::Size InputInjector::viewport_size() {
  content::RenderWidgetHostView* view =
      web_contents()->GetRenderWidgetHostView();
  return view ? view->GetViewBounds().size() : gfx::Size();
}

gfx::PointF InputInjector::cursor_position() {
  if (!cursor_) {
    // A resting hand: somewhere around the middle-lower part of the page.
    gfx::Size size = viewport_size();
    cursor_ = gfx::PointF(
        static_cast<float>(size.width() * base::RandDouble() * 0.5 +
                           size.width() * 0.25),
        static_cast<float>(size.height() * base::RandDouble() * 0.4 +
                           size.height() * 0.45));
  }
  return *cursor_;
}

// Queue ----------------------------------------------------------------------

void InputInjector::Enqueue(base::TimeDelta delay_before,
                            base::OnceClosure step) {
  steps_.emplace_back(delay_before, std::move(step));
  if (!step_running_ && !timer_.IsRunning()) {
    timer_.Start(FROM_HERE, steps_.front().delay,
                 base::BindOnce(&InputInjector::RunNextStep,
                                weak_factory_.GetWeakPtr()));
  }
}

void InputInjector::InsertStepsAtFront(std::vector<Step> steps) {
  for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
    steps_.push_front(std::move(*it));
  }
}

void InputInjector::RunNextStep() {
  if (steps_.empty()) {
    return;
  }
  Step step = std::move(steps_.front());
  steps_.pop_front();

  base::WeakPtr<InputInjector> weak = weak_factory_.GetWeakPtr();
  step_running_ = true;
  std::move(step.closure).Run();
  if (!weak) {
    return;  // Dispatching the event destroyed the tab.
  }
  step_running_ = false;

  if (!steps_.empty()) {
    timer_.Start(FROM_HERE, steps_.front().delay,
                 base::BindOnce(&InputInjector::RunNextStep,
                                weak_factory_.GetWeakPtr()));
  }
}

void InputInjector::EnqueueDone(DoneCallback done) {
  Enqueue(base::TimeDelta(),
          base::BindOnce(
              [](base::WeakPtr<InputInjector> self, DoneCallback done) {
                std::string error;
                if (self) {
                  error = std::move(self->pending_error_);
                  self->pending_error_.clear();
                }
                std::move(done).Run(std::move(error));
              },
              weak_factory_.GetWeakPtr(), std::move(done)));
}

// Mouse ----------------------------------------------------------------------

void InputInjector::EnqueueMove(const gfx::PointF& target,
                                const HumanMousePathOptions& options) {
  // The path is computed when this step runs so it starts from wherever the
  // cursor is after the operations queued before it.
  Enqueue(base::TimeDelta(),
          base::BindOnce(&InputInjector::StartMove, base::Unretained(this),
                         target, options));
}

void InputInjector::StartMove(gfx::PointF target,
                              HumanMousePathOptions options) {
  std::vector<MousePathPoint> path =
      GenerateHumanMousePath(cursor_position(), target, options);
  std::vector<Step> steps;
  base::TimeDelta previous;
  for (const MousePathPoint& point : path) {
    steps.emplace_back(point.time - previous,
                       base::BindOnce(&InputInjector::DispatchMouse,
                                      base::Unretained(this),
                                      MouseAction::kMove, point.position,
                                      Button::kLeft, 0));
    previous = point.time;
  }
  InsertStepsAtFront(std::move(steps));
}

void InputInjector::MoveTo(const gfx::PointF& target,
                           const HumanMousePathOptions& options,
                           DoneCallback done) {
  EnqueueMove(target, options);
  EnqueueDone(std::move(done));
}

void InputInjector::Click(const gfx::PointF& target,
                          Button button,
                          int click_count,
                          const HumanMousePathOptions& options,
                          DoneCallback done) {
  EnqueueMove(target, options);
  click_count = std::clamp(click_count, 1, 3);
  for (int i = 1; i <= click_count; ++i) {
    // Settle on the target (first press) or pause between clicks.
    base::TimeDelta before_press =
        i == 1 ? RandomDelay(60, 180) : RandomDelay(70, 140);
    Enqueue(before_press,
            base::BindOnce(&InputInjector::DispatchMouse,
                           base::Unretained(this), MouseAction::kPress, target,
                           button, i));
    Enqueue(RandomDelay(55, 130),
            base::BindOnce(&InputInjector::DispatchMouse,
                           base::Unretained(this), MouseAction::kRelease,
                           target, button, i));
  }
  EnqueueDone(std::move(done));
}

void InputInjector::Scroll(const gfx::PointF& at,
                           const gfx::Vector2dF& delta,
                           DoneCallback done) {
  HumanMousePathOptions options;
  options.target_width = 200;  // Anywhere over the content will do.
  EnqueueMove(at, options);

  // Wheel offsets point the other way: positive y scrolls up.
  double remaining_x = -delta.x();
  double remaining_y = -delta.y();
  bool first = true;
  while (std::abs(remaining_x) >= 1 || std::abs(remaining_y) >= 1) {
    int dx = static_cast<int>(
        std::clamp(remaining_x, double{-kWheelNotch}, double{kWheelNotch}));
    int dy = static_cast<int>(
        std::clamp(remaining_y, double{-kWheelNotch}, double{kWheelNotch}));
    remaining_x -= dx;
    remaining_y -= dy;
    Enqueue(first ? RandomDelay(80, 200) : RandomDelay(25, 70),
            base::BindOnce(&InputInjector::DispatchWheel,
                           base::Unretained(this), at,
                           gfx::Vector2d(dx, dy)));
    first = false;
  }
  EnqueueDone(std::move(done));
}

void InputInjector::UpdateCursor(const gfx::PointF& point) {
  cursor_ = point;
#if defined(CHROMEAF_HAS_CURSOR_OVERLAY)
  if (overlay_) {
    overlay_->MoveTo(point);
  }
#endif
}

void InputInjector::DispatchMouse(MouseAction action,
                                  const gfx::PointF& point,
                                  Button button,
                                  int click_count) {
  content::RenderWidgetHostView* view =
      web_contents()->GetRenderWidgetHostView();
  if (!view) {
    pending_error_ = "The tab has no view";
    return;
  }
  UpdateCursor(point);
#if defined(CHROMEAF_HAS_CURSOR_OVERLAY)
  if (overlay_ && action != MouseAction::kMove) {
    overlay_->SetPressed(action == MouseAction::kPress);
  }
#endif

#if defined(USE_AURA)
  aura::Window* window = view->GetNativeView();
  aura::WindowTreeHost* host = window ? window->GetHost() : nullptr;
  if (!host) {
    pending_error_ = "The tab is not attached to a window";
    return;
  }
  // Same coordinate space as events coming from the platform: host pixels.
  gfx::PointF location = point;
  aura::Window::ConvertPointToTarget(window, window->GetRootWindow(),
                                     &location);
  host->ConvertDIPToPixels(&location);

  int button_flag = AuraButtonFlag(button);
  ui::EventType type = ui::EventType::kMouseMoved;
  int changed_button_flags = 0;
  switch (action) {
    case MouseAction::kMove:
      type = pressed_buttons_ ? ui::EventType::kMouseDragged
                              : ui::EventType::kMouseMoved;
      break;
    case MouseAction::kPress:
      pressed_buttons_ |= button_flag;
      type = ui::EventType::kMousePressed;
      changed_button_flags = button_flag;
      break;
    case MouseAction::kRelease:
      type = ui::EventType::kMouseReleased;
      changed_button_flags = button_flag;
      break;
  }
  ui::MouseEvent event(type, location, location, base::TimeTicks::Now(),
                       pressed_buttons_, changed_button_flags);
  if (action != MouseAction::kMove) {
    event.SetClickCount(click_count);
  }
  if (action == MouseAction::kRelease) {
    pressed_buttons_ &= ~button_flag;
  }
  std::ignore = host->GetEventSource()->SendEventToSink(&event);
#else
  content::RenderWidgetHost* widget = view->GetRenderWidgetHost();
  WebInputEvent::Type type = WebInputEvent::Type::kMouseMove;
  blink::WebPointerProperties::Button web_button =
      blink::WebPointerProperties::Button::kNoButton;
  switch (action) {
    case MouseAction::kMove:
      break;
    case MouseAction::kPress:
      type = WebInputEvent::Type::kMouseDown;
      pressed_buttons_ |= BlinkButtonModifier(button);
      break;
    case MouseAction::kRelease:
      type = WebInputEvent::Type::kMouseUp;
      break;
  }
  if (action != MouseAction::kMove) {
    switch (button) {
      case Button::kLeft:
        web_button = blink::WebPointerProperties::Button::kLeft;
        break;
      case Button::kMiddle:
        web_button = blink::WebPointerProperties::Button::kMiddle;
        break;
      case Button::kRight:
        web_button = blink::WebPointerProperties::Button::kRight;
        break;
    }
  }
  gfx::PointF screen_point =
      point + view->GetViewBounds().OffsetFromOrigin();
  blink::WebMouseEvent event(type, point, screen_point, web_button,
                             click_count, pressed_buttons_,
                             base::TimeTicks::Now());
  event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
  if (action == MouseAction::kRelease) {
    pressed_buttons_ &= ~BlinkButtonModifier(button);
  }
  widget->ForwardMouseEvent(event);
#endif
}

void InputInjector::DispatchWheel(const gfx::PointF& point,
                                  const gfx::Vector2d& offset) {
  content::RenderWidgetHostView* view =
      web_contents()->GetRenderWidgetHostView();
  if (!view) {
    pending_error_ = "The tab has no view";
    return;
  }
#if defined(USE_AURA)
  aura::Window* window = view->GetNativeView();
  aura::WindowTreeHost* host = window ? window->GetHost() : nullptr;
  if (!host) {
    pending_error_ = "The tab is not attached to a window";
    return;
  }
  gfx::PointF location = point;
  aura::Window::ConvertPointToTarget(window, window->GetRootWindow(),
                                     &location);
  host->ConvertDIPToPixels(&location);
  ui::MouseWheelEvent event(offset, location, location, base::TimeTicks::Now(),
                            pressed_buttons_, 0);
  std::ignore = host->GetEventSource()->SendEventToSink(&event);
#else
  blink::WebMouseWheelEvent event(WebInputEvent::Type::kMouseWheel,
                                  pressed_buttons_, base::TimeTicks::Now());
  event.SetPositionInWidget(point);
  event.SetPositionInScreen(point +
                            view->GetViewBounds().OffsetFromOrigin());
  event.delta_x = offset.x();
  event.delta_y = offset.y();
  event.wheel_ticks_x = static_cast<float>(offset.x()) / kWheelNotch;
  event.wheel_ticks_y = static_cast<float>(offset.y()) / kWheelNotch;
  event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;
  view->GetRenderWidgetHost()->ForwardWheelEvent(event);
#endif
}

// Keyboard -------------------------------------------------------------------

// static
InputInjector::KeySpec InputInjector::KeySpecForCharacter(char32_t character) {
  KeySpec spec;
  spec.key = ui::DomKey::FromCharacter(character);
  spec.character = character;
  if (character == '\n' || character == '\r') {
    spec.key = ui::DomKey::ENTER;
    spec.code = ui::DomCode::ENTER;
    spec.key_code = ui::VKEY_RETURN;
    spec.character = '\r';
    return spec;
  }
  if (character == '\t') {
    spec.key = ui::DomKey::TAB;
    spec.code = ui::DomCode::TAB;
    spec.key_code = ui::VKEY_TAB;
    return spec;
  }
  spec.code = ui::UsLayoutDomKeyToDomCode(spec.key);
  if (spec.code != ui::DomCode::NONE) {
    spec.key_code = ui::DomCodeToUsLayoutKeyboardCode(spec.code);
    if (ui::DomCodeToUsLayoutCharacter(spec.code, 0) !=
        static_cast<char16_t>(character)) {
      spec.implied_modifiers = WebInputEvent::kShiftKey;
    }
  }
  // Characters outside the US layout (accents, CJK, emoji) are delivered as
  // text with an unidentified key, like an input method would.
  return spec;
}

// static
std::optional<InputInjector::KeySpec> InputInjector::KeySpecForName(
    std::string_view raw_name) {
  std::string name = NormalizeKeyName(raw_name);
  // A single character, e.g. "a", "A", "1", " ".
  size_t index = 0;
  base_icu::UChar32 code_point = 0;
  if (base::ReadUnicodeCharacter(name, &index, &code_point) &&
      index + 1 == name.size()) {
    return KeySpecForCharacter(static_cast<char32_t>(code_point));
  }

  KeySpec spec;
  spec.key = ui::KeycodeConverter::KeyStringToDomKey(name);
  if (spec.key == ui::DomKey::NONE) {
    return std::nullopt;
  }
  if (spec.key == ui::DomKey::ENTER) {
    return KeySpecForCharacter('\r');
  }
  if (spec.key == ui::DomKey::TAB) {
    return KeySpecForCharacter('\t');
  }
  spec.code = ui::UsLayoutDomKeyToDomCode(spec.key);
  spec.key_code = ui::NonPrintableDomKeyToKeyboardCode(spec.key);
  return spec;
}

void InputInjector::DispatchKey(int type,
                                const KeySpec& key,
                                int modifiers) {
  content::RenderFrameHost* frame = web_contents()->GetFocusedFrame();
  if (!frame) {
    frame = web_contents()->GetPrimaryMainFrame();
  }
  content::RenderWidgetHost* widget =
      frame ? frame->GetRenderWidgetHost() : nullptr;
  if (!widget) {
    pending_error_ = "No focused frame to type into";
    return;
  }
  auto event_type = static_cast<WebInputEvent::Type>(type);
  input::NativeWebKeyboardEvent event(event_type, modifiers,
                                      base::TimeTicks::Now());
  event.dom_key = key.key;
  event.dom_code = static_cast<int>(key.code);
  event.native_key_code =
      ui::KeycodeConverter::DomCodeToNativeKeycode(key.code);
  event.windows_key_code = key.key_code;
  event.is_system_key = false;
  // Like real input the page sees the event first; synthetic events the page
  // ignores are not turned into browser shortcuts.
  event.skip_if_unhandled = true;
  if (key.character && (event_type == WebInputEvent::Type::kChar ||
                        event_type == WebInputEvent::Type::kRawKeyDown)) {
    std::u16string text;
    base::WriteUnicodeCharacter(static_cast<base_icu::UChar32>(key.character),
                                &text);
    for (size_t i = 0; i < text.size() && i + 1 < event.text.size(); ++i) {
      event.text[i] = text[i];
      event.unmodified_text[i] = text[i];
    }
  }
  widget->ForwardKeyboardEvent(event);
}

void InputInjector::EnqueueKeyStroke(const KeySpec& key, int modifiers) {
  KeySpec shift;
  shift.key = ui::DomKey::SHIFT;
  shift.code = ui::DomCode::SHIFT_LEFT;
  shift.key_code = ui::VKEY_SHIFT;

  bool press_shift = (key.implied_modifiers & WebInputEvent::kShiftKey) &&
                     !(modifiers & WebInputEvent::kShiftKey);
  int mods = modifiers | key.implied_modifiers;
  auto key_step = [this](int type, const KeySpec& spec, int step_mods) {
    return base::BindOnce(&InputInjector::DispatchKey, base::Unretained(this),
                          type, spec, step_mods);
  };

  if (press_shift) {
    Enqueue(RandomDelay(15, 45),
            key_step(static_cast<int>(WebInputEvent::Type::kRawKeyDown), shift,
                     modifiers | WebInputEvent::kShiftKey));
  }
  Enqueue(press_shift ? RandomDelay(30, 80) : base::TimeDelta(),
          key_step(static_cast<int>(WebInputEvent::Type::kRawKeyDown), key,
                   mods));
  bool produces_text =
      key.character &&
      !(mods & (WebInputEvent::kControlKey | WebInputEvent::kAltKey |
                WebInputEvent::kMetaKey));
  if (produces_text) {
    Enqueue(base::TimeDelta(),
            key_step(static_cast<int>(WebInputEvent::Type::kChar), key, mods));
  }
  Enqueue(RandomDelay(35, 95),
          key_step(static_cast<int>(WebInputEvent::Type::kKeyUp), key, mods));
  if (press_shift) {
    Enqueue(RandomDelay(10, 40),
            key_step(static_cast<int>(WebInputEvent::Type::kKeyUp), shift,
                     modifiers));
  }
}

void InputInjector::TypeText(const std::u16string& text,
                             base::TimeDelta min_delay,
                             base::TimeDelta max_delay,
                             DoneCallback done) {
  int min_ms = static_cast<int>(min_delay.InMilliseconds());
  int max_ms = std::max(min_ms, static_cast<int>(max_delay.InMilliseconds()));
  size_t index = 0;
  bool first = true;
  while (index < text.size()) {
    base_icu::UChar32 code_point = 0;
    if (!base::ReadUnicodeCharacter(text, &index, &code_point)) {
      code_point = 0xFFFD;
    }
    ++index;
    // Inter-key interval; a little longer after spaces, like word breaks.
    base::TimeDelta pause = first ? base::Milliseconds(base::RandIntInclusive(
                                        10, 40))
                                  : RandomDelay(min_ms, max_ms);
    if (code_point == ' ') {
      pause += RandomDelay(0, min_ms);
    }
    Enqueue(pause, base::DoNothing());
    EnqueueKeyStroke(KeySpecForCharacter(static_cast<char32_t>(code_point)),
                     0);
    first = false;
  }
  EnqueueDone(std::move(done));
}

void InputInjector::PressKey(std::string_view chord, DoneCallback done) {
  // "Control+Shift+t" -> modifiers [Control, Shift], key "t". A lone "+" is
  // the plus key.
  std::vector<std::string> parts;
  if (chord == "+") {
    parts.push_back("+");
  } else {
    parts = base::SplitString(chord, "+", base::TRIM_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY);
  }
  if (parts.empty()) {
    std::move(done).Run("Empty key");
    return;
  }

  std::vector<KeySpec> modifier_keys;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    std::optional<KeySpec> spec = KeySpecForName(parts[i]);
    int modifier = spec ? ModifierForKey(spec->key) : 0;
    if (!modifier) {
      std::move(done).Run("Unknown modifier: " + parts[i]);
      return;
    }
    modifier_keys.push_back(*spec);
  }
  std::optional<KeySpec> key = KeySpecForName(parts.back());
  if (!key) {
    std::move(done).Run("Unknown key: " + parts.back());
    return;
  }

  int held = 0;
  for (const KeySpec& modifier : modifier_keys) {
    held |= ModifierForKey(modifier.key);
    Enqueue(RandomDelay(20, 60),
            base::BindOnce(&InputInjector::DispatchKey, base::Unretained(this),
                           static_cast<int>(WebInputEvent::Type::kRawKeyDown),
                           modifier, held));
  }
  int key_modifier = ModifierForKey(key->key);
  if (key_modifier) {
    // Pressing a modifier on its own.
    Enqueue(RandomDelay(20, 60),
            base::BindOnce(&InputInjector::DispatchKey, base::Unretained(this),
                           static_cast<int>(WebInputEvent::Type::kRawKeyDown),
                           *key, held | key_modifier));
    Enqueue(RandomDelay(40, 100),
            base::BindOnce(&InputInjector::DispatchKey, base::Unretained(this),
                           static_cast<int>(WebInputEvent::Type::kKeyUp), *key,
                           held));
  } else {
    Enqueue(RandomDelay(20, 60), base::DoNothing());
    EnqueueKeyStroke(*key, held);
  }
  for (auto it = modifier_keys.rbegin(); it != modifier_keys.rend(); ++it) {
    held &= ~ModifierForKey(it->key);
    Enqueue(RandomDelay(15, 50),
            base::BindOnce(&InputInjector::DispatchKey, base::Unretained(this),
                           static_cast<int>(WebInputEvent::Type::kKeyUp), *it,
                           held));
  }
  EnqueueDone(std::move(done));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(InputInjector);

}  // namespace chromeaf
