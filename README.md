# chromeaf

**chromeaf** is [ungoogled-chromium](https://github.com/ungoogled-software/ungoogled-chromium)
with a **Model Context Protocol (MCP) server built into the browser process**.
Launch it with `--chromeaf-mcp-port=9223` and an AI agent (or a script, or a
person with `curl`) can drive that browser instance over HTTP: read pages,
find clickable elements, take screenshots, move the mouse like a human, click,
type, scroll and manage tabs.

It is a normal browser to every website. The control layer:

- does **not** use DevTools, CDP, `--remote-debugging-*`, WebDriver or
  extensions; there is no `navigator.webdriver`, no "controlled by automated
  test software" bar, and no script is ever injected into pages;
- injects mouse/wheel input at the window tree host, the same place OS input
  enters Chromium, so events are hit-tested like real ones and pages see
  ordinary `isTrusted` events;
- reads the DOM through Blink's public C++ API from a renderer-side agent that
  is invisible to the page.

Everything is C++ inside the Chromium tree; upstream files are touched by just
two small patches.

## Repository layout

```
Makefile                    convenience entry point (make help)
ungoogled-chromium/         submodule, pinned release (version, patches, utils)
platform/portablelinux/     submodule, ungoogled-chromium's Linux build layer
chromeaf/                   our sources, copied to <chromium>/chromeaf
  common/                   page_agent.mojom, command-line switches
  browser/mcp/              HTTP transport, JSON-RPC/MCP protocol, tool registry
  browser/tools/            tab, page, screenshot and input tools
  browser/input/            human mouse-path model, input injector
  browser/ui/               on-screen cursor overlay (desktop)
  browser/tabs/             desktop/Android tab-list adapters
  renderer/                 PageAgent (DOM/HTML/clickables via Blink C++ API)
  test/                     chromeaf_unittests
patches/                    quilt series applied after ungoogled + portablelinux
build/args/                 GN args (linux.gn, dev.gn, android.gn, local.gn)
scripts/chromeaf.sh         the build steps the Makefile runs
scripts/mcp_smoke_test.py   end-to-end test of every tool
```

Pinned versions: Chromium **155.0.8059.39**, ungoogled-chromium
**155.0.8059.39-1**, portablelinux **155.0.8059.39-1**.

## Building on Linux

A full Chromium build needs roughly 100 GB of disk, 16 GB+ RAM and several
hours of CPU. The host packages are those in
`platform/portablelinux/docker/build.Dockerfile` (clang, ninja, node with
global `typescript` and `esbuild`, gperf, go, clang-format, …); `make
check-deps` reports what is missing.

```sh
git clone --recursive https://github.com/ondbyte/chromeaf && cd chromeaf
make deps        # submodules + host tool check
make fetch       # Chromium source tarball (or FETCH=clone for a git checkout)
make prepare     # prune binaries, apply ungoogled + portablelinux + chromeaf
                 # patches, domain substitution, download clang/rust/sysroot
make linux       # gn gen + ninja chrome chromeaf_unittests
make test        # unit tests + end-to-end smoke test
make package     # build/release/chromeaf-<version>-<cpu>-linux.tar.xz
```

Or build everything in the portablelinux Debian image:

```sh
make docker-linux
```

Useful variables: `BUILD_TYPE=dev` (component build with DCHECKs, much faster
to iterate), `JOBS=N`, `TARGET_CPU=arm64`, `OUT=...`. Extra GN args go in
`build/args/local.gn`. After editing files in `chromeaf/`, `make linux` copies
them into the tree again (or run `make overlay`).

The upstream build is untouched: `cd build/src && out/chromeaf/gn args
out/chromeaf` and `ninja -C out/chromeaf <target>` work as usual.

## Running

```sh
make run                                  # MCP on http://127.0.0.1:9223/mcp
make run MCP_PORT=9333 MCP_TOKEN=secret   # require Authorization: Bearer secret
```

or directly:

```sh
chrome --chromeaf-mcp-port=9223 [--chromeaf-mcp-host=127.0.0.1] \
       [--chromeaf-mcp-token=TOKEN] [--chromeaf-mcp-no-cursor-overlay]
```

Without `--chromeaf-mcp-port` nothing listens and the browser behaves exactly
like ungoogled-chromium. The server binds to loopback by default, rejects
requests whose `Host`/`Origin` is not loopback (DNS-rebinding protection) and
can require a bearer token. **Anyone who can reach the port controls the
browser** — only bind elsewhere with a token, behind a firewall.

### Connecting an MCP client

The endpoint speaks MCP *Streamable HTTP* (JSON responses) at `/mcp`:

```sh
claude mcp add --transport http chromeaf http://127.0.0.1:9223/mcp
```

```sh
curl -s localhost:9223/mcp -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
       "params":{"name":"get-tabs","arguments":{}}}'
```

## Tools

Coordinates are CSS/DIP pixels relative to the top-left of the tab's viewport
(the same numbers `getBoundingClientRect()` gives). Every page/input tool takes
an optional `tab_id`; the default is the active tab of the focused window.

| Tool | What it does |
|---|---|
| `get-tabs` | Windows and their tabs: `tab_id`, title, url, active, loading |
| `create-tab` | Open a tab (`url`, `foreground`, `window_id`, `wait_for_load`) |
| `close-tab` / `activate-tab` | Close / switch to a tab |
| `navigate` | Load a URL as if typed into the omnibox (waits for load) |
| `go-back` / `go-forward` / `reload` / `wait-for-load` | History and load control |
| `get-whole-page-as-html` | Live DOM serialized as HTML |
| `get-page-text` | Visible text of the page |
| `get-clickables` | Links, buttons, inputs, ARIA widgets, click handlers and pointer-cursor elements with `id`, label, bounds and center |
| `get-screen-shot` | PNG/JPEG of the viewport, optional clip and downscale |
| `move-cursor` | Human-like pointer movement to `x,y` or `clickable_id` |
| `click` / `rightclick` / `double-click` | Move there, then press/hold/release like a person |
| `scroll` | Mouse-wheel scrolling by `delta_x`/`delta_y` pixels |
| `type-text` | Types into the focused element key by key with human timing |
| `press-key` | Keys and chords: `Enter`, `Escape`, `ArrowDown`, `Control+a`, … |
| `get-cursor-position` | Where the synthetic pointer currently is |

Typical loop: `get-clickables` → `click {clickable_id}` → `type-text` →
`press-key Enter` → `get-page-text` / `get-screen-shot`.

### How the human-like input works

`move-cursor` (and the movement part of every click) follows a cubic Bézier
path with randomized curvature, a minimum-jerk velocity profile (slow start,
fast middle, slow arrival), small hand tremor that fades near the target,
irregular 7–16 ms sampling, Fitts's-law duration based on distance and target
size, and an occasional small overshoot-and-correct on long moves. Clicks aim
near — not exactly at — the element center, hover briefly, and hold the
button for 55–130 ms. Typing uses per-key delays with longer pauses at word
breaks and presses Shift for capitals. A small pointer overlay (browser UI,
not part of the page) shows the agent's cursor to anyone watching; disable it
with `--chromeaf-mcp-no-cursor-overlay`.

## Android (experimental)

The source tarball has no Android SDK/NDK, so Android uses a depot_tools
checkout of the same Chromium tag:

```sh
make android-fetch            # large: ~100 GB checkout with Android deps
make android-prepare          # chromeaf patches (ANDROID_UC_PATCHES=1 adds ungoogled's)
make android ANDROID_CPU=arm64
make android-install          # adb install, write the command-line file, adb forward
```

Android reads `/data/local/tmp/chrome-command-line` only on debuggable builds
or with *chrome://flags#enable-command-line-on-non-rooted-devices* enabled.
On Android the tab tools use `TabModel`; input is forwarded to the page's
`RenderWidgetHost` and the cursor overlay is not drawn yet.

## Architecture

```
 MCP client ──HTTP──► McpHttpServer (net::HttpServer, own IO thread)
                          │ JSON-RPC body
                          ▼  (UI thread)
                      McpProtocol ──► ToolRegistry ──► tools/*
                                                        │
          ┌──────────────────────┬──────────────────────┼──────────────────────┐
          ▼                      ▼                      ▼                      ▼
   TabListInterface      PageAgentHost ──mojo──► PageAgent (renderer)   InputInjector
   (tab strip / TabModel)                         Blink public C++ API   aura host event
                                                                         source / RWH
```

The two upstream hooks:

- `patches/chromeaf/browser-main-extra-parts.patch`:
  `ChromeBrowserMainParts::Create()` adds `CreateChromeafMainExtraParts()`;
  `//chrome/browser/ui` links `//chromeaf:browser`.
- `patches/chromeaf/renderer-page-agent.patch`:
  `ChromeContentRendererClient::RenderFrameCreated()` attaches a `PageAgent`.

## Updating to a new Chromium

1. Move both submodules to the new matching tags.
2. `make fetch prepare`; if a chromeaf patch no longer applies, fix it in the
   tree and regenerate it with `diff -u` against the post-ungoogled file.
3. Build; API drift is confined to `chromeaf/`.
