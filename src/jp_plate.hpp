// jp_plate.hpp — synthetic Japanese licence-plate images (scenes with YOLO boxes + labelled crops).
//
// A C++ re-implementation of the idea in "ナンバープレートの合成画像で物体検出モデルを作る"
// (DevelopersIO, https://dev.classmethod.jp/articles/detect_number_plate/): draw a 440x220 plate with
// four text fields, skew it in perspective, paste it on a 1280x960 background without overlap, blur and
// add noise. On top of that it draws every region (all 138, including ご当地), the real class-number and
// hiragana rules per plate type, many faces with per-glyph fallback (bundled free fonts, TRM font JB,
// and the Japanese faces installed on Windows), frames, bolts, embossing, car bodies, sign distractors,
// JPEG artefacts — and writes recogniser crops in the dyama/alpr_jp layout with a full-label TSV, so
// yolo_lpr_cpp can train on them directly (`--alpr <out>/crops --alpr-labels <out>/crops/labels.tsv`).
//
// Header-only. Needs stb_truetype / stb_image / stb_image_write (implementations in main.cpp).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_truetype.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace jp {

// ---- random --------------------------------------------------------------------------------------
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  double unit() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
  double range(double a, double b) { return a + (b - a) * unit(); }
  int below(int n) { return n <= 0 ? 0 : (int)(next() % (uint64_t)n); }
  bool chance(double p) { return unit() < p; }
  double normal() {
    double u1 = std::max(unit(), 1e-12), u2 = unit();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
  template <class T> const T& pick(const std::vector<T>& v) { return v[(size_t)below((int)v.size())]; }
  int weighted(const std::vector<double>& w) {
    double t = 0;
    for (double x : w) t += x;
    double r = unit() * t;
    for (size_t i = 0; i < w.size(); ++i) {
      if (r < w[i]) return (int)i;
      r -= w[i];
    }
    return (int)w.size() - 1;
  }
};

// ---- files (UTF-8 paths, also on Windows) ------------------------------------------------------------
#ifdef _WIN32
inline std::wstring to_w(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w((size_t)std::max(n, 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(wcslen(w.c_str()));
  return w;
}
inline std::string from_w(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s((size_t)std::max(n, 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  s.resize(strlen(s.c_str()));
  return s;
}
inline FILE* uopen(const std::string& p, const char* mode) { return _wfopen(to_w(p).c_str(), to_w(mode).c_str()); }
#else
inline FILE* uopen(const std::string& p, const char* mode) { return fopen(p.c_str(), mode); }
#endif

inline std::vector<unsigned char> read_file(const std::string& path) {
  std::vector<unsigned char> b;
  FILE* f = uopen(path, "rb");
  if (!f) return b;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0) {
    b.resize((size_t)n);
    if (fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
  }
  fclose(f);
  return b;
}

inline bool write_file(const std::string& path, const void* data, size_t n) {
  FILE* f = uopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  fclose(f);
  return ok;
}

inline void make_dirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/' || path[i] == '\\') {
      if (!cur.empty() && cur.back() != ':') {
#ifdef _WIN32
        _wmkdir(to_w(cur).c_str());
#else
        mkdir(cur.c_str(), 0755);
#endif
      }
    }
    if (i < path.size()) cur += path[i];
  }
}

inline std::vector<std::string> list_files(const std::string& dir) {
  std::vector<std::string> out;
#ifdef _WIN32
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(to_w(dir + "/*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(from_w(fd.cFileName));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d))
    if (e->d_type != DT_DIR) out.push_back(e->d_name);
  closedir(d);
#endif
  std::sort(out.begin(), out.end());
  return out;
}

inline std::string lower(std::string s) {
  for (char& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

inline std::vector<int> utf8(const std::string& s) {
  std::vector<int> cps;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    int cp = c, len = 1;
    if (c >= 0xF0) { cp = c & 0x07; len = 4; }
    else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
    else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
    for (int k = 1; k < len && i + (size_t)k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + (size_t)k] & 0x3F);
    cps.push_back(cp);
    i += (size_t)len;
  }
  return cps;
}

inline std::string to_utf8(int cp) {
  std::string o;
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
    o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
  } else {
    o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
    o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
  }
  return o;
}

// ---- images ----------------------------------------------------------------------------------------
struct Rgb { float r = 0, g = 0, b = 0; };

struct Img {                                  // float pixels 0..255, interleaved, c = 1 or 3
  int w = 0, h = 0, c = 3;
  std::vector<float> px;
  Img() {}
  Img(int W, int H, int C = 3, float v = 0.f) : w(W), h(H), c(C), px((size_t)W * H * C, v) {}
  float* at(int x, int y) { return &px[((size_t)y * w + x) * c]; }
  const float* at(int x, int y) const { return &px[((size_t)y * w + x) * c]; }
  bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
  void blend(int x, int y, Rgb col, float a) {
    if (!in(x, y) || a <= 0.f) return;
    a = std::min(a, 1.f);
    float* p = at(x, y);
    p[0] += (col.r - p[0]) * a; p[1] += (col.g - p[1]) * a; p[2] += (col.b - p[2]) * a;
  }
  // bilinear sample (clamped); out has c values
  void sample(float x, float y, float* out) const {
    x -= 0.5f; y -= 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    auto P = [&](int xx, int yy) {
      xx = std::clamp(xx, 0, w - 1); yy = std::clamp(yy, 0, h - 1);
      return at(xx, yy);
    };
    const float *a = P(x0, y0), *b = P(x0 + 1, y0), *cc = P(x0, y0 + 1), *d = P(x0 + 1, y0 + 1);
    for (int k = 0; k < c; ++k)
      out[k] = a[k] * (1 - fx) * (1 - fy) + b[k] * fx * (1 - fy) + cc[k] * (1 - fx) * fy + d[k] * fx * fy;
  }
};

// area-average downscale by an integer factor
inline Img shrink(const Img& s, int f) {
  if (f <= 1) return s;
  Img o(std::max(1, s.w / f), std::max(1, s.h / f), s.c);
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x)
      for (int k = 0; k < s.c; ++k) {
        float acc = 0;
        for (int yy = 0; yy < f; ++yy)
          for (int xx = 0; xx < f; ++xx) acc += s.at(std::min(s.w - 1, x * f + xx), std::min(s.h - 1, y * f + yy))[k];
        o.at(x, y)[k] = acc / (float)(f * f);
      }
  return o;
}

inline Img resize(const Img& s, int W, int H) {
  // shrink first so bilinear never skips source pixels
  int f = std::max(1, std::min(s.w / std::max(1, W), s.h / std::max(1, H)));
  Img src = f > 1 ? shrink(s, f) : s;
  Img o(W, H, s.c);
  float sx = (float)src.w / W, sy = (float)src.h / H;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) src.sample((x + 0.5f) * sx, (y + 0.5f) * sy, o.at(x, y));
  return o;
}

