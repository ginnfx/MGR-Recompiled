// Tiny anti-aliased software canvas for the Options diagrams. Shapes are signed
// distance functions, so edges get analytic coverage without supersampling.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mgr_pictures {

struct Color { float r, g, b, a; };
struct Point { float x, y; };

class Canvas {
 public:
  Canvas(int width, int height) : width_(width), height_(height), px_(size_t(width) * height, Color{0, 0, 0, 0}) {}

  int Width() const { return width_; }
  int Height() const { return height_; }

  // Straight-alpha "over" blend of `c` weighted by `cover` (0..1).
  void Blend(int x, int y, Color c, float cover) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
    const float a = c.a * std::clamp(cover, 0.0f, 1.0f);
    if (a <= 0) return;
    Color& d = px_[size_t(y) * width_ + x];
    const float out_a = a + d.a * (1 - a);
    const auto mix = [&](float s, float t) { return (s * a + t * d.a * (1 - a)) / out_a; };
    d = {mix(c.r, d.r), mix(c.g, d.g), mix(c.b, d.b), out_a};
  }

  // Fills every pixel whose signed distance (negative inside) is below zero.
  template <typename Sdf> void Shape(float x0, float y0, float x1, float y1, Color c, Sdf sdf) {
    for (int y = std::max(0, int(y0) - 1); y <= std::min(height_ - 1, int(y1) + 1); ++y)
      for (int x = std::max(0, int(x0) - 1); x <= std::min(width_ - 1, int(x1) + 1); ++x)
        Blend(x, y, c, 0.5f - sdf(float(x) + 0.5f, float(y) + 0.5f));
  }

  void RoundRect(float x, float y, float w, float h, float radius, Color c) {
    const float cx = x + w / 2, cy = y + h / 2;
    Shape(x, y, x + w, y + h, c, [=](float px, float py) {
      const float qx = std::fabs(px - cx) - (w / 2 - radius), qy = std::fabs(py - cy) - (h / 2 - radius);
      return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
    });
  }
  void Rect(float x, float y, float w, float h, Color c) { RoundRect(x, y, w, h, 0, c); }

  void Frame(float x, float y, float w, float h, float thickness, Color c) {
    const float cx = x + w / 2, cy = y + h / 2;
    Shape(x, y, x + w, y + h, c, [=](float px, float py) {
      const float qx = std::fabs(px - cx) - w / 2, qy = std::fabs(py - cy) - h / 2;
      const float outer = std::max(qx, qy);
      return std::max(outer, -outer - thickness);
    });
  }

  void Disc(float cx, float cy, float r, Color c) {
    Shape(cx - r, cy - r, cx + r, cy + r, c, [=](float px, float py) { return std::hypot(px - cx, py - cy) - r; });
  }
  void Ring(float cx, float cy, float r, float thickness, Color c) {
    Shape(cx - r - thickness, cy - r - thickness, cx + r + thickness, cy + r + thickness, c,
          [=](float px, float py) { return std::fabs(std::hypot(px - cx, py - cy) - r) - thickness / 2; });
  }

  void Line(Point a, Point b, float thickness, Color c) {
    Shape(std::min(a.x, b.x) - thickness, std::min(a.y, b.y) - thickness, std::max(a.x, b.x) + thickness,
          std::max(a.y, b.y) + thickness, c, [=](float px, float py) {
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float t = std::clamp(((px - a.x) * dx + (py - a.y) * dy) / std::max(dx * dx + dy * dy, 1e-6f), 0.0f, 1.0f);
            return std::hypot(px - (a.x + t * dx), py - (a.y + t * dy)) - thickness / 2;
          });
  }

  // Filled convex-or-simple polygon, 4x4 supersampled.
  void Polygon(const std::vector<Point>& pts, Color c) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const Point& p : pts) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
    for (int y = std::max(0, int(y0)); y <= std::min(height_ - 1, int(y1)); ++y)
      for (int x = std::max(0, int(x0)); x <= std::min(width_ - 1, int(x1)); ++x) {
        int inside = 0;
        for (int sy = 0; sy < 4; ++sy)
          for (int sx = 0; sx < 4; ++sx) inside += Contains(pts, x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
        Blend(x, y, c, inside / 16.0f);
      }
  }

  // Regular polygon with `sides` corners (a circle for large counts).
  void RegularPolygon(float cx, float cy, float r, int sides, float rotation, Color c) {
    std::vector<Point> pts;
    for (int i = 0; i < sides; ++i) {
      const float a = rotation + 6.2831853f * float(i) / float(sides);
      pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    Polygon(pts, c);
  }


  // "Over" composites `src` with its top-left corner at (x, y).
  void Composite(const Canvas& src, int x, int y) {
    for (int sy = 0; sy < src.height_; ++sy)
      for (int sx = 0; sx < src.width_; ++sx) {
        const Color& c = src.px_[size_t(sy) * src.width_ + sx];
        Blend(x + sx, y + sy, c, 1.0f);
      }
  }

  // Area-averaged copy at a new size (premultiplied, so soft edges stay clean).
  Canvas Scaled(int width, int height) const {
    Canvas out(width, height);
    constexpr int kTaps = 3;
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        float r = 0, g = 0, b = 0, a = 0;
        for (int ty = 0; ty < kTaps; ++ty)
          for (int tx = 0; tx < kTaps; ++tx) {
            const int sx = std::min(width_ - 1, int((float(x) + (tx + 0.5f) / kTaps) * float(width_) / float(width)));
            const int sy = std::min(height_ - 1, int((float(y) + (ty + 0.5f) / kTaps) * float(height_) / float(height)));
            const Color& c = px_[size_t(sy) * width_ + sx];
            r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
          }
        const float n = float(kTaps * kTaps);
        out.px_[size_t(y) * width + x] = a > 0 ? Color{r / a, g / a, b / a, a / n} : Color{0, 0, 0, 0};
      }
    return out;
  }

  // Writes the canvas into the top-left corner of a larger RGBA8 image.
  std::vector<uint8_t> ToRgba8(uint32_t target_width, uint32_t target_height) const {
    std::vector<uint8_t> out(size_t(target_width) * target_height * 4, 0);
    for (int y = 0; y < std::min<int>(height_, int(target_height)); ++y)
      for (int x = 0; x < std::min<int>(width_, int(target_width)); ++x) {
        const Color& c = px_[size_t(y) * width_ + x];
        uint8_t* o = &out[(size_t(y) * target_width + x) * 4];
        const auto q = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        o[0] = q(c.r); o[1] = q(c.g); o[2] = q(c.b); o[3] = q(c.a);
      }
    return out;
  }

 private:
  static bool Contains(const std::vector<Point>& pts, float x, float y) {
    bool inside = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
      if ((pts[i].y > y) != (pts[j].y > y) &&
          x < (pts[j].x - pts[i].x) * (y - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x)
        inside = !inside;
    return inside;
  }

  int width_, height_;
  std::vector<Color> px_;
};

}  // namespace mgr_pictures
