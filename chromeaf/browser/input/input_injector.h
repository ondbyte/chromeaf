// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_INPUT_INPUT_INJECTOR_H_
#define CHROMEAF_BROWSER_INPUT_INPUT_INJECTOR_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chromeaf/browser/input/human_mouse_path.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/geometry/vector2d_f.h"

namespace chromeaf {

class CursorOverlay;

// Produces mouse, wheel and keyboard input for one tab the way a person
// would: the cursor travels along curved, irregularly sampled paths, buttons
// are held for a realistic time, keys are typed one by one with variable
// delays.
//
// On Aura (Linux desktop) mouse and wheel events are injected at the window
// tree host, i.e. exactly where events from the OS enter Chromium, so they are
// hit-tested like real input (browser bubbles, cross-site iframes, ...).
// Keyboard events go to the focused frame's RenderWidgetHost. Nothing goes
// through DevTools and pages observe ordinary trusted events.
//
// Operations are queued and run sequentially, so concurrent tool calls never
// interleave their events. Positions are DIPs relative to the viewport.
class InputInjector : public content::WebContentsObserver,
                      public content::WebContentsUserData<InputInjector> {
 public:
  enum class Button { kLeft, kMiddle, kRight };

  // Called when an operation finished. |error| is empty on success.
  using DoneCallback = base::OnceCallback<void(std::string error)>;

  InputInjector(const InputInjector&) = delete;
  InputInjector& operator=(const InputInjector&) = delete;
  ~InputInjector() override;

  static InputInjector* For(content::WebContents* contents);

  // Current (synthetic) cursor position. Before the first movement this is a
  // plausible resting position inside the viewport.
  gfx::PointF cursor_position();
  gfx::Size viewport_size();

  void MoveTo(const gfx::PointF& target,
              const HumanMousePathOptions& options,
              DoneCallback done);
  // Moves to |target| (if not already there), then presses and releases
  // |button| |click_count| times.
  void Click(const gfx::PointF& target,
             Button button,
             int click_count,
             const HumanMousePathOptions& options,
             DoneCallback done);
  // Scrolls by |delta| DIPs (positive y = down) with the cursor at |at|,
  // moving there first. Emitted as a series of wheel notches.
  void Scroll(const gfx::PointF& at,
              const gfx::Vector2dF& delta,
              DoneCallback done);
  // Types |text| into the focused element.
  void TypeText(const std::u16string& text,
                base::TimeDelta min_delay,
                base::TimeDelta max_delay,
                DoneCallback done);
  // Presses a key or chord such as "Enter", "Tab", "a", "Control+a",
  // "Shift+ArrowDown". Key names are DOM KeyboardEvent.key values.
  void PressKey(std::string_view chord, DoneCallback done);

 private:
  friend class content::WebContentsUserData<InputInjector>;
  explicit InputInjector(content::WebContents* contents);

  enum class MouseAction { kMove, kPress, kRelease };

  struct Step {
    Step(base::TimeDelta delay, base::OnceClosure closure);
    Step(Step&&);
    Step& operator=(Step&&);
    ~Step();
    base::TimeDelta delay;
    base::OnceClosure closure;
  };

  struct KeySpec {
    ui::DomKey key;
    ui::DomCode code = ui::DomCode::NONE;
    ui::KeyboardCode key_code = ui::VKEY_UNKNOWN;
    // Blink modifier needed to produce the key (e.g. shift for 'A').
    int implied_modifiers = 0;
    // Character produced, or 0 for non-printing keys.
    char32_t character = 0;
  };

  // Queue management.
  void Enqueue(base::TimeDelta delay_before, base::OnceClosure step);
  void EnqueueDone(DoneCallback done);
  void RunNextStep();

  // Queues the steps of a human-like move from the cursor's position at the
  // time the steps run.
  void EnqueueMove(const gfx::PointF& target,
                   const HumanMousePathOptions& options);
  void StartMove(gfx::PointF target, HumanMousePathOptions options);
  void InsertStepsAtFront(std::vector<Step> steps);
  void EnqueueKeyStroke(const KeySpec& key, int modifiers);

  // Low level dispatch (runs as queued steps).
  void DispatchMouse(MouseAction action,
                     const gfx::PointF& point,
                     Button button,
                     int click_count);
  void DispatchWheel(const gfx::PointF& point, const gfx::Vector2d& offset);
  void DispatchKey(int type, const KeySpec& key, int modifiers);
  void UpdateCursor(const gfx::PointF& point);

  static std::optional<KeySpec> KeySpecForName(std::string_view name);
  static KeySpec KeySpecForCharacter(char32_t character);

  base::circular_deque<Step> steps_;
  base::OneShotTimer timer_;
  bool step_running_ = false;

  // First failure seen while running the current operation's steps.
  std::string pending_error_;
  std::optional<gfx::PointF> cursor_;
  int pressed_buttons_ = 0;  // ui::EF_*_MOUSE_BUTTON flags.
  std::unique_ptr<CursorOverlay> overlay_;

  base::WeakPtrFactory<InputInjector> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_INPUT_INPUT_INJECTOR_H_