inline void box_blur(Img& im, int r) {
  if (r <= 0) return;
  Img t = im;
  for (int y = 0; y < im.h; ++y)                // horizontal
    for (int x = 0; x < im.w; ++x)
      for (int k = 0; k < im.c; ++k) {
        float a = 0; int n = 0;
        for (int d = -r; d <= r; ++d) { int xx = x + d; if (xx < 0 || xx >= im.w) continue; a += im.at(xx, y)[k]; ++n; }
        t.at(x, y)[k] = a / n;
      }
  for (int y = 0; y < im.h; ++y)                // vertical
    for (int x = 0; x < im.w; ++x)
      for (int k = 0; k < im.c; ++k) {
        float a = 0; int n = 0;
        for (int d = -r; d <= r; ++d) { int yy = y + d; if (yy < 0 || yy >= im.h) continue; a += t.at(x, yy)[k]; ++n; }
        im.at(x, y)[k] = a / n;
      }
}

inline void gaussian_blur(Img& im, double sigma) {
  if (sigma < 0.3) return;
  int r = (int)std::ceil(sigma * 2.5);
  std::vector<float> k((size_t)(2 * r + 1));
  float s = 0;
  for (int i = -r; i <= r; ++i) s += k[(size_t)(i + r)] = (float)std::exp(-(i * i) / (2 * sigma * sigma));
  for (float& v : k) v /= s;
  Img t = im;
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x)
      for (int c = 0; c < im.c; ++c) {
        float a = 0;
        for (int i = -r; i <= r; ++i) a += k[(size_t)(i + r)] * im.at(std::clamp(x + i, 0, im.w - 1), y)[c];
        t.at(x, y)[c] = a;
      }
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x)
      for (int c = 0; c < im.c; ++c) {
        float a = 0;
        for (int i = -r; i <= r; ++i) a += k[(size_t)(i + r)] * t.at(x, std::clamp(y + i, 0, im.h - 1))[c];
        im.at(x, y)[c] = a;
      }
}

inline void motion_blur(Img& im, int len, double ang) {
  if (len < 2) return;
  Img t = im;
  float dx = (float)std::cos(ang), dy = (float)std::sin(ang);
  std::vector<float> v((size_t)im.c);
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x) {
      float acc[3] = {0, 0, 0};
      for (int i = 0; i < len; ++i) {
        float o = i - (len - 1) * 0.5f;
        im.sample(x + 0.5f + o * dx, y + 0.5f + o * dy, v.data());
        for (int c = 0; c < im.c; ++c) acc[c] += v[(size_t)c];
      }
      for (int c = 0; c < im.c; ++c) t.at(x, y)[c] = acc[c] / len;
    }
  im = t;
}

inline std::vector<unsigned char> to_u8(const Img& im) {
  std::vector<unsigned char> o(im.px.size());
  for (size_t i = 0; i < o.size(); ++i) o[i] = (unsigned char)std::clamp((int)std::lround(im.px[i]), 0, 255);
  return o;
}

inline Img from_u8(const unsigned char* p, int w, int h) {
  Img im(w, h, 3);
  for (size_t i = 0; i < im.px.size(); ++i) im.px[i] = p[i];
  return im;
}

inline void sink(void* ctx, void* data, int size) {
  auto* v = (std::vector<unsigned char>*)ctx;
  v->insert(v->end(), (unsigned char*)data, (unsigned char*)data + size);
}

inline bool save_image(const std::string& path, const Img& im, int jpg_quality) {
  std::vector<unsigned char> u = to_u8(im), out;
  if (jpg_quality > 0) stbi_write_jpg_to_func(sink, &out, im.w, im.h, im.c, u.data(), jpg_quality);
  else stbi_write_png_to_func(sink, &out, im.w, im.h, im.c, u.data(), im.w * im.c);
  return !out.empty() && write_file(path, out.data(), out.size());
}

// JPEG round trip in memory: real crops carry block artefacts, so the training pixels should too
inline void jpeg_roundtrip(Img& im, int q) {
  std::vector<unsigned char> u = to_u8(im), out;
  stbi_write_jpg_to_func(sink, &out, im.w, im.h, 3, u.data(), q);
  int w, h, c;
  unsigned char* p = stbi_load_from_memory(out.data(), (int)out.size(), &w, &h, &c, 3);
  if (!p) return;
  im = from_u8(p, w, h);
  stbi_image_free(p);
}

// ---- shapes (into a 1-channel coverage mask or straight onto RGB) ------------------------------------
inline float rrect_cov(float x, float y, float x0, float y0, float x1, float y1, float r) {
  // signed-distance coverage of a rounded rectangle, 1px antialiasing
  float cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
  float dx = x - cx, dy = y - cy;
  float d = (x < x0 + r || x > x1 - r) && (y < y0 + r || y > y1 - r) ? std::sqrt(dx * dx + dy * dy) - r
                                                                       : std::max(std::max(x0 - x, x - x1), std::max(y0 - y, y - y1));
  return std::clamp(0.5f - d, 0.f, 1.f);
}

inline void fill_rrect(Img& im, float x0, float y0, float x1, float y1, float r, Rgb col, float alpha = 1.f) {
  for (int y = std::max(0, (int)y0 - 1); y <= std::min(im.h - 1, (int)y1 + 1); ++y)
    for (int x = std::max(0, (int)x0 - 1); x <= std::min(im.w - 1, (int)x1 + 1); ++x) {
      float a = rrect_cov(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r);
      if (a > 0) {
        if (im.c == 1) { float* p = im.at(x, y); p[0] = std::max(p[0], a * alpha); }
        else im.blend(x, y, col, a * alpha);
      }
    }
}

inline void fill_circle(Img& im, float cx, float cy, float r, Rgb col, float alpha = 1.f) {
  for (int y = std::max(0, (int)(cy - r) - 1); y <= std::min(im.h - 1, (int)(cy + r) + 1); ++y)
    for (int x = std::max(0, (int)(cx - r) - 1); x <= std::min(im.w - 1, (int)(cx + r) + 1); ++x) {
      float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
      float a = std::clamp(r - d + 0.5f, 0.f, 1.f);
      if (a > 0) {
        if (im.c == 1) { float* p = im.at(x, y); p[0] = std::max(p[0], a * alpha); }
        else im.blend(x, y, col, a * alpha);
      }
    }
}

// ---- fonts -----------------------------------------------------------------------------------------
enum Role { REGION = 0, CLASSNUM = 1, HIRA = 2, SERIAL = 3 };

