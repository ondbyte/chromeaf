// Copyright 2026 The chromeaf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeaf/renderer/page_agent.h"

#include <string>
#include <string_view>
#include <utility>

#include "base/containers/fixed_flat_set.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_element_collection.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_frame_content_dumper.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_node.h"
#include "ui/gfx/geometry/rect.h"
#include "url/gurl.h"

namespace chromeaf {

namespace {

constexpr size_t kMaxLabelLength = 150;

constexpr auto kInteractiveTags = base::MakeFixedFlatSet<std::string_view>({
    "button",
    "select",
    "textarea",
    "summary",
    "details",
    "option",
    "video",
    "audio",
});

constexpr auto kInteractiveRoles = base::MakeFixedFlatSet<std::string_view>({
    "button",
    "checkbox",
    "combobox",
    "link",
    "listbox",
    "menuitem",
    "menuitemcheckbox",
    "menuitemradio",
    "option",
    "radio",
    "searchbox",
    "slider",
    "spinbutton",
    "switch",
    "tab",
    "textbox",
    "treeitem",
});

std::string Lower(const blink::WebString& string) {
  return base::ToLowerASCII(string.Utf8());
}

std::string Attribute(const blink::WebElement& element, const char* name) {
  blink::WebString web_name = blink::WebString::FromAscii(name);
  return element.HasAttribute(web_name)
             ? element.GetAttribute(web_name).Utf8()
             : std::string();
}

std::string EscapeAttribute(std::string_view value) {
  std::string escaped;
  for (char c : value) {
    switch (c) {
      case '&':
        escaped += "&amp;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      default:
        escaped += c;
    }
  }
  return escaped;
}

std::string Shorten(std::string text) {
  text = base::CollapseWhitespaceASCII(text, true);
  if (text.size() > kMaxLabelLength) {
    text.resize(base::TruncateUTF8ToByteSize(text, kMaxLabelLength).size());
    text += "…";
  }
  return text;
}

bool HasPointerCursor(blink::WebElement element) {
  return element.GetComputedValue(blink::WebString::FromAscii("cursor"))
             .Utf8() == "pointer";
}

// Whether a person would consider |element| something to click or type into.
bool IsInteractive(blink::WebElement& element, const std::string& tag) {
  if (element.HasAttribute(blink::WebString::FromAscii("disabled")) ||
      Attribute(element, "aria-disabled") == "true") {
    return false;
  }
  if (tag == "a" || tag == "area") {
    if (element.HasAttribute(blink::WebString::FromAscii("href"))) {
      return true;
    }
  } else if (tag == "input") {
    return base::ToLowerASCII(Attribute(element, "type")) != "hidden";
  } else if (kInteractiveTags.contains(tag)) {
    return true;
  }

  std::string role = base::ToLowerASCII(Attribute(element, "role"));
  if (kInteractiveRoles.contains(role)) {
    return true;
  }
  if (element.HasAttribute(blink::WebString::FromAscii("onclick")) ||
      element.IsEditable()) {
    return true;
  }
  std::string tabindex = Attribute(element, "tabindex");
  if (!tabindex.empty() && tabindex != "-1") {
    return true;
  }

  // Script-driven widgets often only reveal themselves through the pointer
  // cursor. Cursor is inherited, so only report the outermost such element.
  if (HasPointerCursor(element)) {
    blink::WebNode parent = element.ParentNode();
    if (parent.IsNull() || !parent.IsElementNode()) {
      return true;
    }
    return !HasPointerCursor(parent.To<blink::WebElement>());
  }
  return false;
}

std::string LabelFor(const blink::WebElement& element, const std::string& tag) {
  std::string aria_label = Attribute(element, "aria-label");
  if (!aria_label.empty()) {
    return Shorten(aria_label);
  }
  if (tag == "input") {
    std::string type = base::ToLowerASCII(Attribute(element, "type"));
    for (const char* attribute : {"value", "placeholder", "title", "name"}) {
      std::string value = Attribute(element, attribute);
      if (!value.empty() && !(type == "password" &&
                              std::string_view(attribute) == "value")) {
        return Shorten(value);
      }
    }
    return std::string();
  }
  std::string text = Shorten(
      element.TextContentAbridged(kMaxLabelLength * 2).Utf8());
  if (!text.empty()) {
    return text;
  }
  for (const char* attribute : {"title", "alt", "placeholder", "value"}) {
    std::string value = Attribute(element, attribute);
    if (!value.empty()) {
      return Shorten(value);
    }
  }
  // An icon link: use the alt text of a contained image.
  blink::WebElement image =
      element.QuerySelector(blink::WebString::FromAscii("img[alt]"));
  if (!image.IsNull()) {
    return Shorten(Attribute(image, "alt"));
  }
  return std::string();
}

bool IsHiddenByStyle(blink::WebElement& element) {
  return element.GetComputedValue(blink::WebString::FromAscii("visibility"))
                 .Utf8() == "hidden" ||
         element
                 .GetComputedValue(
                     blink::WebString::FromAscii("pointer-events"))
                 .Utf8() == "none";
}

}  // namespace

// static
void PageAgent::MaybeCreate(content::RenderFrame* render_frame) {
  if (render_frame->IsMainFrame()) {
    new PageAgent(render_frame);
  }
}

PageAgent::PageAgent(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {
  render_frame->GetAssociatedInterfaceRegistry()
      ->AddInterface<mojom::PageAgent>(base::BindRepeating(
          &PageAgent::BindReceiver, base::Unretained(this)));
}

PageAgent::~PageAgent() = default;

void PageAgent::OnDestruct() {
  delete this;
}

void PageAgent::DidCommitProvisionalLoad(ui::PageTransition transition) {
  clickables_.clear();
}

void PageAgent::BindReceiver(
    mojo::PendingAssociatedReceiver<mojom::PageAgent> receiver) {
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void PageAgent::GetHtml(uint32_t max_bytes, GetHtmlCallback callback) {
  blink::WebDocument document = render_frame()->GetWebFrame()->GetDocument();
  std::string html;
  if (!document.FirstChild().IsNull() &&
      document.FirstChild().IsDocumentTypeNode()) {
    html = "<!DOCTYPE html>\n";
  }
  blink::WebElement root = document.DocumentElement();
  if (!root.IsNull()) {
    std::string tag = Lower(root.TagName());
    html += "<" + tag;
    for (const char* attribute : {"lang", "dir", "class"}) {
      std::string value = Attribute(root, attribute);
      if (!value.empty()) {
        html += std::string(" ") + attribute + "=\"" + EscapeAttribute(value) +
                "\"";
      }
    }
    html += ">";
    html += root.InnerHTML().Utf8();
    html += "</" + tag + ">";
  }
  bool truncated = html.size() > max_bytes;
  if (truncated) {
    html.resize(base::TruncateUTF8ToByteSize(html, max_bytes).size());
  }
  std::move(callback).Run(html, truncated);
}

void PageAgent::GetText(uint32_t max_chars, GetTextCallback callback) {
  std::move(callback).Run(blink::WebFrameContentDumper::DumpFrameTreeAsText(
                              render_frame()->GetWebFrame(), max_chars)
                              .Utf8());
}

void PageAgent::GetClickables(bool include_offscreen,
                              uint32_t max_count,
                              GetClickablesCallback callback) {
  clickables_.clear();
  std::vector<mojom::ClickablePtr> items;
  blink::WebLocalFrame* main_frame = render_frame()->GetWebFrame();
  blink::WebFrameWidget* widget = main_frame->FrameWidget();
  if (!widget) {
    std::move(callback).Run(std::move(items), gfx::Size());
    return;
  }
  gfx::Size viewport =
      widget->BlinkSpaceToEnclosedDIPs(gfx::Rect(widget->Size())).size();

  // Walk the main frame and any same-process (same widget) child frames, in
  // document order.
  std::vector<blink::WebLocalFrame*> frames = {main_frame};
  for (size_t f = 0; f < frames.size(); ++f) {
    for (blink::WebFrame* child = frames[f]->FirstChild(); child;
         child = child->NextSibling()) {
      if (child->IsWebLocalFrame() &&
          child->ToWebLocalFrame()->LocalRoot() == main_frame) {
        frames.push_back(child->ToWebLocalFrame());
      }
    }
  }

  for (blink::WebLocalFrame* frame : frames) {
    blink::WebDocument document = frame->GetDocument();
    blink::WebElementCollection all = document.All();
    if (all.IsNull()) {
      continue;
    }
    for (blink::WebElement element = all.NextItem(); !element.IsNull();
         element = all.NextItem()) {
      if (items.size() >= max_count) {
        break;
      }
      std::string tag = Lower(element.TagName());
      if (!IsInteractive(element, tag)) {
        continue;
      }
      gfx::Rect blink_bounds = element.BoundsInWidget();
      if (blink_bounds.IsEmpty() || IsHiddenByStyle(element)) {
        continue;
      }
      bool in_viewport = !element.VisibleBoundsInWidget().IsEmpty();
      if (!in_viewport && !include_offscreen) {
        continue;
      }

      auto item = mojom::Clickable::New();
      item->id = static_cast<int32_t>(clickables_.size());
      item->tag = tag;
      item->role = Attribute(element, "role");
      item->text = LabelFor(element, tag);
      std::string href = Attribute(element, "href");
      if (!href.empty()) {
        item->href = GURL(document.CompleteURL(blink::WebString::FromUtf8(href)))
                         .possibly_invalid_spec();
      }
      item->type = Attribute(element, "type");
      item->dom_id = element.GetIdAttribute().Utf8();
      item->bounds = widget->BlinkSpaceToEnclosedDIPs(blink_bounds);
      item->in_viewport = in_viewport;
      items.push_back(std::move(item));
      clickables_.push_back(element);
    }
  }
  std::move(callback).Run(std::move(items), viewport);
}

void PageAgent::GetClickableBounds(int32_t id,
                                   GetClickableBoundsCallback callback) {
  blink::WebFrameWidget* widget = render_frame()->GetWebFrame()->FrameWidget();
  if (id < 0 || static_cast<size_t>(id) >= clickables_.size() || !widget) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  const blink::WebElement& element = clickables_[id];
  if (element.IsNull() || !element.IsConnected()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  gfx::Rect bounds = element.BoundsInWidget();
  if (bounds.IsEmpty()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::move(callback).Run(widget->BlinkSpaceToEnclosedDIPs(bounds));
}

}  // namespace chromeaf
