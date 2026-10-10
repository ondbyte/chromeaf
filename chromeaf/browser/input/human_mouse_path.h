// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_INPUT_HUMAN_MOUSE_PATH_H_
#define CHROMEAF_BROWSER_INPUT_HUMAN_MOUSE_PATH_H_

#include <optional>
#include <vector>

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "ui/gfx/geometry/point_f.h"

namespace chromeaf {

struct MousePathPoint {
  gfx::PointF position;
  // Offset from the start of the movement.
  base::TimeDelta time;
};

struct HumanMousePathOptions {
  // Total duration of the movement. When unset it is derived from Fitts's law
  // using |target_width|.
  std::optional<base::TimeDelta> duration;
  // Approximate size of the thing being pointed at, in DIPs.
  double target_width = 24.0;
  // Interval between emitted mouse moves (a random value in this range is
  // picked per step, like a real mouse's irregular report rate).
  base::TimeDelta min_step = base::Milliseconds(7);
  base::TimeDelta max_step = base::Milliseconds(16);
  // Maximum hand-tremor jitter in DIPs, applied mid-path and fading to zero at
  // the destination.
  double jitter = 0.8;
  // Whether long movements may overshoot slightly and correct, as people do.
  bool allow_overshoot = true;
};

// Returns a uniformly distributed random number in [0, 1).
using RandomSource = base::RepeatingCallback<double()>;

// Movement time predicted by Fitts's law (Shannon formulation), clamped to a
// plausible human range.
base::TimeDelta FittsDuration(double distance, double target_width);

// Generates the sequence of cursor positions a person would produce moving a
// mouse from |from| to |to|: a curved cubic Bezier path with minimum-jerk
// velocity profile (slow-fast-slow), small tremor, irregular sampling and an
// optional overshoot-and-correct. The last point is exactly |to|. Times are
// strictly increasing. |random| defaults to base::RandDouble.
std::vector<MousePathPoint> GenerateHumanMousePath(
    const gfx::PointF& from,
    const gfx::PointF& to,
    const HumanMousePathOptions& options,
    RandomSource random = RandomSource());

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_INPUT_HUMAN_MOUSE_PATH_H_