struct Font {
  std::string name, path;
  bool system = false;                        // read from the OS font folder, never redistributed
  bool trm = false;                           // TRM font JB: model-making font with its own key map
  std::vector<unsigned char> blob;
  stbtt_fontinfo info{};
  bool ok = false;
  // "has" means the glyph has outlines: TRM maps ~15000 code points but only 51 carry a shape
  bool has(int cp) const {
    int g = stbtt_FindGlyphIndex(&info, cp);
    return g != 0 && !stbtt_IsGlyphEmpty(&info, g);
  }
};

inline bool load_font(const std::string& path, Font& f) {
  f.blob = read_file(path);
  if (f.blob.empty()) return false;
  int off = stbtt_GetFontOffsetForIndex(f.blob.data(), 0);
  if (off < 0 || !stbtt_InitFont(&f.info, f.blob.data(), off)) return false;
  f.path = path;
  f.name = path.substr(path.find_last_of("/\\") + 1);
  f.trm = lower(f.name).find("trmfont") != std::string::npos;
  f.ok = true;
  return true;
}

// The code points to type for `text` in face `f`. TRM font JB (JB1.2-key.txt): class numbers are
// full-width digits (only 0 1 2 3 8 exist), なにわ is typed in katakana, 尾張小牧 = full-width Ｏ and
// 和泉 = full-width Ｉ (one combined glyph each). Every other face takes the text as it is.
inline std::vector<int> encode(const Font& f, Role role, const std::string& text) {
  std::vector<int> cps = utf8(text);
  if (!f.trm) return cps;
  if (role == REGION) {
    if (text == "尾張小牧") return {0xFF2F};
    if (text == "和泉") return {0xFF29};
    for (int& c : cps)
      if (c >= 0x3041 && c <= 0x3096) c += 0x60;      // hiragana -> katakana
  }
  if (role == CLASSNUM)
    for (int& c : cps)
      if (c >= '0' && c <= '9') c = 0xFF10 + (c - '0');
  return cps;
}

struct FontSet {
  std::vector<Font> fonts;

  bool usable(const Font& f, Role role, const std::string& text) const {
    for (int cp : encode(f, role, text)) if (cp != 0x30FB && !f.has(cp)) return false;   // ・ is drawn
    return true;
  }
  // Pick one face for a whole field so a field never mixes typefaces. `trm_p`: chance of TRM when
  // it can draw the field. -1 = nothing can draw all of it (the caller falls back per glyph).
  int pick(Role role, const std::string& text, Rng& rng, double trm_p) const {
    std::vector<int> any, trm;
    for (size_t i = 0; i < fonts.size(); ++i) {
      if (!usable(fonts[i], role, text)) continue;
      (fonts[i].trm ? trm : any).push_back((int)i);
    }
    if (!trm.empty() && (any.empty() || rng.chance(trm_p))) return rng.pick(trm);
    if (!any.empty()) return rng.pick(any);
    return -1;
  }
  const Font* fallback(int cp) const {
    for (const Font& f : fonts) if (!f.trm && f.has(cp)) return &f;
    return nullptr;
  }
};

// ---- plate content ---------------------------------------------------------------------------------
// Category = plate type, also the YOLO class with --classes category (the article's four classes).
enum Category { PRIVATE = 0, COMMERCIAL = 1, KEI_PRIVATE = 2, KEI_COMMERCIAL = 3 };
static const char* kCategoryDir[4] = {"自家用", "事業用", "自家用(軽)", "事業用(軽)"};
static const char* kCategoryName[4] = {"private", "commercial", "kei_private", "kei_commercial"};

struct Region { std::string name; bool gotochi = false; };

inline std::vector<Region> load_regions(const std::string& path) {
  std::vector<Region> out;
  std::vector<unsigned char> b = read_file(path);
  std::string s(b.begin(), b.end()), line;
  size_t p = 0;
  while (p <= s.size()) {
    size_t e = s.find('\n', p);
    line = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
    p = e == std::string::npos ? s.size() + 1 : e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    Region r;
    size_t t = line.find('\t');
    r.name = line.substr(0, t);
    r.gotochi = t != std::string::npos && line.find("ご当地", t) != std::string::npos;
    out.push_back(r);
  }
  return out;
}

struct PlateText {
  int category = PRIVATE;
  std::string region, cls, hira;
  std::string serial;                         // 4 chars, '.' = blank slot (drawn as ・)
  bool design = false;                        // 図柄入り (pictorial background)
};

struct ContentOpts {
  std::vector<double> category_w = {0.40, 0.15, 0.30, 0.15};
  double gotochi_p = -1;                      // <0: every region equally likely
  double rental_p = 0.05;                     // わ / れ on private plates
  double letter_p = 0.12;                     // alphabet in class number positions 2-3 (since 2017)
  double two_digit_p = 0.10;                  // old 2-digit class numbers
  double short_serial_p = 0.40;               // 1-3 digit serials (・・12)
  double design_p = 0.06;
};

inline PlateText sample_text(Rng& rng, const std::vector<Region>& regions, const ContentOpts& o) {
  PlateText t;
  t.category = rng.weighted(o.category_w);
  if (o.gotochi_p >= 0) {
    bool g = rng.chance(o.gotochi_p);
    std::vector<int> idx;
    for (size_t i = 0; i < regions.size(); ++i) if (regions[i].gotochi == g) idx.push_back((int)i);
    t.region = regions[(size_t)(idx.empty() ? rng.below((int)regions.size()) : rng.pick(idx))].name;
  } else {
    t.region = regions[(size_t)rng.below((int)regions.size())].name;
  }
  // class number: the first digit follows the vehicle kind of each plate type
  static const std::vector<std::vector<int>> first = {
      {3, 3, 3, 5, 5, 5, 7, 1, 4, 4, 6, 8, 8},   // 自家用: 普通 3, 小型 5/7, 貨物 1/4/6, 特種 8
      {1, 1, 2, 3, 3, 4, 5, 5, 8},               // 事業用: + バス 2
      {5, 5, 5, 5, 4, 4, 4, 8},                  // 軽自家用: 乗用 5, 貨物 4, 特種 8
      {4, 4, 4, 4, 5, 8}};                       // 軽事業用
  static const char* letters = "ACFHKLMPXY";
  t.cls = std::to_string(rng.pick(first[(size_t)t.category]));
  int n = rng.chance(o.two_digit_p) ? 2 : 3;
  for (int i = 1; i < n; ++i) {
    if (n == 3 && rng.chance(o.letter_p)) t.cls += letters[rng.below(10)];
    else t.cls += (char)('0' + rng.below(10));
  }
  // hiragana: 事業用 uses あ-こ/を, 自家用 さ-ろ (no し へ ん), レンタカー わ/れ; 軽自家用 uses both
  static const std::vector<std::string> priv = {"さ", "す", "せ", "そ", "た", "ち", "つ", "て", "と", "な", "に",
                                                "ぬ", "ね", "の", "は", "ひ", "ふ", "ほ", "ま", "み", "む", "め",
                                                "も", "や", "ゆ", "よ", "ら", "り", "る", "ろ"};
  static const std::vector<std::string> comm = {"あ", "い", "う", "え", "か", "き", "く", "け", "こ", "を"};
  static const std::vector<std::string> rent = {"わ", "れ"};
  if (t.category == COMMERCIAL || t.category == KEI_COMMERCIAL) t.hira = rng.pick(comm);
  else if (rng.chance(o.rental_p)) t.hira = rng.pick(rent);
  else if (t.category == KEI_PRIVATE && rng.chance(0.3)) t.hira = rng.pick(comm);
  else t.hira = rng.pick(priv);
  // serial: 1-9999; blank leading slots are ・, the hyphen only appears with 4 digits
  int v = rng.chance(o.short_serial_p) ? 1 + rng.below(999) : 1000 + rng.below(9000);
  std::string d = std::to_string(v);
  t.serial = std::string(4 - d.size(), '.') + d;
  t.design = t.category != COMMERCIAL && t.category != KEI_COMMERCIAL && rng.chance(o.design_p);
  return t;
}

