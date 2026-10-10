// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/input/human_mouse_path.h"

#include <algorithm>
#include <cmath>

#include "base/functional/bind.h"
#include "base/rand_util.h"
#include "ui/gfx/geometry/vector2d_f.h"

namespace chromeaf {

namespace {

constexpr base::TimeDelta kMinMoveDuration = base::Milliseconds(90);
constexpr base::TimeDelta kMaxMoveDuration = base::Milliseconds(1600);

double Lerp(double a, double b, double t) {
  return a + (b - a) * t;
}

gfx::PointF Bezier(const gfx::PointF& p0,
                   const gfx::PointF& p1,
                   const gfx::PointF& p2,
                   const gfx::PointF& p3,
                   double t) {
  double u = 1.0 - t;
  double b0 = u * u * u;
  double b1 = 3 * u * u * t;
  double b2 = 3 * u * t * t;
  double b3 = t * t * t;
  return gfx::PointF(
      static_cast<float>(b0 * p0.x() + b1 * p1.x() + b2 * p2.x() +
                         b3 * p3.x()),
      static_cast<float>(b0 * p0.y() + b1 * p1.y() + b2 * p2.y() +
                         b3 * p3.y()));
}

// Minimum-jerk position profile: smooth acceleration and deceleration.
double MinimumJerk(double t) {
  return t * t * t * (10.0 + t * (-15.0 + 6.0 * t));
}

class Rng {
 public:
  explicit Rng(RandomSource source) : source_(std::move(source)) {}
  double Uniform() { return source_.Run(); }
  double Range(double lo, double hi) { return Lerp(lo, hi, Uniform()); }
  // Roughly normal in [-1, 1] (sum of uniforms).
  double Centered() { return (Uniform() + Uniform() + Uniform()) / 1.5 - 1.0; }

 private:
  RandomSource source_;
};

// Appends one curved segment from |from| to |to| lasting |duration|, starting
// at |start_time|. The first point of the segment is not emitted (the caller
// is already there).
void AppendSegment(const gfx::PointF& from,
                   const gfx::PointF& to,
                   base::TimeDelta start_time,
                   base::TimeDelta duration,
                   const HumanMousePathOptions& options,
                   Rng& rng,
                   std::vector<MousePathPoint>& out) {
  gfx::Vector2dF delta = to - from;
  double distance = delta.Length();
  gfx::Vector2dF normal(-delta.y(), delta.x());
  if (distance > 0) {
    normal.InvScale(static_cast<float>(distance));
  }

  // Curvature grows with distance but is capped; people arc more on long
  // movements. Both control points bend to the same side most of the time.
  double max_bend = std::min(distance * 0.22, 160.0);
  double side = rng.Uniform() < 0.5 ? -1.0 : 1.0;
  double bend1 = side * rng.Range(0.2, 1.0) * max_bend;
  double bend2 = side * rng.Range(-0.2, 0.8) * max_bend;
  gfx::PointF c1 = from + gfx::ScaleVector2d(delta, static_cast<float>(rng.Range(0.2, 0.4))) +
                   gfx::ScaleVector2d(normal, static_cast<float>(bend1));
  gfx::PointF c2 = from + gfx::ScaleVector2d(delta, static_cast<float>(rng.Range(0.6, 0.85))) +
                   gfx::ScaleVector2d(normal, static_cast<float>(bend2));

  double min_step_ms = options.min_step.InMillisecondsF();
  double max_step_ms = std::max(options.max_step.InMillisecondsF(),
                                min_step_ms);
  double total_ms = std::max(duration.InMillisecondsF(), min_step_ms);

  double elapsed_ms = 0;
  while (true) {
    elapsed_ms += std::max(1.0, rng.Range(min_step_ms, max_step_ms));
    bool last = elapsed_ms >= total_ms;
    if (last) {
      elapsed_ms = total_ms;
    }
    double s = MinimumJerk(elapsed_ms / total_ms);
    gfx::PointF p = last ? to : Bezier(from, c1, c2, to, s);
    if (!last && options.jitter > 0) {
      // Tremor fades out as the hand homes in on the target.
      double amplitude = options.jitter * (1.0 - s);
      p.Offset(static_cast<float>(rng.Centered() * amplitude),
               static_cast<float>(rng.Centered() * amplitude));
    }
    out.push_back({p, start_time + base::Milliseconds(elapsed_ms)});
    if (last) {
      return;
    }
  }
}

}  // namespace

base::TimeDelta FittsDuration(double distance, double target_width) {
  // T = a + b * log2(D / W + 1), with constants typical for mouse pointing.
  constexpr double kA = 110.0;  // ms
  constexpr double kB = 140.0;  // ms per bit
  double width = std::max(target_width, 1.0);
  double ms = kA + kB * std::log2(std::max(distance, 0.0) / width + 1.0);
  return std::clamp(base::Milliseconds(ms), kMinMoveDuration,
                    kMaxMoveDuration);
}

std::vector<MousePathPoint> GenerateHumanMousePath(
    const gfx::PointF& from,
    const gfx::PointF& to,
    const HumanMousePathOptions& options,
    RandomSource random) {
  Rng rng(random ? std::move(random) : base::BindRepeating(&base::RandDouble));
  std::vector<MousePathPoint> path;

  double distance = (to - from).Length();
  if (distance < 0.5) {
    path.push_back({to, base::TimeDelta()});
    return path;
  }

  base::TimeDelta duration =
      options.duration.value_or(FittsDuration(distance, options.target_width) *
                                rng.Range(0.85, 1.15));
  duration = std::max(duration, options.min_step);

  bool overshoot = options.allow_overshoot && distance > 180.0 &&
                   rng.Uniform() < 0.3;
  if (!overshoot) {
    AppendSegment(from, to, base::TimeDelta(), duration, options, rng, path);
    return path;
  }

  // Overshoot past the target along the direction of travel, then make a
  // short corrective movement back.
  gfx::Vector2dF direction = to - from;
  direction.InvScale(static_cast<float>(distance));
  gfx::Vector2dF normal(-direction.y(), direction.x());
  gfx::PointF overshoot_point =
      to + gfx::ScaleVector2d(direction, static_cast<float>(rng.Range(4.0, 14.0))) +
      gfx::ScaleVector2d(normal, static_cast<float>(rng.Range(-5.0, 5.0)));
  base::TimeDelta main_duration = duration * 0.85;
  AppendSegment(from, overshoot_point, base::TimeDelta(), main_duration,
                options, rng, path);
  base::TimeDelta correction = base::Milliseconds(rng.Range(70.0, 160.0));
  HumanMousePathOptions correction_options = options;
  correction_options.jitter = options.jitter * 0.5;
  AppendSegment(overshoot_point, to, path.back().time, correction,
                correction_options, rng, path);
  return path;
}

}  // namespace chromeaf
