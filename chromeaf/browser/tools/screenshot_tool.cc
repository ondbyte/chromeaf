// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "chromeaf/browser/mcp/tool_registry.h"
#include "chromeaf/browser/tools/tool_util.h"
#include "chromeaf/browser/tools/tools.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/size.h"

namespace chromeaf {

namespace {

constexpr base::TimeDelta kCaptureTimeout = base::Seconds(10);

struct EncodedImage {
  std::string base64;
  std::string mime_type;
  int width = 0;
  int height = 0;
};

std::optional<EncodedImage> Encode(SkBitmap bitmap, bool jpeg, int quality) {
  std::optional<std::vector<uint8_t>> bytes;
  if (jpeg) {
    SkPixmap pixmap;
    if (bitmap.peekPixels(&pixmap)) {
      bytes = gfx::JPEGCodec::Encode(pixmap, quality);
    }
  } else {
    bytes = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap,
                                              /*discard_transparency=*/true);
  }
  if (!bytes) {
    return std::nullopt;
  }
  return EncodedImage{base::Base64Encode(*bytes),
                      jpeg ? "image/jpeg" : "image/png", bitmap.width(),
                      bitmap.height()};
}

void OnEncoded(ToolCallback callback,
               float pixels_per_dip,
               std::optional<EncodedImage> image) {
  if (!image) {
    std::move(callback).Run(ToolResult::Error("Failed to encode screenshot"));
    return;
  }
  ToolResult result =
      ToolResult::Image(std::move(image->base64), std::move(image->mime_type));
  result.AddText(base::StrCat(
      {"Screenshot ", base::NumberToString(image->width), "x",
       base::NumberToString(image->height), " px; ",
       base::NumberToString(pixels_per_dip),
       " image px per viewport DIP. Divide image coordinates by this factor "
       "(and add the clip origin, if any) to get coordinates for "
       "move-cursor/click."}));
  std::move(callback).Run(std::move(result));
}

void OnCaptured(ToolCallback callback,
                bool jpeg,
                int quality,
                float pixels_per_dip,
                const content::CopyFromSurfaceResult& result) {
  if (!result.has_value() || result->bitmap.drawsNothing()) {
    std::move(callback).Run(ToolResult::Error(
        "Could not capture the page (is the tab visible and painted?)"));
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&Encode, result->bitmap, jpeg, quality),
      base::BindOnce(&OnEncoded, std::move(callback), pixels_per_dip));
}

void GetScreenshot(const base::DictValue& args, ToolCallback callback) {
  base::expected<ResolvedTab, std::string> resolved = ResolveTab(args);
  if (!resolved.has_value()) {
    std::move(callback).Run(ToolResult::Error(resolved.error()));
    return;
  }
  content::RenderWidgetHostView* view =
      resolved->contents->GetRenderWidgetHostView();
  if (!view) {
    std::move(callback).Run(ToolResult::Error("The tab has no view"));
    return;
  }

  const std::string* format = GetStringArg(args, "format");
  bool jpeg = format && *format == "jpeg";
  int quality = std::clamp(GetIntArg(args, "quality").value_or(80), 1, 100);
  double scale = std::clamp(GetNumberArg(args, "scale").value_or(1.0), 0.05,
                            1.0);

  // Source rect in DIPs; empty means the whole viewport.
  gfx::Rect viewport(view->GetViewBounds().size());
  gfx::Rect clip = viewport;
  std::optional<double> x = GetNumberArg(args, "x");
  std::optional<double> y = GetNumberArg(args, "y");
  std::optional<double> width = GetNumberArg(args, "width");
  std::optional<double> height = GetNumberArg(args, "height");
  if (x || y || width || height) {
    clip = gfx::ToEnclosingRect(gfx::RectF(
        x.value_or(0), y.value_or(0),
        width.value_or(viewport.width() - x.value_or(0)),
        height.value_or(viewport.height() - y.value_or(0))));
    clip.Intersect(viewport);
    if (clip.IsEmpty()) {
      std::move(callback).Run(
          ToolResult::Error("Clip rectangle is outside the viewport"));
      return;
    }
  }

  float device_scale = view->GetDeviceScaleFactor();
  float pixels_per_dip = static_cast<float>(device_scale * scale);
  gfx::Size output_size =
      gfx::ScaleToCeiledSize(clip.size(), pixels_per_dip);

  view->CopyFromSurface(
      clip == viewport ? gfx::Rect() : clip, output_size, kCaptureTimeout,
      base::BindOnce(&OnCaptured,
                     ErrorIfDropped(std::move(callback),
                                    "Screenshot request was dropped"),
                     jpeg, quality, pixels_per_dip));
}

}  // namespace

void RegisterScreenshotTool(ToolRegistry& registry) {
  registry.Register(
      {"get-screen-shot",
       "Captures what the tab currently shows (the visible viewport) as an "
       "image. Use when HTML is not enough to understand the page (canvas, "
       "images, complex layouts).",
       SchemaBuilder()
           .Integer(kTabIdArg, kTabIdDescription)
           .Enum("format", "Image format (default png).", {"png", "jpeg"})
           .Integer("quality", "JPEG quality 1-100 (default 80).")
           .Number("scale",
                   "Downscale factor 0.05-1 relative to device pixels "
                   "(default 1).")
           .Number("x", "Clip left, in viewport DIPs.")
           .Number("y", "Clip top, in viewport DIPs.")
           .Number("width", "Clip width, in viewport DIPs.")
           .Number("height", "Clip height, in viewport DIPs.")
           .Build(),
       base::BindRepeating(&GetScreenshot)});
}

}  // namespace chromeaf