// ---- drawing text into a box -----------------------------------------------------------------------
struct Box { float x0, y0, x1, y1; float w() const { return x1 - x0; } float h() const { return y1 - y0; } };

struct Glyph { const Font* f; int cp; };

// Rasterise `gs` into the coverage mask so that their joint ink height fills box.h. cells > 0: equal
// cells (the serial); otherwise packed with `track` spacing and squeezed horizontally to fit box.w,
// as real plates squeeze 3-4 character region names.
inline void draw_run(Img& mask, const std::vector<Glyph>& gs, Box b, float track, int cells, float squeeze) {
  if (gs.empty()) return;
  struct M { float x0, y0, x1, y1, s; };       // ink box in em units (y up) and units->em scale
  std::vector<M> m(gs.size());
  float top = -1e9f, bot = 1e9f;
  for (size_t i = 0; i < gs.size(); ++i) {
    const Font& f = *gs[i].f;
    float s = stbtt_ScaleForMappingEmToPixels(&f.info, 1.f);
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(&f.info, gs[i].cp, &x0, &y0, &x1, &y1)) { x0 = y0 = 0; x1 = y1 = 1; }
    m[i] = {x0 * s, y0 * s, x1 * s, y1 * s, s};
    top = std::max(top, m[i].y1);
    bot = std::min(bot, m[i].y0);
  }
  float k = b.h() / std::max(1e-3f, top - bot);                 // pixels per em
  std::vector<float> w(gs.size());
  float total = 0;
  for (size_t i = 0; i < gs.size(); ++i) total += (w[i] = (m[i].x1 - m[i].x0) * k);
  float sx = squeeze, left = 0, cellw = 0;
  if (cells > 0) {
    cellw = b.w() / cells;
    float widest = *std::max_element(w.begin(), w.end());
    sx = std::min(squeeze, cellw * 0.9f / std::max(1.f, widest));
  } else {
    float gap = track * b.h();
    float tw = total * sx + gap * (gs.size() - 1);
    if (tw > b.w()) { sx = std::max(0.2f, (b.w() - gap * (gs.size() - 1)) / std::max(1.f, total)); tw = b.w(); }
    left = b.x0 + (b.w() - tw) * 0.5f;
  }
  float baseline = b.y0 + top * k;
  float x = left;
  for (size_t i = 0; i < gs.size(); ++i) {
    const Font& f = *gs[i].f;
    float ppx = k * sx * m[i].s, ppy = k * m[i].s;                 // pixels per font unit
    float gx = cells > 0 ? b.x0 + cellw * ((float)i + 0.5f) - w[i] * sx * 0.5f : x;
    float ox = gx - m[i].x0 * k * sx;                              // origin so the ink starts at gx
    int ix = (int)std::floor(ox), iy = (int)std::floor(baseline);
    float fx = ox - ix, fy = baseline - iy;
    int bw, bh, xo, yo;
    unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(&f.info, ppx, ppy, fx, fy, gs[i].cp, &bw, &bh, &xo, &yo);
    if (bmp) {
      for (int yy = 0; yy < bh; ++yy)
        for (int xx = 0; xx < bw; ++xx) {
          int X = ix + xo + xx, Y = iy + yo + yy;
          if (!mask.in(X, Y)) continue;
          float* p = mask.at(X, Y);
          p[0] = std::max(p[0], bmp[yy * bw + xx] / 255.f);
        }
      stbtt_FreeBitmap(bmp, nullptr);
    }
    x += w[i] * sx + track * b.h();
  }
}

inline std::vector<Glyph> glyphs_for(const FontSet& fs, int face, Role role, const std::string& text) {
  std::vector<Glyph> out;
  if (face >= 0) {
    for (int cp : encode(fs.fonts[(size_t)face], role, text)) out.push_back({&fs.fonts[(size_t)face], cp});
    return out;
  }
  for (int cp : utf8(text)) {                 // nothing draws the whole field: borrow per glyph
    const Font* f = fs.fallback(cp);
    if (f) out.push_back({f, cp});
  }
  return out;
}

inline void dilate(Img& mask, float amount) {
  if (amount <= 0) return;
  Img t = mask;
  for (int y = 0; y < mask.h; ++y)
    for (int x = 0; x < mask.w; ++x) {
      float v = t.at(x, y)[0];
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
          if (t.in(x + dx, y + dy)) v = std::max(v, t.at(x + dx, y + dy)[0] * amount);
      mask.at(x, y)[0] = v;
    }
}

// ---- the plate texture -----------------------------------------------------------------------------
struct PlateOpts {
  float scale = 1.5f;                         // texture pixels per article unit (plate = 440 x 220 units)
  double trm_p = 0.35;                        // TRM font for a field it can draw
  double frame_p = 0.35;
  double emboss = 0.35;
  double bold_p = 0.3;                        // article: stroke_width=1 on the region name
};

struct Texture {
  Img rgb, alpha;                             // alpha = 1-channel coverage
  float px0, py0, px1, py1;                   // the plate itself inside the texture (frame is outside)
  std::string fonts;                          // faces used, for meta.tsv
};

inline Rgb jitter(Rng& rng, Rgb c, float a) {
  return {std::clamp(c.r + (float)rng.range(-a, a), 0.f, 255.f), std::clamp(c.g + (float)rng.range(-a, a), 0.f, 255.f),
          std::clamp(c.b + (float)rng.range(-a, a), 0.f, 255.f)};
}

