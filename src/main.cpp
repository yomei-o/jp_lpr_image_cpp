// jp_lpr_image — synthetic Japanese licence-plate dataset generator. See README.md.
//
//   jp_lpr_image --out out --count 1000 [--crops] [--bg backgrounds/] [--threads 8]
#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "jp_plate.hpp"

#include <cstdlib>
#include <string>
#include <vector>
#ifdef _OPENMP
#include <omp.h>
#endif

using namespace jp;

static std::string arg(int argc, char** argv, const char* k, const std::string& def) {
  for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], k)) return argv[i + 1];
  return def;
}
static bool flag(int argc, char** argv, const char* k) {
  for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], k)) return true;
  return false;
}
static std::vector<double> numbers(const std::string& s) {
  std::vector<double> v;
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find(',', p);
    v.push_back(atof(s.substr(p, e == std::string::npos ? std::string::npos : e - p).c_str()));
    if (e == std::string::npos) break;
    p = e + 1;
  }
  return v;
}

// Japanese faces that ship with Windows. They are read where they are installed and never copied.
static const char* kSystemFonts[] = {"meiryo.ttc", "meiryob.ttc", "msgothic.ttc", "YuGothB.ttc", "YuGothM.ttc",
                                     "BIZ-UDGothicB.ttc", "BIZ-UDGothicR.ttc", "UDDigiKyokashoN-B.ttc",
                                     "UDDigiKyokashoN-R.ttc", "HGRSGU.TTC", "HGRGE.TTC", "HGRSMP.TTF"};

static void usage() {
  printf(
      "jp_lpr_image --out DIR --count N [options]\n"
      "  --seed S                 (12345) image i uses seed S+i, so any range can be regenerated\n"
      "  --start I                (0)     first image index\n"
      "  --size WxH               (1280x960, the article's background size)\n"
      "  --bg DIR                 background photos (jpg/png); default: procedural backgrounds\n"
      "  --fonts DIR              (fonts) bundled faces, every .ttf/.ttc/.otf in it\n"
      "  --sysfonts DIR|none      (C:/Windows/Fonts on Windows) installed Japanese faces to add\n"
      "  --regions FILE           (data/regions.txt)\n"
      "  --max-plates N           (4) plates per image (1..N)\n"
      "  --width MIN,MAX          (0.03,0.35) plate width / image width\n"
      "  --classes plate|category (plate) YOLO class: 0 = plate, or the 4 plate types\n"
      "  --category-w a,b,c,d     (0.4,0.15,0.3,0.15) 自家用,事業用,軽自家用,軽事業用\n"
      "  --gotochi-p P            share of ご当地 regions (default: all 138 regions equally)\n"
      "  --trm-p P                (0.35) chance of TRM font JB for a field it can draw\n"
      "  --crops                  also write recogniser crops in the alpr_jp layout + labels.tsv\n"
      "  --crop-min PX            (40) skip crops narrower than this\n"
      "  --no-scenes              crops only (scenes are still rendered, not saved)\n"
      "  --png                    save scenes as PNG instead of JPEG\n"
      "  --threads N              OpenMP threads\n"
      "  --list-fonts             print the faces found and exit\n");
}

