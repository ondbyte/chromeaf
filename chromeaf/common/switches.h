// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEAF_COMMON_SWITCHES_H_
#define CHROMEAF_COMMON_SWITCHES_H_

namespace chromeaf::switches {

// TCP port for the MCP-over-HTTP control server. The server is only started
// when this switch is present.
extern const char kMcpPort[];

// Address the MCP server binds to. Defaults to 127.0.0.1.
extern const char kMcpHost[];

// If set, every request must carry "Authorization: Bearer <token>".
extern const char kMcpToken[];

// Disables the on-screen cursor overlay drawn for synthetic mouse input.
extern const char kMcpNoCursorOverlay[];

}  // namespace chromeaf::switches

#endif  // CHROMEAF_COMMON_SWITCHES_H_