inline Texture render_plate(const PlateText& t, const FontSet& fs, const PlateOpts& o, Rng& rng) {
  const float S = o.scale;
  float frame = rng.chance(o.frame_p) ? (float)rng.range(5, 14) : 0.f;
  float F = frame * S;
  int W = (int)std::lround((440 + 2 * frame) * S), H = (int)std::lround((220 + 2 * frame) * S);
  Texture tx;
  tx.rgb = Img(W, H, 3, 0.f);
  tx.alpha = Img(W, H, 1, 0.f);
  tx.px0 = F; tx.py0 = F; tx.px1 = W - F; tx.py1 = H - F;
  const float R = (float)rng.range(8, 16) * S;

  // colours (RGB). Article: white (240,240,240), green (0,60,0), yellow, black
  Rgb bg, fg;
  switch (t.category) {
    case PRIVATE: bg = {238, 240, 236}; fg = {12, 78, 42}; break;
    case COMMERCIAL: bg = {14, 88, 50}; fg = {240, 242, 238}; break;
    case KEI_PRIVATE: bg = {244, 214, 40}; fg = {28, 28, 26}; break;
    default: bg = {26, 26, 26}; fg = {240, 204, 24}; break;
  }
  bg = jitter(rng, bg, 14);
  fg = jitter(rng, fg, 14);

  // frame / holder (outside the labelled plate) then the plate body
  if (frame > 0) {
    static const Rgb frames[] = {{20, 20, 22}, {190, 190, 195}, {120, 120, 125}, {200, 170, 60}, {240, 240, 240}};
    fill_rrect(tx.rgb, 0, 0, (float)W, (float)H, R + F * 0.6f, jitter(rng, frames[rng.below(5)], 12));
    fill_rrect(tx.alpha, 0, 0, (float)W, (float)H, R + F * 0.6f, {}, 1.f);
  }
  fill_rrect(tx.rgb, F, F, W - F, H - F, R, {128, 128, 128});                       // 1px grey rim
  fill_rrect(tx.alpha, F, F, W - F, H - F, R, {}, 1.f);
  fill_rrect(tx.rgb, F + S, F + S, W - F - S, H - F - S, R - S, bg);
  if (t.design) {                             // 図柄入り: soft pictorial blobs under a white wash
    for (int k = 0; k < 7; ++k) {
      Rgb c = {(float)rng.range(110, 230), (float)rng.range(140, 230), (float)rng.range(140, 230)};
      fill_circle(tx.rgb, F + (float)rng.range(0.05, 0.95) * 440 * S, F + (float)rng.range(0.1, 0.9) * 220 * S,
                  (float)rng.range(0.05, 0.18) * 440 * S, c, 0.55f);
    }
    fill_rrect(tx.rgb, F + S, F + S, W - F - S, H - F - S, R - S, bg, 0.45f);
  }

  // text fields, article layout (region 100,10 / class 230,10 / hiragana 20,80 / serial 80,80 on
  // 440x220) turned into ink boxes, each jittered a little
  auto box = [&](float x0, float y0, float x1, float y1) {
    float j = 3.f;
    return Box{F + (x0 + (float)rng.range(-j, j)) * S, F + (y0 + (float)rng.range(-j, j)) * S,
               F + (x1 + (float)rng.range(-j, j)) * S, F + (y1 + (float)rng.range(-j, j)) * S};
  };
  Img mask(W, H, 1, 0.f);
  std::string used;
  auto field = [&](Role role, const std::string& text, Box b, float track, int cells) {
    int face = fs.pick(role, text, rng, o.trm_p);
    std::vector<Glyph> g = glyphs_for(fs, face, role, text);
    Img m(W, H, 1, 0.f);
    draw_run(m, g, b, track, cells, (float)rng.range(0.85, 1.0));
    if (role == REGION && rng.chance(o.bold_p)) dilate(m, 0.6f);
    for (size_t i = 0; i < m.px.size(); ++i) mask.px[i] = std::max(mask.px[i], m.px[i]);
    used += (used.empty() ? "" : ",") + (face >= 0 ? fs.fonts[(size_t)face].name : std::string("mixed"));
  };
  int nreg = (int)utf8(t.region).size();
  float rx0 = nreg == 1 ? 125.f : 92.f, rx1 = nreg == 1 ? 190.f : 224.f;
  field(REGION, t.region, box(rx0, 16, rx1, 64), 0.08f, 0);
  float cx0 = t.cls.size() == 2 ? 248.f : 232.f, cx1 = t.cls.size() == 2 ? 330.f : 352.f;
  field(CLASSNUM, t.cls, box(cx0, 14, cx1, 66), 0.10f, 0);
  field(HIRA, t.hira, box(22, 112, 86, 172), 0.f, 0);

  // serial: four equal cells; ・ for blanks, hyphen between cells 2 and 3 only with four digits
  Box sb = box(100, 86, 426, 202);
  std::string digits;
  for (char c : t.serial) if (c != '.') digits += c;
  {
    int face = fs.pick(SERIAL, digits, rng, o.trm_p);
    Img m(W, H, 1, 0.f);
    float cw = sb.w() / 4.f, gap = cw * 0.10f;
    // draw each digit in its cell so blanks keep their slot
    std::vector<Glyph> g = glyphs_for(fs, face, SERIAL, digits);
    float sq = (float)rng.range(0.85, 1.0);
    size_t gi = 0;
    for (int i = 0; i < 4; ++i) {
      float x0 = sb.x0 + cw * i + (i >= 2 ? gap : -gap) * 0.5f;
      Box cb{x0, sb.y0, x0 + cw, sb.y1};
      if (t.serial[(size_t)i] == '.') {
        fill_circle(m, (cb.x0 + cb.x1) * 0.5f, (cb.y0 + cb.y1) * 0.5f, sb.h() * 0.075f, {}, 1.f);
      } else if (gi < g.size()) {
        // a single-glyph run in a 1-cell box, but with the height of the whole digit set
        std::vector<Glyph> one = {g[gi++]};
        draw_run(m, one, cb, 0.f, 1, sq);
      }
    }
    if (t.serial[0] != '.')
      fill_rrect(m, sb.x0 + cw * 2 - cw * 0.16f, (sb.y0 + sb.y1) * 0.5f - sb.h() * 0.04f, sb.x0 + cw * 2 + cw * 0.16f,
                 (sb.y0 + sb.y1) * 0.5f + sb.h() * 0.04f, sb.h() * 0.02f, {}, 1.f);
    for (size_t i = 0; i < m.px.size(); ++i) mask.px[i] = std::max(mask.px[i], m.px[i]);
    used += "," + (face >= 0 ? fs.fonts[(size_t)face].name : std::string("mixed"));
  }

  // raised characters: highlight up-left, shadow down-right, then the ink
  int e = std::max(1, (int)std::lround(2.0 * S));
  float em = (float)o.emboss;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      if (tx.alpha.at(x, y)[0] <= 0) continue;
      float hi = (x + e < W && y + e < H) ? mask.at(x + e, y + e)[0] : 0.f;
      float sh = (x - e >= 0 && y - e >= 0) ? mask.at(x - e, y - e)[0] : 0.f;
      if (hi > 0) tx.rgb.blend(x, y, {255, 255, 255}, hi * em * 0.6f);
      if (sh > 0) tx.rgb.blend(x, y, {0, 0, 0}, sh * em);
      float a = mask.at(x, y)[0];
      if (a > 0) tx.rgb.blend(x, y, fg, a);
    }

  // bolts (article: grey circles r=6 at (80,30) and (360,30))
  if (frame == 0 || rng.chance(0.5)) {
    Rgb bolt = jitter(rng, {150, 150, 155}, 25);
    for (float bx : {80.f, 360.f}) {
      fill_circle(tx.rgb, F + bx * S, F + 30 * S, 7 * S, {90, 90, 95});
      fill_circle(tx.rgb, F + bx * S, F + 30 * S, 5.5f * S, bolt);
    }
  }
  // dirt and uneven light across the plate
  for (int k = rng.below(4); k > 0; --k)
    fill_circle(tx.rgb, (float)rng.range(0, W), (float)rng.range(0, H), (float)rng.range(0.03, 0.15) * W,
                {(float)rng.range(60, 120), (float)rng.range(50, 100), (float)rng.range(30, 70)}, (float)rng.range(0.03, 0.12));
  float g0 = (float)rng.range(0.75, 1.1), g1 = (float)rng.range(0.75, 1.1);
  bool horiz = rng.chance(0.5);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float u = horiz ? (float)x / W : (float)y / H, g = g0 + (g1 - g0) * u;
      float* p = tx.rgb.at(x, y);
      for (int c = 0; c < 3; ++c) p[c] = std::min(255.f, p[c] * g);
    }
  tx.fonts = used;
  return tx;
}

