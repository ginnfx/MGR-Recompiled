#include "mgr_icon_render.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr double kMinHorizontalScale = 0.65;  // narrower than this reads badly
constexpr double kCornerRatio = 0.3;
constexpr double kTallRatio = 1.3;      // height/width above which labels may wrap
constexpr size_t kWrapCodePoints = 2;   // single glyphs stay on one row
constexpr double kRowSizeRatio = 0.6;   // starting font size of a wrapped row
constexpr double kRoundTextInset = 0.18;  // ~ (1 - 1/sqrt(2)) / 2 + margin
// Round caps hold up to two glyphs; longer labels get a capsule inside the same
// circle (its farthest point is 0.47 of the diameter from the centre).
constexpr size_t kRoundCodePoints = 2;
constexpr double kCapsuleWidth = 0.94, kCapsuleHeight = 0.62;  // of the diameter
constexpr double kCapsuleTextPadX = 0.2, kCapsuleTextPadY = 0.1;  // of its height

template <typename T>
struct CfRelease {
  void operator()(T ref) const {
    if (ref) CFRelease(ref);
  }
};
template <typename T>
using CfPtr = std::unique_ptr<std::remove_pointer_t<T>, CfRelease<T>>;

struct Layout {
  double inset;       // shape inset from the rect edge
  double text_pad;    // text padding inside the shape
  double text_ratio;  // initial font size relative to the shape height
};
Layout StyleLayout(IconStyle style) {
  switch (style) {
    case IconStyle::kOutline: return {1.0, 3.0, 0.62};
    case IconStyle::kFilled: return {1.0, 3.0, 0.62};
    case IconStyle::kAction: return {4.0, 5.0, 0.6};
  }
  return {1.0, 3.0, 0.6};
}

CfPtr<CTLineRef> MakeLine(const std::string& label, double size) {
  CfPtr<CTFontRef> font(CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, size, nullptr));
  CfPtr<CFStringRef> text(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(label.data()),
                                                  CFIndex(label.size()), kCFStringEncodingUTF8, false));
  if (!font || !text) return nullptr;
  const void* keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
  const void* values[] = {font.get(), kCFBooleanTrue};
  CfPtr<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr, keys, values, 2,
                                                       &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks));
  CfPtr<CFAttributedStringRef> string(CFAttributedStringCreate(nullptr, text.get(), attributes.get()));
  return CfPtr<CTLineRef>(CTLineCreateWithAttributedString(string.get()));
}

enum class Cap { kRounded, kCircle, kCapsule };

void AddCap(CGContextRef ctx, CGRect r, Cap cap) {
  if (cap == Cap::kCircle) {
    CGContextAddEllipseInRect(ctx, r);
    return;
  }
  const double radius = std::min(r.size.width, r.size.height) * (cap == Cap::kCapsule ? 0.5 : kCornerRatio);
  CfPtr<CGPathRef> path(CGPathCreateWithRoundedRect(r, radius, radius, nullptr));
  CGContextAddPath(ctx, path.get());
}

// The largest centered square: round caps must stay circular in wide cells.
CGRect CenteredSquare(CGRect r) {
  const double side = std::min(r.size.width, r.size.height);
  return CGRectMake(CGRectGetMidX(r) - side / 2, CGRectGetMidY(r) - side / 2, side, side);
}

// Fits the label inside 'box' (narrowing, then shrinking) and draws it centered.
void DrawLine(CGContextRef ctx, const std::string& label, CGRect box, double size) {
  for (; size >= 5; size -= 1) {
    CfPtr<CTLineRef> line = MakeLine(label, size);
    if (!line) return;
    // Text position is not graphics state; image bounds are relative to it.
    CGContextSetTextPosition(ctx, 0, 0);
    const CGRect bounds = CTLineGetImageBounds(line.get(), ctx);
    if (bounds.size.width <= 0 || bounds.size.height > box.size.height) continue;
    const double scale = std::min(1.0, box.size.width / bounds.size.width);
    if (scale < kMinHorizontalScale && size > 6) continue;
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, CGRectGetMidX(box), CGRectGetMidY(box));
    CGContextScaleCTM(ctx, scale, 1);
    CGContextSetTextPosition(ctx, -CGRectGetMidX(bounds), -CGRectGetMidY(bounds));
    CTLineDraw(line.get(), ctx);
    CGContextRestoreGState(ctx);
    return;
  }
}