int main(int argc, char** argv) {
  if (argc < 2 || flag(argc, argv, "-h") || flag(argc, argv, "--help")) { usage(); return argc < 2 ? 1 : 0; }
  std::string out = arg(argc, argv, "--out", "out");
  int count = atoi(arg(argc, argv, "--count", "10").c_str());
  uint64_t seed = strtoull(arg(argc, argv, "--seed", "12345").c_str(), nullptr, 10);
  int start = atoi(arg(argc, argv, "--start", "0").c_str());
  bool crops = flag(argc, argv, "--crops"), scenes = !flag(argc, argv, "--no-scenes");
  bool by_category = arg(argc, argv, "--classes", "plate") == "category";
  int crop_min = atoi(arg(argc, argv, "--crop-min", "40").c_str());

  // fonts: bundled first (they are the fallback order), then the OS faces
  FontSet fs;
  std::string fdir = arg(argc, argv, "--fonts", "fonts");
  for (const std::string& f : list_files(fdir)) {
    std::string l = lower(f);
    if (l.size() < 4) continue;
    std::string ext = l.substr(l.size() - 4);
    if (ext != ".ttf" && ext != ".ttc" && ext != ".otf") continue;
    Font fo;
    if (load_font(fdir + "/" + f, fo)) fs.fonts.push_back(std::move(fo));
    else printf("warn: cannot load %s\n", f.c_str());
  }
#ifdef _WIN32
  std::string sdir = arg(argc, argv, "--sysfonts", "C:/Windows/Fonts");
#else
  std::string sdir = arg(argc, argv, "--sysfonts", "none");
#endif
  if (sdir != "none")
    for (const char* f : kSystemFonts) {
      Font fo;
      if (load_font(sdir + "/" + f, fo)) { fo.system = true; fs.fonts.push_back(std::move(fo)); }
    }
  // GenSen (or any face with full coverage) first, so per-glyph fallback finds digits there
  std::stable_sort(fs.fonts.begin(), fs.fonts.end(), [](const Font& a, const Font& b) {
    return (a.has('0') && a.has(0x6A2A)) > (b.has('0') && b.has(0x6A2A));
  });
  if (fs.fonts.empty()) { printf("no fonts in %s\n", fdir.c_str()); return 1; }

  std::vector<Region> regions = load_regions(arg(argc, argv, "--regions", "data/regions.txt"));
  if (regions.empty()) { printf("no regions (data/regions.txt)\n"); return 1; }

  if (flag(argc, argv, "--list-fonts")) {
    for (const Font& f : fs.fonts) {
      int reg = 0, dig = 0;
      for (const Region& r : regions) reg += fs.usable(f, REGION, r.name) ? 1 : 0;
      dig = fs.usable(f, SERIAL, "0123456789") ? 1 : 0;
      printf("%-28s %-7s regions %3d/%zu  digits %s  hiragana %s%s\n", f.name.c_str(), f.system ? "system" : "bundled",
             reg, regions.size(), dig ? "yes" : "no ", fs.usable(f, HIRA, "さ") ? "yes" : "partial",
             f.trm ? "  (TRM key map)" : "");
    }
    return 0;
  }

  ContentOpts co;
  PlateOpts po;
  SceneOpts so;
  {
    std::string sz = arg(argc, argv, "--size", "1280x960");
    size_t x = sz.find('x');
    if (x != std::string::npos) { so.W = atoi(sz.substr(0, x).c_str()); so.H = atoi(sz.substr(x + 1).c_str()); }
  }
  so.max_plates = std::max(1, atoi(arg(argc, argv, "--max-plates", "4").c_str()));
  std::vector<double> wr = numbers(arg(argc, argv, "--width", "0.03,0.35"));
  if (wr.size() == 2) { so.min_width = wr[0]; so.max_width = wr[1]; }
  std::vector<double> cw = numbers(arg(argc, argv, "--category-w", "0.4,0.15,0.3,0.15"));
  if (cw.size() == 4) co.category_w = cw;
  co.gotochi_p = atof(arg(argc, argv, "--gotochi-p", "-1").c_str());
  po.trm_p = atof(arg(argc, argv, "--trm-p", "0.35").c_str());
  std::string bg = arg(argc, argv, "--bg", "");
  if (!bg.empty())
    for (const std::string& f : list_files(bg)) {
      std::string l = lower(f);
      if (l.find(".jpg") != std::string::npos || l.find(".jpeg") != std::string::npos || l.find(".png") != std::string::npos)
        so.backgrounds.push_back(bg + "/" + f);
    }
  bool png = flag(argc, argv, "--png");
#ifdef _OPENMP
  int th = atoi(arg(argc, argv, "--threads", "0").c_str());
  if (th > 0) omp_set_num_threads(th);
#endif

  if (scenes) { make_dirs(out + "/images"); make_dirs(out + "/labels"); }
  if (crops)
    for (const char* c : kCategoryDir)
      for (const Region& r : regions) make_dirs(out + "/crops/" + c + "/" + r.name);

  int nsys = 0, ntrm = 0;
  for (const Font& f : fs.fonts) { nsys += f.system; ntrm += f.trm; }
  printf("%zu faces (%d system, TRM %s), %zu regions, %zu backgrounds%s\n", fs.fonts.size(), nsys,
         ntrm ? "yes" : "no", regions.size(), so.backgrounds.size(), so.backgrounds.empty() ? " (procedural)" : "");

  std::vector<std::string> meta((size_t)count), clab((size_t)count);
  std::vector<int> nplates((size_t)count, 0), ncrops((size_t)count, 0);
#pragma omp parallel for schedule(dynamic)
  for (int i = 0; i < count; ++i) {
    int idx = start + i;
    Rng rng(seed * 0x100000001B3ULL + (uint64_t)idx);
    rng.next();
    std::vector<Placed> placed;
    Img im = make_scene(fs, regions, co, po, so, rng, placed);
    int q = rng.below(so.jpg_hi - so.jpg_lo + 1) + so.jpg_lo;
    char name[32];
    snprintf(name, sizeof name, "%08d", idx);
    if (crops) jpeg_roundtrip(im, q);         // crops get the same block artefacts as the scene
    std::string yolo, m, cl;
    for (size_t k = 0; k < placed.size(); ++k) {
      const Placed& p = placed[k];
      BBox b = bbox(p.quad);
      double x0 = std::max(0.0, b.x0), y0 = std::max(0.0, b.y0), x1 = std::min((double)im.w, b.x1), y1 = std::min((double)im.h, b.y1);
      char line[256];
      snprintf(line, sizeof line, "%d %.6f %.6f %.6f %.6f\n", by_category ? p.text.category : 0, (x0 + x1) / 2 / im.w,
               (y0 + y1) / 2 / im.h, (x1 - x0) / im.w, (y1 - y0) / im.h);
      yolo += line;
      m += std::string(name) + "\t" + std::to_string(k) + "\t" + kCategoryName[p.text.category] + "\t" + p.text.region + "\t" +
           p.text.cls + "\t" + p.text.hira + "\t" + p.text.serial;
      for (const Pt& c : p.quad) { snprintf(line, sizeof line, "\t%.1f\t%.1f", c.x, c.y); m += line; }
      m += "\t" + p.fonts + "\n";
      if (crops && x1 - x0 >= crop_min) {
        double mx = (x1 - x0) * rng.range(0.02, 0.15), my = (y1 - y0) * rng.range(0.02, 0.2);
        Img c = crop(im, (int)(x0 - mx), (int)(y0 - my), (int)std::ceil(x1 + mx), (int)std::ceil(y1 + my));
        std::string key = std::string(kCategoryDir[p.text.category]) + "/" + p.text.region + "/" + name + "_" + std::to_string(k) + ".png";
        if (save_image(out + "/crops/" + key, c, 0)) {
          cl += key + "\t\t" + p.text.cls + "\t" + p.text.hira + "\t" + p.text.serial + "\n";
          ncrops[(size_t)i]++;
        }
      }
    }
    if (scenes) {
      save_image(out + "/images/" + name + (png ? ".png" : ".jpg"), im, png ? 0 : q);
      write_file(out + "/labels/" + name + ".txt", yolo.data(), yolo.size());
    }
    meta[(size_t)i] = m;
    clab[(size_t)i] = cl;
    nplates[(size_t)i] = (int)placed.size();
  }

  // tables, in image order whatever the thread schedule was
  std::string M = "# file\tk\tcategory\tregion\tclass\thiragana\tserial(. = blank)\tTLx\tTLy\tTRx\tTRy\tBRx\tBRy\tBLx\tBLy\tfonts(region,class,hira,serial)\n";
  for (const std::string& s : meta) M += s;
  std::string mp = out + (start ? "/meta_" + std::to_string(start) + ".tsv" : "/meta.tsv");
  write_file(mp, M.data(), M.size());
  if (crops) {
    std::string L = "# key\tregion\tcls\thira\tnum  (yolo_lpr_cpp spec/alpr_labels.tsv format; region empty = the folder)\n";
    for (const std::string& s : clab) L += s;
    std::string lp = out + (start ? "/crops/labels_" + std::to_string(start) + ".tsv" : "/crops/labels.tsv");
    write_file(lp, L.data(), L.size());
  }
  long np = 0, nc = 0;
  for (int i = 0; i < count; ++i) { np += nplates[(size_t)i]; nc += ncrops[(size_t)i]; }
  printf("wrote %d images, %ld plates%s into %s\n", count, np, crops ? (", " + std::to_string(nc) + " crops").c_str() : "", out.c_str());
  return 0;
}