// ---- geometry --------------------------------------------------------------------------------------
struct Pt { double x, y; };
using Quad = std::array<Pt, 4>;               // TL, TR, BR, BL

// homography H (3x3, row-major) with H * src[i] ~ dst[i]
inline std::array<double, 9> homography(const Quad& src, const Quad& dst) {
  double A[8][9];
  for (int i = 0; i < 4; ++i) {
    double x = src[(size_t)i].x, y = src[(size_t)i].y, u = dst[(size_t)i].x, v = dst[(size_t)i].y;
    double r0[9] = {x, y, 1, 0, 0, 0, -u * x, -u * y, u};
    double r1[9] = {0, 0, 0, x, y, 1, -v * x, -v * y, v};
    std::memcpy(A[2 * i], r0, sizeof r0);
    std::memcpy(A[2 * i + 1], r1, sizeof r1);
  }
  for (int c = 0; c < 8; ++c) {
    int p = c;
    for (int r = c + 1; r < 8; ++r) if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
    for (int k = 0; k < 9; ++k) std::swap(A[c][k], A[p][k]);
    for (int r = 0; r < 8; ++r) {
      if (r == c || A[c][c] == 0) continue;
      double f = A[r][c] / A[c][c];
      for (int k = c; k < 9; ++k) A[r][k] -= f * A[c][k];
    }
  }
  std::array<double, 9> H;
  for (int i = 0; i < 8; ++i) H[(size_t)i] = A[i][8] / A[i][i];
  H[8] = 1;
  return H;
}

inline Pt apply(const std::array<double, 9>& H, double x, double y) {
  double w = H[6] * x + H[7] * y + H[8];
  return {(H[0] * x + H[1] * y + H[2]) / w, (H[3] * x + H[4] * y + H[5]) / w};
}

inline std::array<double, 9> invert(const std::array<double, 9>& m) {
  double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
  double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
  double det = a * A + b * B + c * C;
  std::array<double, 9> r = {A, -(b * i - c * h), b * f - c * e, B, a * i - c * g, -(a * f - c * d), C, -(a * h - b * g), a * e - b * d};
  for (double& v : r) v /= det;
  return r;
}

struct BBox { double x0, y0, x1, y1; };
inline BBox bbox(const Quad& q) {
  BBox b{1e18, 1e18, -1e18, -1e18};
  for (const Pt& p : q) { b.x0 = std::min(b.x0, p.x); b.y0 = std::min(b.y0, p.y); b.x1 = std::max(b.x1, p.x); b.y1 = std::max(b.y1, p.y); }
  return b;
}
inline bool overlap(const BBox& a, const BBox& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

// Paste texture (rgb + alpha) onto `dst` so that texture quad `src` lands on `q`.
inline void warp_paste(Img& dst, const Img& rgb, const Img& alpha, const Quad& src, const Quad& q) {
  std::array<double, 9> Hinv = invert(homography(src, q));
  BBox b = bbox(q);
  int x0 = std::max(0, (int)std::floor(b.x0)), x1 = std::min(dst.w - 1, (int)std::ceil(b.x1));
  int y0 = std::max(0, (int)std::floor(b.y0)), y1 = std::min(dst.h - 1, (int)std::ceil(b.y1));
  float c[3], a;
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      Pt u = apply(Hinv, x + 0.5, y + 0.5);
      if (u.x < -1 || u.y < -1 || u.x > rgb.w + 1 || u.y > rgb.h + 1) continue;
      alpha.sample((float)u.x, (float)u.y, &a);
      if (u.x < 0 || u.y < 0 || u.x > rgb.w || u.y > rgb.h) a *= 0.5f;
      if (a <= 0.003f) continue;
      rgb.sample((float)u.x, (float)u.y, c);
      dst.blend(x, y, {c[0], c[1], c[2]}, a);
    }
}

// a plate-plane rectangle (centre 0,0) seen from a pinhole camera: yaw / pitch / roll in degrees,
// `dist` = camera distance in the same units as w,h (smaller = stronger perspective)
inline Quad project(double w, double h, double yaw, double pitch, double roll, double dist, double cx, double cy) {
  const double d2r = 3.14159265358979 / 180.0;
  double cy_ = std::cos(yaw * d2r), sy = std::sin(yaw * d2r), cp = std::cos(pitch * d2r), sp = std::sin(pitch * d2r);
  double cr = std::cos(roll * d2r), sr = std::sin(roll * d2r);
  Quad q;
  double pts[4][2] = {{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2}};
  for (int i = 0; i < 4; ++i) {
    double x = pts[i][0], y = pts[i][1], z = 0;
    double x1 = x * cy_ + z * sy, z1 = -x * sy + z * cy_;           // yaw (around y)
    double y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;             // pitch (around x)
    double x3 = x1 * cr - y2 * sr, y3 = x1 * sr + y2 * cr;           // roll (in image plane)
    double k = dist / (dist + z2);
    q[(size_t)i] = {cx + x3 * k, cy + y3 * k};
  }
  return q;
}