// Byte offsets of UTF-8 code point starts.
std::vector<size_t> CodePoints(const std::string& s) {
  std::vector<size_t> starts;
  for (size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) starts.push_back(i);
  }
  return starts;
}

// Tall cells (the D-pad art) read better with long labels split in two rows.
void DrawLabel(CGContextRef ctx, const std::string& label, CGRect box, double size) {
  const std::vector<size_t> starts = CodePoints(label);
  // Words ("SPACE") stay whole; only symbol sequences ("⇧↑↓") wrap.
  const bool word = starts.size() == label.size();
  if (word || box.size.height < box.size.width * kTallRatio || starts.size() < kWrapCodePoints) {
    DrawLine(ctx, label, box, size);
    return;
  }
  // A leading modifier symbol gets its own row: "⇧" over "↑↓".
  const bool modifier = label.starts_with("⇧") || label.starts_with("⌃") || label.starts_with("⌥");
  const size_t split = starts[modifier ? 1 : (starts.size() + 1) / 2];
  const double half = box.size.height / 2;
  size *= kRowSizeRatio;
  // CG's origin is bottom-left: the first row goes in the upper half.
  DrawLine(ctx, label.substr(0, split),
           CGRectMake(box.origin.x, box.origin.y + half, box.size.width, half), size);
  DrawLine(ctx, label.substr(split), CGRectMake(box.origin.x, box.origin.y, box.size.width, half),
           size);
}
}  // namespace

Image RenderKeyIcon(const IconRect& rect, const std::string& label, bool round) {
  Image image{rect.w, rect.h, std::vector<uint8_t>(size_t(rect.w) * rect.h * 4)};
  CfPtr<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
  CfPtr<CGContextRef> ctx(CGBitmapContextCreate(image.rgba.data(), rect.w, rect.h, 8,
                                                size_t(rect.w) * 4, space.get(),
                                                kCGImageAlphaPremultipliedLast));
  if (!ctx) return {};
  CGContextRef c = ctx.get();
  CGContextSetShouldAntialias(c, true);
  CGContextSetRGBFillColor(c, 1, 1, 1, 1);
  CGContextSetRGBStrokeColor(c, 1, 1, 1, 1);

  const Layout layout = StyleLayout(rect.style);
  CGRect shape = CGRectInset(CGRectMake(0, 0, rect.w, rect.h), layout.inset, layout.inset);
  const Cap cap = !round ? Cap::kRounded
                  : CodePoints(label).size() <= kRoundCodePoints ? Cap::kCircle
                                                                 : Cap::kCapsule;
  if (round) shape = CenteredSquare(shape);
  if (cap == Cap::kCapsule)
    shape = CGRectInset(shape, shape.size.width * (1 - kCapsuleWidth) / 2,
                        shape.size.height * (1 - kCapsuleHeight) / 2);
  // A circle's inscribed square holds the glyph.
  const CGRect text_box =
      cap == Cap::kCircle ? CGRectInset(shape, shape.size.width * kRoundTextInset,
                                        shape.size.height * kRoundTextInset)
      : cap == Cap::kCapsule ? CGRectInset(shape, shape.size.height * kCapsuleTextPadX,
                                           shape.size.height * kCapsuleTextPadY)
                             : CGRectInset(shape, layout.text_pad, layout.text_pad * 0.6);
  const double size = shape.size.height * layout.text_ratio;

  if (rect.style == IconStyle::kOutline) {
    CGContextSetLineWidth(c, 2);
    AddCap(c, CGRectInset(shape, 1, 1), cap);
    CGContextStrokePath(c);
    DrawLabel(c, label, text_box, size);
  } else {
    if (rect.style == IconStyle::kAction) {
      // The HUD art has a soft glow around a solid cap.
      CfPtr<CGColorRef> glow(CGColorCreateSRGB(1, 1, 1, 0.8));
      CGContextSetShadowWithColor(c, CGSizeZero, 3, glow.get());
    }
    AddCap(c, shape, cap);
    CGContextFillPath(c);
    CGContextSetShadowWithColor(c, CGSizeZero, 0, nullptr);
    CGContextSetBlendMode(c, kCGBlendModeClear);  // knocked-out text
    DrawLabel(c, label, text_box, size);
  }
  CGContextFlush(c);
  // Keep the context's premultiplied white (color = alpha), like the original
  // art's dark empty texels: the HUD's pulsing highlight adds the icon's color
  // without its alpha, so white empty texels would light up the whole cell.
  return image;
}
