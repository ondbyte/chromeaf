// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_BROWSER_TOOLS_TOOLS_H_
#define CHROMEAF_BROWSER_TOOLS_TOOLS_H_

namespace chromeaf {

class ToolRegistry;

// get-tabs, create-tab, close-tab, activate-tab, navigate, go-back,
// go-forward, reload, wait-for-load.
void RegisterTabTools(ToolRegistry& registry);

// get-whole-page-as-html, get-page-text, get-clickables.
void RegisterPageTools(ToolRegistry& registry);

// get-screen-shot.
void RegisterScreenshotTool(ToolRegistry& registry);

// move-cursor, click, rightclick, double-click, scroll, type-text, press-key,
// get-cursor-position.
void RegisterInputTools(ToolRegistry& registry);

// Registers everything above.
void RegisterAllTools(ToolRegistry& registry);

}  // namespace chromeaf

#endif  // CHROMEAF_BROWSER_TOOLS_TOOLS_H_