// ---- backgrounds and distractors -------------------------------------------------------------------
inline Img procedural_background(int W, int H, Rng& rng) {
  Img im(W, H, 3);
  Rgb a = {(float)rng.range(40, 220), (float)rng.range(40, 220), (float)rng.range(40, 220)};
  Rgb b = {(float)rng.range(20, 200), (float)rng.range(20, 200), (float)rng.range(20, 200)};
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float u = (float)y / H;
      float* p = im.at(x, y);
      p[0] = a.r + (b.r - a.r) * u; p[1] = a.g + (b.g - a.g) * u; p[2] = a.b + (b.b - a.b) * u;
    }
  for (int k = 3 + rng.below(10); k > 0; --k) {           // buildings, cars, road marks
    float x0 = (float)rng.range(-0.2, 1.0) * W, y0 = (float)rng.range(-0.2, 1.0) * H;
    float w = (float)rng.range(0.05, 0.6) * W, h = (float)rng.range(0.03, 0.4) * H;
    Rgb c = {(float)rng.range(0, 255), (float)rng.range(0, 255), (float)rng.range(0, 255)};
    fill_rrect(im, x0, y0, x0 + w, y0 + h, (float)rng.range(0, 0.3) * std::min(w, h), c, (float)rng.range(0.4, 1.0));
  }
  for (int k = rng.below(6); k > 0; --k)
    fill_circle(im, (float)rng.range(0, W), (float)rng.range(0, H), (float)rng.range(0.01, 0.12) * W,
                {(float)rng.range(0, 255), (float)rng.range(0, 255), (float)rng.range(0, 255)}, (float)rng.range(0.3, 1.0));
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float n = (float)(rng.normal() * 6);
      float* p = im.at(x, y);
      for (int c = 0; c < 3; ++c) p[c] = std::clamp(p[c] + n, 0.f, 255.f);
    }
  return im;
}

inline Img background_from(const std::string& path, int W, int H, Rng& rng) {
  std::vector<unsigned char> blob = read_file(path);
  int w, h, c;
  unsigned char* p = blob.empty() ? nullptr : stbi_load_from_memory(blob.data(), (int)blob.size(), &w, &h, &c, 3);
  if (!p) return procedural_background(W, H, rng);
  Img src = from_u8(p, w, h);
  stbi_image_free(p);
  // random crop with the output aspect, 50-100% of the largest such crop, optional mirror
  double asp = (double)W / H;
  int cw = w, ch = (int)(w / asp);
  if (ch > h) { ch = h; cw = (int)(h * asp); }
  double s = rng.range(0.5, 1.0);
  cw = std::max(8, (int)(cw * s)); ch = std::max(8, (int)(ch * s));
  int ox = rng.below(w - cw + 1), oy = rng.below(h - ch + 1);
  Img crop(cw, ch, 3);
  bool mirror = rng.chance(0.5);
  for (int y = 0; y < ch; ++y)
    for (int x = 0; x < cw; ++x) {
      const float* q = src.at(ox + (mirror ? cw - 1 - x : x), oy + y);
      float* d = crop.at(x, y);
      d[0] = q[0]; d[1] = q[1]; d[2] = q[2];
    }
  return resize(crop, W, H);
}

// A sign / sticker with Japanese text: an unlabelled negative so a detector learns that "white
// rectangle with characters" is not enough (bus-stop signs were a real false positive).
inline Texture render_sign(const FontSet& fs, const std::vector<Region>& regions, Rng& rng) {
  int W = (int)rng.range(200, 600), H = (int)(W * rng.range(0.25, 0.9));
  Texture tx;
  tx.rgb = Img(W, H, 3);
  tx.alpha = Img(W, H, 1, 0.f);
  static const Rgb bgs[] = {{240, 240, 240}, {20, 60, 160}, {20, 110, 60}, {250, 210, 40}, {200, 30, 30}, {30, 30, 30}};
  Rgb bg = jitter(rng, bgs[rng.below(6)], 12);
  Rgb fg = (bg.r + bg.g + bg.b > 450) ? Rgb{20, 20, 30} : Rgb{245, 245, 245};
  float r = (float)rng.range(0, 0.15) * H;
  fill_rrect(tx.rgb, 0, 0, (float)W, (float)H, r, bg);
  fill_rrect(tx.alpha, 0, 0, (float)W, (float)H, r, {}, 1.f);
  Img mask(W, H, 1, 0.f);
  int lines = 1 + rng.below(2);
  for (int l = 0; l < lines; ++l) {
    std::string s = rng.pick(regions).name;
    if (rng.chance(0.5)) s += rng.pick(regions).name;
    if (rng.chance(0.4)) s += std::to_string(rng.below(1000));
    int face = fs.pick(REGION, s, rng, 0.0);
    float y0 = H * (0.12f + 0.8f * l / lines), y1 = H * (0.12f + 0.8f * (l + 1) / lines) - H * 0.1f;
    draw_run(mask, glyphs_for(fs, face, REGION, s), Box{W * 0.06f, y0, W * 0.94f, y1}, 0.05f, 0, 1.f);
  }
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (mask.at(x, y)[0] > 0) tx.rgb.blend(x, y, fg, mask.at(x, y)[0]);
  tx.px0 = 0; tx.py0 = 0; tx.px1 = (float)W; tx.py1 = (float)H;
  return tx;
}

// ---- one scene -------------------------------------------------------------------------------------
struct SceneOpts {
  int W = 1280, H = 960;
  int max_plates = 4;
  double min_width = 0.03, max_width = 0.35;  // plate width / image width (article: 440 * U(0.1, 1.0) / 1280)
  double max_yaw = 45, max_pitch = 30, max_roll = 12;
  double car_p = 0.6;                         // draw a car body behind the plate
  double sign_p = 0.5;                        // unlabelled sign distractors
  double blur_max = 1.6, motion_p = 0.15, noise_max = 10;
  int jpg_lo = 35, jpg_hi = 95;
  std::vector<std::string> backgrounds;       // image paths; empty = procedural
};

struct Placed {
  PlateText text;
  Quad quad;                                  // the plate (without holder) in the scene
  std::string fonts;
};

