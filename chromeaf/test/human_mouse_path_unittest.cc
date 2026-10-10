// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/browser/input/human_mouse_path.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "base/functional/bind.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeaf {
namespace {

// Deterministic pseudo random source in [0, 1).
RandomSource SeededRandom(uint32_t seed) {
  return base::BindRepeating(
      [](uint32_t* state) {
        *state = *state * 1664525u + 1013904223u;
        return (*state >> 8) / static_cast<double>(1u << 24);
      },
      base::Owned(new uint32_t(seed)));
}

double DistanceToSegment(const gfx::PointF& p,
                         const gfx::PointF& a,
                         const gfx::PointF& b) {
  gfx::Vector2dF ab = b - a;
  gfx::Vector2dF ap = p - a;
  double t = std::clamp(
      (ap.x() * ab.x() + ap.y() * ab.y()) / ab.LengthSquared(), 0.0, 1.0);
  gfx::PointF closest = a + gfx::ScaleVector2d(ab, static_cast<float>(t));
  return (p - closest).Length();
}

TEST(HumanMousePathTest, EndsExactlyAtTargetWithIncreasingTime) {
  for (uint32_t seed = 0; seed < 200; ++seed) {
    gfx::PointF from(10, 20);
    gfx::PointF to(800, 450);
    std::vector<MousePathPoint> path = GenerateHumanMousePath(
        from, to, HumanMousePathOptions(), SeededRandom(seed));
    ASSERT_GT(path.size(), 5u) << seed;
    EXPECT_EQ(to.x(), path.back().position.x()) << seed;
    EXPECT_EQ(to.y(), path.back().position.y()) << seed;
    base::TimeDelta previous;
    for (const MousePathPoint& point : path) {
      EXPECT_GT(point.time, previous) << seed;
      previous = point.time;
    }
  }
}

TEST(HumanMousePathTest, DurationFollowsOptionsAndFitts) {
  HumanMousePathOptions options;
  options.duration = base::Milliseconds(400);
  options.allow_overshoot = false;
  std::vector<MousePathPoint> path = GenerateHumanMousePath(
      gfx::PointF(0, 0), gfx::PointF(300, 0), options, SeededRandom(1));
  EXPECT_EQ(base::Milliseconds(400), path.back().time);

  // Farther and smaller targets take longer.
  EXPECT_LT(FittsDuration(100, 50), FittsDuration(1000, 50));
  EXPECT_LT(FittsDuration(500, 100), FittsDuration(500, 5));
  EXPECT_GE(FittsDuration(0, 10), base::Milliseconds(90));
  EXPECT_LE(FittsDuration(100000, 1), base::Milliseconds(1600));
}

TEST(HumanMousePathTest, PathIsCurvedButBounded) {
  gfx::PointF from(100, 100);
  gfx::PointF to(700, 100);
  int curved = 0;
  for (uint32_t seed = 0; seed < 50; ++seed) {
    HumanMousePathOptions options;
    options.allow_overshoot = false;
    std::vector<MousePathPoint> path =
        GenerateHumanMousePath(from, to, options, SeededRandom(seed));
    double max_deviation = 0;
    for (const MousePathPoint& point : path) {
      max_deviation =
          std::max(max_deviation, DistanceToSegment(point.position, from, to));
    }
    // Never wildly off course...
    EXPECT_LT(max_deviation, 200.0) << seed;
    // ...but usually not a ruler-straight line either.
    if (max_deviation > 3.0) {
      ++curved;
    }
  }
  EXPECT_GT(curved, 40);
}

TEST(HumanMousePathTest, StepIntervalsAreIrregular) {
  HumanMousePathOptions options;
  options.allow_overshoot = false;
  std::vector<MousePathPoint> path = GenerateHumanMousePath(
      gfx::PointF(0, 0), gfx::PointF(600, 300), options, SeededRandom(7));
  std::set<int64_t> intervals;
  for (size_t i = 1; i + 1 < path.size(); ++i) {
    base::TimeDelta interval = path[i].time - path[i - 1].time;
    EXPECT_GE(interval, base::Milliseconds(1));
    EXPECT_LE(interval, options.max_step + base::Milliseconds(1));
    intervals.insert(interval.InMicroseconds());
  }
  EXPECT_GT(intervals.size(), 3u);
}

TEST(HumanMousePathTest, TinyMoveIsSinglePoint) {
  std::vector<MousePathPoint> path = GenerateHumanMousePath(
      gfx::PointF(5, 5), gfx::PointF(5.2f, 5.1f), HumanMousePathOptions(),
      SeededRandom(3));
  ASSERT_EQ(1u, path.size());
  EXPECT_FLOAT_EQ(5.2f, path[0].position.x());
  EXPECT_FLOAT_EQ(5.1f, path[0].position.y());
}

}  // namespace
}  // namespace chromeaf