inline Img make_scene(const FontSet& fs, const std::vector<Region>& regions, const ContentOpts& co, const PlateOpts& po,
                      const SceneOpts& so, Rng& rng, std::vector<Placed>& placed) {
  Img im = so.backgrounds.empty() ? procedural_background(so.W, so.H, rng)
                                  : background_from(rng.pick(so.backgrounds), so.W, so.H, rng);
  std::vector<BBox> taken;
  if (rng.chance(so.sign_p))
    for (int k = 1 + rng.below(2); k > 0; --k) {
      Texture s = render_sign(fs, regions, rng);
      double w = rng.range(0.06, 0.3) * so.W, h = w * s.rgb.h / s.rgb.w;
      Quad q = project(w, h, rng.range(-30, 30), rng.range(-15, 15), rng.range(-8, 8), w * rng.range(1.5, 5),
                       rng.range(w, so.W - w), rng.range(h, so.H - h));
      Quad src = {Pt{0, 0}, Pt{(double)s.rgb.w, 0}, Pt{(double)s.rgb.w, (double)s.rgb.h}, Pt{0, (double)s.rgb.h}};
      warp_paste(im, s.rgb, s.alpha, src, q);
      taken.push_back(bbox(q));
    }
  struct Todo { Texture tx; Quad outer, inner; PlateText text; double sc; };
  std::vector<Todo> todo;
  int n = 1 + rng.below(so.max_plates);
  for (int k = 0; k < n; ++k) {
    PlateText t = sample_text(rng, regions, co);
    Texture tx = render_plate(t, fs, po, rng);
    // plate width log-uniform: small plates are the common case in real footage
    double pw = so.W * std::exp(rng.range(std::log(so.min_width), std::log(so.max_width)));
    double sc = pw / (tx.px1 - tx.px0);                       // scene px per texture px
    double tw = tx.rgb.w * sc, th = tx.rgb.h * sc;
    double yaw = rng.range(-so.max_yaw, so.max_yaw), pitch = rng.range(-so.max_pitch, so.max_pitch);
    double roll = rng.normal() * so.max_roll / 2.5, dist = pw * rng.range(1.2, 6.0);
    bool ok = false;
    Quad outer{}, inner{};
    for (int tries = 0; tries < 30 && !ok; ++tries) {
      double cx = rng.range(tw * 0.6, so.W - tw * 0.6), cy = rng.range(th * 0.6, so.H - th * 0.6);
      outer = project(tw, th, yaw, pitch, roll, dist, cx, cy);
      BBox b = bbox(outer);
      if (b.x0 < 1 || b.y0 < 1 || b.x1 > so.W - 2 || b.y1 > so.H - 2) continue;
      BBox grown{b.x0 - 0.15 * (b.x1 - b.x0), b.y0 - 0.15 * (b.y1 - b.y0), b.x1 + 0.15 * (b.x1 - b.x0), b.y1 + 0.15 * (b.y1 - b.y0)};
      ok = true;
      for (const BBox& o : taken) if (overlap(grown, o)) { ok = false; break; }   // article: reject any overlap
    }
    if (!ok) continue;
    Quad src = {Pt{0, 0}, Pt{(double)tx.rgb.w, 0}, Pt{(double)tx.rgb.w, (double)tx.rgb.h}, Pt{0, (double)tx.rgb.h}};
    std::array<double, 9> H = homography(src, outer);
    inner = {apply(H, tx.px0, tx.py0), apply(H, tx.px1, tx.py0), apply(H, tx.px1, tx.py1), apply(H, tx.px0, tx.py1)};
    taken.push_back(bbox(outer));
    todo.push_back({std::move(tx), outer, inner, t, sc});
  }
  // every car body first, then the plates: a body pasted later must never cover an earlier plate
  for (const Todo& d : todo) {
    if (!rng.chance(so.car_p)) continue;
    Img body(64, 64, 3), ba(64, 64, 1, 0.f);
    Rgb c = {(float)rng.range(0, 255), (float)rng.range(0, 255), (float)rng.range(0, 255)};
    if (rng.chance(0.4)) { float g = (float)rng.range(10, 245); c = {g, g, g}; }
    fill_rrect(body, 0, 0, 64, 64, 10, c);
    fill_rrect(ba, 0, 0, 64, 64, 10, {}, 1.f);
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x) {
        float* p = body.at(x, y);
        float g = 0.7f + 0.5f * (float)y / 64;
        for (int i = 0; i < 3; ++i) p[i] = std::min(255.f, p[i] * g);
      }
    double ex = rng.range(1.6, 3.5), ey = rng.range(1.5, 3.0);
    Pt ctr = {(d.inner[0].x + d.inner[2].x) / 2, (d.inner[0].y + d.inner[2].y) / 2};
    Quad bq;
    for (int i = 0; i < 4; ++i) bq[(size_t)i] = {ctr.x + (d.outer[(size_t)i].x - ctr.x) * ex, ctr.y + (d.outer[(size_t)i].y - ctr.y) * ey};
    warp_paste(im, body, ba, Quad{Pt{0, 0}, Pt{64, 0}, Pt{64, 64}, Pt{0, 64}}, bq);
  }
  for (const Todo& d : todo) {
    // texture far larger than its footprint: pre-shrink so the warp does not alias
    int f = (int)std::floor(1.0 / std::max(1e-6, d.sc * 2.0));
    const Img& rgb = f >= 2 ? shrink(d.tx.rgb, f) : d.tx.rgb;
    const Img& al = f >= 2 ? shrink(d.tx.alpha, f) : d.tx.alpha;
    warp_paste(im, rgb, al, Quad{Pt{0, 0}, Pt{(double)rgb.w, 0}, Pt{(double)rgb.w, (double)rgb.h}, Pt{0, (double)rgb.h}}, d.outer);
    placed.push_back({d.text, d.inner, d.tx.fonts});
  }
  // camera: blur (article kernel 1/3/5), motion, sensor noise, colour cast
  if (rng.chance(0.15)) box_blur(im, 1 + rng.below(2));
  else gaussian_blur(im, rng.range(0.0, so.blur_max));
  if (rng.chance(so.motion_p)) motion_blur(im, 2 + rng.below(6), rng.range(0, 3.14159));
  double sigma = rng.range(0, so.noise_max), gain = rng.range(0.7, 1.25), bias = rng.range(-20, 20);
  Rgb cast = {(float)rng.range(0.9, 1.1), (float)rng.range(0.9, 1.1), (float)rng.range(0.9, 1.1)};
  for (int y = 0; y < im.h; ++y)
    for (int x = 0; x < im.w; ++x) {
      float* p = im.at(x, y);
      float mul[3] = {cast.r, cast.g, cast.b};
      for (int c = 0; c < 3; ++c) p[c] = std::clamp((float)(p[c] * gain * mul[c] + bias + rng.normal() * sigma), 0.f, 255.f);
    }
  return im;
}

inline Img crop(const Img& im, int x0, int y0, int x1, int y1) {
  x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(im.w, x1); y1 = std::min(im.h, y1);
  Img o(std::max(1, x1 - x0), std::max(1, y1 - y0), im.c);
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x)
      for (int c = 0; c < im.c; ++c) o.at(x, y)[c] = im.at(x0 + x, y0 + y)[c];
  return o;
}

}  // namespace jp
