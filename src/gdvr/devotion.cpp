#include "devotion.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mmsystem.h>  // MAKEFOURCC
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace gdvr::devotion {

namespace {

template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    c = char(tolower(static_cast<unsigned char>(c)));
    if (c == '\\') c = '/';
  }
  return s;
}

// ---- LZ4 block format (Grim Dawn compresses archive parts and database records with it) --------
bool Lz4Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
  const uint8_t* ip = src;
  const uint8_t* const iend = src + srcSize;
  uint8_t* op = dst;
  uint8_t* const oend = dst + dstSize;
  while (ip < iend) {
    const unsigned token = *ip++;
    size_t literals = token >> 4;
    if (literals == 15) {
      unsigned b;
      do {
        if (ip >= iend) return false;
        b = *ip++;
        literals += b;
      } while (b == 255);
    }
    if (size_t(iend - ip) < literals || size_t(oend - op) < literals) return false;
    std::memcpy(op, ip, literals);
    ip += literals;
    op += literals;
    if (ip >= iend) break;  // the last sequence has literals only
    if (iend - ip < 2) return false;
    const size_t offset = size_t(ip[0]) | (size_t(ip[1]) << 8);
    ip += 2;
    if (offset == 0 || size_t(op - dst) < offset) return false;
    size_t match = token & 15;
    if (match == 15) {
      unsigned b;
      do {
        if (ip >= iend) return false;
        b = *ip++;
        match += b;
      } while (b == 255);
    }
    match += 4;
    if (size_t(oend - op) < match) return false;
    const uint8_t* m = op - offset;
    for (size_t i = 0; i < match; ++i) op[i] = m[i];  // may overlap
    op += match;
  }
  return op == oend;
}

bool ReadAt(HANDLE file, uint64_t offset, void* out, DWORD size) {
  OVERLAPPED ov{};
  ov.Offset = DWORD(offset);
  ov.OffsetHigh = DWORD(offset >> 32);
  DWORD got = 0;
  return ReadFile(file, out, size, &got, &ov) && got == size;
}

// ---- .arc archives (version 3) --------------------------------------------------------------------
struct Arc {
  HANDLE file = INVALID_HANDLE_VALUE;
  struct Part {
    uint32_t offset, compressed, size;
  };
  struct Entry {
    uint32_t type, offset, compressed, size, parts, first;
  };
  std::vector<Part> parts;
  std::unordered_map<std::string, Entry> files;

  bool Open(const std::wstring& path) {
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    uint32_t h[7];
    if (!ReadAt(file, 0, h, sizeof(h)) || h[0] != 0x435241 || h[1] != 3) return false;
    const uint32_t nfiles = h[2], nparts = h[3], ptsize = h[4], stsize = h[5], ptoff = h[6];
    std::vector<uint8_t> tables(size_t(ptsize) + stsize + size_t(nfiles) * 44);
    if (!ReadAt(file, ptoff, tables.data(), DWORD(tables.size()))) return false;
    parts.resize(nparts);
    std::memcpy(parts.data(), tables.data(), size_t(nparts) * 12);
    const char* strings = reinterpret_cast<const char*>(tables.data() + ptsize);
    const uint8_t* entries = tables.data() + ptsize + stsize;
    for (uint32_t i = 0; i < nfiles; ++i) {
      const uint8_t* e = entries + size_t(i) * 44;
      uint32_t f[5], tail[4];
      std::memcpy(f, e, 20);
      std::memcpy(tail, e + 28, 16);  // after the 64-bit file time
      if (tail[3] + tail[2] > stsize) continue;
      files[Lower(std::string(strings + tail[3], tail[2]))] = {f[0], f[1], f[2], f[3], tail[0], tail[1]};
    }
    return true;
  }
  bool Read(const std::string& name, std::vector<uint8_t>& out) const {
    auto it = files.find(name);
    if (it == files.end()) return false;
    const Entry& e = it->second;
    out.resize(e.size);
    if (e.type == 1 && e.compressed == e.size) return ReadAt(file, e.offset, out.data(), e.size);
    size_t at = 0;
    std::vector<uint8_t> chunk;
    for (uint32_t p = e.first; p < e.first + e.parts && p < parts.size(); ++p) {
      const Part& part = parts[p];
      if (at + part.size > out.size()) return false;
      if (part.compressed == part.size) {
        if (!ReadAt(file, part.offset, out.data() + at, part.size)) return false;
      } else {
        chunk.resize(part.compressed);
        if (!ReadAt(file, part.offset, chunk.data(), part.compressed) ||
            !Lz4Decompress(chunk.data(), chunk.size(), out.data() + at, part.size))
          return false;
      }
      at += part.size;
    }
    return at == out.size();
  }
  ~Arc() {
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  }
};

// ---- .arz databases (version 3) -----------------------------------------------------------------
struct Arz {
  std::vector<uint8_t> data;
  std::vector<std::string> strings;
  struct Record {
    uint32_t offset, compressed, size;
  };
  std::unordered_map<std::string, Record> records;

  bool Open(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    data.resize(size_t(size.QuadPart));
    const bool ok = ReadAt(f, 0, data.data(), DWORD(data.size()));
    CloseHandle(f);
    if (!ok || data.size() < 24) return false;
    uint16_t v[2];
    uint32_t h[5];
    std::memcpy(v, data.data(), 4);
    std::memcpy(h, data.data() + 4, 20);
    if (v[1] != 3) return false;
    const uint32_t rtoff = h[0], rcount = h[2], stoff = h[3];
    size_t p = stoff;
    uint32_t count = U32(p);
    p += 4;
    strings.reserve(count);
    for (uint32_t i = 0; i < count && p + 4 <= data.size(); ++i) {
      const uint32_t n = U32(p);
      strings.emplace_back(reinterpret_cast<const char*>(&data[p + 4]), n);
      p += 4 + n;
    }
    p = rtoff;
    for (uint32_t i = 0; i < rcount && p + 8 <= data.size(); ++i) {
      const uint32_t nameIdx = U32(p), typeLen = U32(p + 4);
      const size_t q = p + 8 + typeLen;
      const uint32_t off = U32(q), comp = U32(q + 4), sz = U32(q + 8);
      if (nameIdx < strings.size()) records[Lower(strings[nameIdx])] = {off + 24, comp, sz};
      p = q + 12 + 8;
    }
    return true;
  }
  uint32_t U32(size_t at) const {
    uint32_t v = 0;
    if (at + 4 <= data.size()) std::memcpy(&v, &data[at], 4);
    return v;
  }
};

// One record's fields: key -> (type, first value). Strings resolve through the database's table.
struct Fields {
  std::unordered_map<std::string, std::pair<uint16_t, uint32_t>> values;
  const Arz* db = nullptr;
  bool Has(const std::string& k) const { return values.count(k) != 0; }
  std::string Str(const std::string& k) const {
    auto it = values.find(k);
    return it != values.end() && it->second.first == 2 && it->second.second < db->strings.size()
               ? db->strings[it->second.second]
               : std::string();
  }
  int Int(const std::string& k) const {
    auto it = values.find(k);
    if (it == values.end()) return 0;
    if (it->second.first == 1) {
      float f;
      std::memcpy(&f, &it->second.second, 4);
      return int(f);
    }
    return int(it->second.second);
  }
};

class Database {
 public:
  std::vector<std::unique_ptr<Arz>> dbs;  // later ones override earlier ones
  bool Get(const std::string& name, Fields& out) const {
    const std::string key = Lower(name);
    for (auto it = dbs.rbegin(); it != dbs.rend(); ++it) {
      auto r = (*it)->records.find(key);
      if (r == (*it)->records.end()) continue;
      const Arz& db = **it;
      std::vector<uint8_t> raw(r->second.size);
      if (r->second.offset + r->second.compressed > db.data.size() ||
          !Lz4Decompress(&db.data[r->second.offset], r->second.compressed, raw.data(), raw.size()))
        return false;
      out.values.clear();
      out.db = &db;
      for (size_t p = 0; p + 8 <= raw.size();) {
        uint16_t type, count;
        uint32_t keyIdx;
        std::memcpy(&type, &raw[p], 2);
        std::memcpy(&count, &raw[p + 2], 2);
        std::memcpy(&keyIdx, &raw[p + 4], 4);
        p += 8;
        uint32_t first = 0;
        if (count && p + 4 <= raw.size()) std::memcpy(&first, &raw[p], 4);
        if (keyIdx < db.strings.size()) out.values[db.strings[keyIdx]] = {type, first};
        p += size_t(count) * 4;
      }
      return true;
    }
    return false;
  }
};

// ---- What gets drawn -------------------------------------------------------------------------------
struct Image {
  std::vector<uint8_t> tex;  // the .tex file (12-byte header + DDS)
};
enum class Kind { Nebula, Art, Star };
struct Sprite {
  std::string image;  // archive name
  int x = 0, y = 0;   // top-left on the devotion map
  float alpha = 1;
  int group = -1;     // constellation (its art and stars turn together), -1 = none
  float rotX = 0, rotY = 0, angle = 0;  // turned by `angle` (radians) around (rotX, rotY)
  Kind kind = Kind::Nebula;
  int star = -1;      // stars: index into MapData::skills
};
struct MapData {
  std::vector<Sprite> sprites;  // in drawing order: nebulas, constellation art, stars
  std::unordered_map<std::string, Image> images;
  float cx = 0, cy = 0, radius = 1;  // the circle (map units) holding everything, turned sprites included
  int constellations = 0;
  std::vector<std::string> skills;   // each star's skill record (allocated = the player has it)
  std::vector<int> skillGroup;       // ...and its constellation
  std::unordered_map<std::string, int> skillIndex;  // skill record -> index
  std::string glow;                  // star glow image ("" if missing)
};

// Corner k (0 TL, 1 TR, 2 BR, 3 BL) of a sprite of size w x h, turned around its rotation centre.
void Corner(const Sprite& s, int w, int h, int k, float& x, float& y) {
  const float lx = float(s.x + ((k == 1 || k == 2) ? w : 0)) - s.rotX;
  const float ly = float(s.y + (k >= 2 ? h : 0)) - s.rotY;
  const float c = std::cos(s.angle), sn = std::sin(s.angle);
  x = s.rotX + lx * c - ly * sn;
  y = s.rotY + lx * sn + ly * c;
}

std::mutex g_mutex;
std::unique_ptr<MapData> g_data;  // set by the loader thread
std::atomic<bool> g_started{false}, g_loaded{false}, g_failed{false};
std::thread g_loader;  // joined on shutdown (no thread left running in an unloaded DLL)
std::string g_status = "not started";
ID3D11Texture2D* g_mapTex = nullptr;
ID3D11ShaderResourceView* g_mapSrv = nullptr;

void SetStatus(const std::string& s) {
  std::lock_guard lock(g_mutex);
  g_status = s;
}

std::wstring GameDir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);  // ...\Grim Dawn\x64\Grim Dawn.exe
  std::wstring d = exe;
  d = d.substr(0, d.find_last_of(L"\\/"));  // ...\x64
  return d.substr(0, d.find_last_of(L"\\/"));
}

// DDS width/height of a .tex file.
bool TexSize(const std::vector<uint8_t>& t, int& w, int& h) {
  if (t.size() < 12 + 128) return false;
  uint32_t hh, ww;
  std::memcpy(&hh, &t[12 + 12], 4);
  std::memcpy(&ww, &t[12 + 16], 4);
  w = int(ww);
  h = int(hh);
  return w > 0 && h > 0;
}

void Load() {
  const std::wstring game = GameDir();
  Database db;
  for (const wchar_t* rel : {L"\\database\\database.arz", L"\\gdx1\\database\\GDX1.arz", L"\\gdx2\\database\\GDX2.arz",
                             L"\\gdx3\\database\\GDX3.arz"}) {
    auto arz = std::make_unique<Arz>();
    if (arz->Open(game + rel)) db.dbs.push_back(std::move(arz));
  }
  std::vector<std::unique_ptr<Arc>> arcs;
  for (const wchar_t* rel : {L"\\resources\\UI.arc", L"\\gdx1\\resources\\UI.arc", L"\\gdx2\\resources\\UI.arc",
                             L"\\gdx3\\resources\\UI.arc"}) {
    auto arc = std::make_unique<Arc>();
    if (arc->Open(game + rel)) arcs.push_back(std::move(arc));
  }
  if (db.dbs.empty() || arcs.empty()) {
    SetStatus("game files not found");
    g_failed = true;
    Log("devotion: game database or UI archive not found under %ls", game.c_str());
    return;
  }
  auto data = std::make_unique<MapData>();
  // Loads an image from the newest archive that has it; false if none does.
  auto image = [&](std::string name) {
    name = Lower(name);
    if (name.rfind("ui/", 0) == 0) name = name.substr(3);
    if (data->images.count(name)) return true;
    for (auto it = arcs.rbegin(); it != arcs.rend(); ++it) {
      Image img;
      if ((*it)->Read(name, img.tex)) {
        data->images[name] = std::move(img);
        return true;
      }
    }
    return false;
  };
  auto key = [](std::string name) {
    name = Lower(name);
    return name.rfind("ui/", 0) == 0 ? name.substr(3) : name;
  };
  std::vector<Sprite> nebulas, art, stars;
  Fields master;
  if (!db.Get("records/ui/skills/devotion/devotion_mastertable.dbr", master)) {
    SetStatus("devotion table not found");
    g_failed = true;
    return;
  }
  for (int i = 1; i < 10; ++i) {
    Fields n;
    char rec[96];
    snprintf(rec, sizeof(rec), "records/ui/skills/devotion/devotion_nebula%02d.dbr", i);
    if (db.Get(rec, n) && image(n.Str("bitmapName")))
      nebulas.push_back({key(n.Str("bitmapName")), n.Int("bitmapPositionX"), n.Int("bitmapPositionY"), 1.0f});
  }
  const std::string star = "skills/devotion/devotion_star_up.tex";
  const bool haveStar = image(star);
  if (image("skills/devotion/devotion_starglow01.tex")) data->glow = "skills/devotion/devotion_starglow01.tex";
  for (int i = 1; i < 300; ++i) {
    const std::string field = "devotionConstellation" + std::to_string(i);
    if (!master.Has(field)) continue;
    Fields c, bg;
    if (!db.Get(master.Str(field), c)) continue;
    const std::string bgRec = c.Str("constellationBackground");
    if (bgRec.empty() || !db.Get(bgRec, bg) || !image(bg.Str("bitmapName"))) continue;  // art not installed
    const int group = data->constellations;
    art.push_back({key(bg.Str("bitmapName")), bg.Int("bitmapPositionX"), bg.Int("bitmapPositionY"), 1.0f, group,
                   0, 0, 0, Kind::Art});
    ++data->constellations;
    for (int s = 1; s < 40 && haveStar; ++s) {
      Fields b;
      const std::string button = c.Str("devotionButton" + std::to_string(s));
      if (!button.empty() && db.Get(button, b)) {
        stars.push_back({star, b.Int("bitmapPositionX"), b.Int("bitmapPositionY"), 1.0f, group, 0, 0, 0, Kind::Star,
                         int(data->skills.size())});
        data->skillIndex[Lower(b.Str("skillName"))] = int(data->skills.size());
        data->skills.push_back(Lower(b.Str("skillName")));
        data->skillGroup.push_back(group);
      }
    }
  }
  for (auto* list : {&nebulas, &art, &stars})
    for (const Sprite& s : *list) data->sprites.push_back(s);
  // Map centre: middle of everything as laid out.
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (Sprite& s : data->sprites) {
    int w, h;
    if (!TexSize(data->images[s.image].tex, w, h)) continue;
    s.rotX = s.x + w * 0.5f;  // nebulas turn around themselves (not at all: angle 0)
    s.rotY = s.y + h * 0.5f;
    x0 = std::min(x0, float(s.x));
    y0 = std::min(y0, float(s.y));
    x1 = std::max(x1, float(s.x + w));
    y1 = std::max(y1, float(s.y + h));
  }
  data->cx = (x0 + x1) * 0.5f;
  data->cy = (y0 + y1) * 0.5f;
  // On the dome the map's centre is straight overhead, so "up" for someone looking at a constellation
  // points to the map's centre. Each constellation (art and stars) turns around its art's centre so
  // its top faces the centre: it reads upright wherever you look.
  std::vector<float> groupX(data->constellations), groupY(data->constellations), groupAngle(data->constellations);
  for (const Sprite& s : art) {
    int w, h;
    if (s.group < 0 || !TexSize(data->images[s.image].tex, w, h)) continue;
    const float ax = s.x + w * 0.5f, ay = s.y + h * 0.5f;
    groupX[s.group] = ax;
    groupY[s.group] = ay;
    groupAngle[s.group] = std::atan2(data->cx - ax, -(data->cy - ay));  // turns the top (0,-1) towards the centre
  }
  float radius = 1;
  for (Sprite& s : data->sprites) {
    if (s.group >= 0) {
      s.rotX = groupX[s.group];
      s.rotY = groupY[s.group];
      s.angle = groupAngle[s.group];
    }
    int w, h;
    if (!TexSize(data->images[s.image].tex, w, h)) continue;
    for (int k = 0; k < 4; ++k) {
      float x, y;
      Corner(s, w, h, k, x, y);
      if (s.group >= 0 || s.image.find("nebula") == std::string::npos)  // nebulas may spill past the rim
        radius = std::max(radius, std::hypot(x - data->cx, y - data->cy));
    }
  }
  data->radius = radius * 1.02f;
  Log("devotion: %d constellations, %zu sprites, %zu images from %zu databases and %zu archives; map radius %.0f",
      data->constellations, data->sprites.size(), data->images.size(), db.dbs.size(), arcs.size(), data->radius);
  SetStatus("loaded " + std::to_string(data->constellations) + " constellations, drawing the map...");
  {
    std::lock_guard lock(g_mutex);
    g_data = std::move(data);
  }
  g_loaded = true;
}

// ---- Drawing the map on the GPU --------------------------------------------------------------------
constexpr char kSpriteShader[] = R"(
cbuffer Sprite : register(b0) { float4 c01; float4 c23; float4 tint; };  // corners in NDC: TL, TR, BR, BL
Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
  uint k = id < 3 ? id : (id == 3 ? 0 : (id == 4 ? 2 : 3));  // triangles TL TR BR, TL BR BL
  float2 p = k == 0 ? c01.xy : (k == 1 ? c01.zw : (k == 2 ? c23.xy : c23.zw));
  VSOut o;
  o.pos = float4(p, 0, 1);
  o.uv = float2((k == 1 || k == 2) ? 1 : 0, k >= 2 ? 1 : 0);
  return o;
}
float4 PS(VSOut i) : SV_Target { return tex.Sample(samp, i.uv) * tint; }
)";

// A D3D texture from a .tex file (uncompressed BGRA or DXT1/3/5, with its mip chain).
ID3D11ShaderResourceView* MakeImage(ID3D11Device* device, const std::vector<uint8_t>& t) {
  if (t.size() < 12 + 128) return nullptr;
  const uint8_t* dds = t.data() + 12;
  uint32_t height, width, mips, pfFlags, fourcc, bits;
  std::memcpy(&height, dds + 12, 4);
  std::memcpy(&width, dds + 16, 4);
  std::memcpy(&mips, dds + 28, 4);
  std::memcpy(&pfFlags, dds + 80, 4);
  std::memcpy(&fourcc, dds + 84, 4);
  std::memcpy(&bits, dds + 88, 4);
  mips = std::max(1u, mips);
  DXGI_FORMAT format;
  UINT block = 0;  // bytes per 4x4 block (0 = uncompressed)
  if (fourcc == MAKEFOURCC('D', 'X', 'T', '1')) format = DXGI_FORMAT_BC1_UNORM, block = 8;
  else if (fourcc == MAKEFOURCC('D', 'X', 'T', '3')) format = DXGI_FORMAT_BC2_UNORM, block = 16;
  else if (fourcc == MAKEFOURCC('D', 'X', 'T', '5')) format = DXGI_FORMAT_BC3_UNORM, block = 16;
  else if (fourcc == 0 && bits == 32) format = DXGI_FORMAT_B8G8R8A8_UNORM;
  else return nullptr;
  if (block && (width % 4 || height % 4)) mips = 1;  // BC needs whole blocks
  std::vector<D3D11_SUBRESOURCE_DATA> init(mips);
  size_t at = 128;
  UINT w = width, h = height, made = 0;
  for (UINT m = 0; m < mips; ++m) {
    const UINT pitch = block ? std::max(1u, (w + 3) / 4) * block : w * 4;
    const UINT rows = block ? std::max(1u, (h + 3) / 4) : h;
    if (at + size_t(pitch) * rows > t.size() - 12) break;
    init[m] = {dds + at, pitch, 0};
    at += size_t(pitch) * rows;
    ++made;
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  if (!made) return nullptr;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = width;
  d.Height = height;
  d.MipLevels = made;
  d.ArraySize = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  if (SUCCEEDED(device->CreateTexture2D(&d, init.data(), &tex))) device->CreateShaderResourceView(tex, nullptr, &srv);
  SafeRelease(tex);
  return srv;
}

// ---- Which stars the player has (asked from the running game) --------------------------------------
// gGameEngine -> GameEngine::GetMainPlayer -> Character::GetSkillManager -> its skills and their levels
// (0 when the player doesn't have that skill). Devotion stars are skills once allocated.
// Every devotion star exists as a skill object; an allocated one has a level above 0.
struct GameApi {
  void** engine = nullptr;  // &gGameEngine
  void* (*getMainPlayer)(void* engine) = nullptr;
  void* (*getSkillManager)(void* character) = nullptr;
  void* (*getSkillList)(void* skills) = nullptr;           // const mem::vector<Skill*>& (begin, end)
  const char* (*getObjectName)(void* object) = nullptr;    // the skill's record
  uint32_t (*getSkillLevel)(void* skill) = nullptr;
  uint32_t (*pointsSpent)(void* skills) = nullptr;
  bool ok = false;
};
GameApi g_api;

void ResolveGameApi() {
  HMODULE game = GetModuleHandleW(L"Game.dll");
  if (!game) return;
  g_api.engine = reinterpret_cast<void**>(GetProcAddress(game, "?gGameEngine@GAME@@3PEAVGameEngine@1@EA"));
  g_api.getMainPlayer = reinterpret_cast<void* (*)(void*)>(
      GetProcAddress(game, "?GetMainPlayer@GameEngine@GAME@@QEBAPEAVPlayer@2@XZ"));
  g_api.getSkillManager = reinterpret_cast<void* (*)(void*)>(
      GetProcAddress(game, "?GetSkillManager@Character@GAME@@QEAAAEAVSkillManager@2@XZ"));
  g_api.getSkillList = reinterpret_cast<void* (*)(void*)>(
      GetProcAddress(game, "?GetSkillList@SkillManager@GAME@@QEBAAEBV?$vector@PEAVSkill@GAME@@@mem@@XZ"));
  g_api.getSkillLevel = reinterpret_cast<uint32_t (*)(void*)>(GetProcAddress(game, "?GetSkillLevel@Skill@GAME@@QEBA?BIXZ"));
  g_api.pointsSpent = reinterpret_cast<uint32_t (*)(void*)>(
      GetProcAddress(game, "?GetNumDevotionPointsSpent@SkillManager@GAME@@QEBAIXZ"));
  if (HMODULE engine = GetModuleHandleW(L"Engine.dll"))
    g_api.getObjectName = reinterpret_cast<const char* (*)(void*)>(
        GetProcAddress(engine, "?GetObjectName@Object@GAME@@QEBAPEBDXZ"));
  g_api.ok = g_api.engine && g_api.getMainPlayer && g_api.getSkillManager && g_api.getSkillList &&
             g_api.getSkillLevel && g_api.getObjectName && g_api.pointsSpent;
  Log("devotion: game skill queries %s", g_api.ok ? "found" : "NOT found (completed constellations won't glow)");
}

// The player's skills with a level above 0 (record names copied out), straight from the game.
struct RawSkill {
  char name[160];
};
constexpr int kMaxSkills = 1024;
RawSkill g_rawSkills[kMaxSkills];

// Returns how many allocated skills were copied to g_rawSkills, or -1 if there is no player (menus) or
// anything faults. Plain data only (it runs under SEH).
int QueryAllocated(uint32_t& spent) {
  __try {
    void* engine = *g_api.engine;
    if (!engine) return -1;
    void* player = g_api.getMainPlayer(engine);
    if (!player) return -1;
    void* manager = g_api.getSkillManager(player);
    if (!manager) return -1;
    spent = g_api.pointsSpent(manager);
    void** list = static_cast<void**>(g_api.getSkillList(manager));  // begin, end
    void** begin = static_cast<void**>(list[0]);
    void** end = static_cast<void**>(list[1]);
    int n = 0;
    for (void** it = begin; it < end && n < kMaxSkills; ++it) {
      if (!*it || g_api.getSkillLevel(*it) == 0) continue;
      const char* name = g_api.getObjectName(*it);
      if (!name) continue;
      strncpy_s(g_rawSkills[n].name, name, _TRUNCATE);
      ++n;
    }
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// Fills have[i] (1 = the player has allocated star i). False if there is no player (menus).
bool QueryStars(const std::unordered_map<std::string, int>& starIndex, std::vector<uint8_t>& have, uint32_t& spent) {
  const int n = QueryAllocated(spent);
  if (n < 0) return false;
  for (int k = 0; k < n; ++k) {
    auto it = starIndex.find(Lower(g_rawSkills[k].name));
    if (it != starIndex.end()) have[it->second] = 1;
  }
  return true;
}

// ---- Drawing the map on the GPU --------------------------------------------------------------------
struct Look {
  bool known = false;            // the player's stars are known (in game, not in menus)
  std::vector<uint8_t> have;     // per star
  float incomplete = 0.45f;      // brightness of constellations not completed
  float glow = 0.5f;             // glow strength of completed ones / allocated stars
};

std::unique_ptr<MapData> g_map;  // kept after loading, for redrawing when the player's progress changes
std::unordered_map<std::string, ID3D11ShaderResourceView*> g_images;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_blendAlpha = nullptr;
ID3D11BlendState* g_blendAdd = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_size = 0;
Look g_drawn;                    // what the map currently shows
bool g_dirty = true;
int g_complete = 0, g_allocated = 0;
uint32_t g_spent = 0;

void ReleaseGpu() {
  for (auto& [name, srv] : g_images) SafeRelease(srv);
  g_images.clear();
  SafeRelease(g_rtv);
  SafeRelease(g_depth);
  SafeRelease(g_raster);
  SafeRelease(g_samp);
  SafeRelease(g_blendAdd);
  SafeRelease(g_blendAlpha);
  SafeRelease(g_cb);
  SafeRelease(g_ps);
  SafeRelease(g_vs);
  SafeRelease(g_mapSrv);
  SafeRelease(g_mapTex);
}

bool CreateGpu(ID3D11Device* device, const MapData& data) {
  ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
  bool ok = SUCCEEDED(D3DCompile(kSpriteShader, sizeof(kSpriteShader) - 1, "devotion", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err)) &&
            SUCCEEDED(D3DCompile(kSpriteShader, sizeof(kSpriteShader) - 1, "devotion", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err)) &&
            SUCCEEDED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
            SUCCEEDED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
  if (!ok) Log("devotion: shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
  SafeRelease(err);
  SafeRelease(psb);
  SafeRelease(vsb);
  // A square canvas around the circle that holds the whole map, up to 4096 px.
  g_size = UINT(std::min(4096.0f, data.radius * 2));
  if (ok) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = g_size;
    d.MipLevels = 0;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    D3D11_BUFFER_DESC bd{48, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    D3D11_BLEND_DESC alpha{}, add{};
    alpha.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                             D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    add.RenderTarget[0] = {TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                           D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_COLOR_WRITE_ENABLE_ALL};
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g_mapTex)) &&
         SUCCEEDED(device->CreateShaderResourceView(g_mapTex, nullptr, &g_mapSrv)) &&
         SUCCEEDED(device->CreateRenderTargetView(g_mapTex, nullptr, &g_rtv)) &&
         SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_cb)) && SUCCEEDED(device->CreateBlendState(&alpha, &g_blendAlpha)) &&
         SUCCEEDED(device->CreateBlendState(&add, &g_blendAdd)) && SUCCEEDED(device->CreateSamplerState(&sd, &g_samp)) &&
         SUCCEEDED(device->CreateRasterizerState(&rd, &g_raster)) && SUCCEEDED(device->CreateDepthStencilState(&dd, &g_depth));
  }
  if (ok)  // the images, once (kept for redrawing)
    for (auto& [name, img] : data.images)
      if (ID3D11ShaderResourceView* srv = MakeImage(device, img.tex)) g_images[name] = srv;
  if (!ok) ReleaseGpu();
  return ok;
}

// Draws the whole map for this look (what the player has completed).
void Redraw(ID3D11Device* device, const MapData& data, const Look& look) {
  ID3D11DeviceContext* ctx = nullptr;
  device->GetImmediateContext(&ctx);
  // Save what this changes, so the game's (cached) pipeline state is untouched afterwards.
  ID3D11RenderTargetView* oldRtv = nullptr;
  ID3D11DepthStencilView* oldDsv = nullptr;
  ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
  UINT nvp = 1;
  D3D11_VIEWPORT oldVp{};
  ctx->RSGetViewports(&nvp, &oldVp);
  ID3D11BlendState* oldBlend = nullptr;
  float oldFactor[4];
  UINT oldMask;
  ctx->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
  ID3D11DepthStencilState* oldDepth = nullptr;
  UINT oldRef;
  ctx->OMGetDepthStencilState(&oldDepth, &oldRef);
  ID3D11RasterizerState* oldRaster = nullptr;
  ctx->RSGetState(&oldRaster);
  ID3D11VertexShader* oldVs = nullptr;
  ID3D11PixelShader* oldPs = nullptr;
  ID3D11GeometryShader* oldGs = nullptr;
  ctx->VSGetShader(&oldVs, nullptr, nullptr);
  ctx->PSGetShader(&oldPs, nullptr, nullptr);
  ctx->GSGetShader(&oldGs, nullptr, nullptr);
  ID3D11Buffer *oldVsCb = nullptr, *oldPsCb = nullptr;
  ctx->VSGetConstantBuffers(0, 1, &oldVsCb);
  ctx->PSGetConstantBuffers(0, 1, &oldPsCb);
  ID3D11ShaderResourceView* oldSrv = nullptr;
  ctx->PSGetShaderResources(0, 1, &oldSrv);
  ID3D11SamplerState* oldSamp = nullptr;
  ctx->PSGetSamplers(0, 1, &oldSamp);
  D3D11_PRIMITIVE_TOPOLOGY oldTopo;
  ctx->IAGetPrimitiveTopology(&oldTopo);
  ID3D11InputLayout* oldLayout = nullptr;
  ctx->IAGetInputLayout(&oldLayout);

  const float clear[4] = {0, 0, 0, 0};
  ctx->ClearRenderTargetView(g_rtv, clear);
  D3D11_VIEWPORT vp{0, 0, float(g_size), float(g_size), 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
  const float factor[4] = {};
  ctx->OMSetDepthStencilState(g_depth, 0);
  ctx->RSSetState(g_raster);
  ctx->VSSetShader(g_vs, nullptr, 0);
  ctx->PSSetShader(g_ps, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->VSSetConstantBuffers(0, 1, &g_cb);
  ctx->PSSetConstantBuffers(0, 1, &g_cb);
  ctx->PSSetSamplers(0, 1, &g_samp);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);

  // Which constellations are complete (every star allocated).
  std::vector<int> starsIn(data.constellations, 0), starsHave(data.constellations, 0);
  for (size_t i = 0; i < data.skills.size(); ++i) {
    ++starsIn[data.skillGroup[i]];
    if (look.known && look.have[i]) ++starsHave[data.skillGroup[i]];
  }
  auto complete = [&](int g) { return look.known && g >= 0 && starsIn[g] > 0 && starsHave[g] == starsIn[g]; };

  const float side = data.radius * 2, scale = float(g_size) / side;
  const float originX = data.cx - data.radius, originY = data.cy - data.radius;
  // One quad: `s`'s place (centre and size of its own image) grown by `grow`, drawn with `image`, tinted.
  auto draw = [&](const Sprite& s, const std::string& image, float grow, float r, float g, float b, float a) {
    auto it = g_images.find(image);
    auto own = data.images.find(s.image);
    if (it == g_images.end() || own == data.images.end()) return;
    int w, h;
    if (!TexSize(own->second.tex, w, h)) return;
    Sprite q = s;
    const float cx = s.x + w * 0.5f, cy = s.y + h * 0.5f;
    const float gw = w * grow, gh = h * grow;
    q.x = int(cx - gw * 0.5f);
    q.y = int(cy - gh * 0.5f);
    float c[12];
    for (int k = 0; k < 4; ++k) {
      float x, y;
      Corner(q, int(gw), int(gh), k, x, y);
      c[k * 2] = (x - originX) * scale / g_size * 2 - 1;
      c[k * 2 + 1] = 1 - (y - originY) * scale / g_size * 2;
    }
    c[8] = r, c[9] = g, c[10] = b, c[11] = a;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, c, sizeof(c));
    ctx->Unmap(g_cb, 0);
    ctx->PSSetShaderResources(0, 1, &it->second);
    ctx->Draw(6, 0);
  };

  // 1. Nebulas, constellation art and stars (incomplete constellations dimmer once the player is known).
  ctx->OMSetBlendState(g_blendAlpha, factor, 0xFFFFFFFF);
  for (const Sprite& s : data.sprites) {
    float a = s.alpha;
    if (s.kind != Kind::Nebula && look.known && !complete(s.group)) a *= look.incomplete;
    if (s.kind == Kind::Star && look.known && s.star >= 0 && look.have[s.star]) a = 1.0f;  // allocated stars bright
    draw(s, s.image, 1.0f, 1, 1, 1, a);
  }
  // 2. Glow: completed constellations bloom, allocated stars get the game's star glow.
  ctx->OMSetBlendState(g_blendAdd, factor, 0xFFFFFFFF);
  if (look.known && look.glow > 0) {
    for (const Sprite& s : data.sprites) {
      if (s.kind == Kind::Art && complete(s.group)) {
        draw(s, s.image, 1.04f, 0.75f, 0.85f, 1.0f, look.glow * 0.6f);
        draw(s, s.image, 1.10f, 0.6f, 0.75f, 1.0f, look.glow * 0.3f);
      } else if (s.kind == Kind::Star && s.star >= 0 && look.have[s.star] && !data.glow.empty()) {
        draw(s, data.glow, 1.0f, 0.8f, 0.9f, 1.0f, look.glow);  // the glow image fitted to the star
      }
    }
  }

  ID3D11ShaderResourceView* none = nullptr;
  ctx->PSSetShaderResources(0, 1, &none);
  ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
  ctx->GenerateMips(g_mapSrv);
  ctx->RSSetViewports(nvp, &oldVp);
  ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);
  ctx->OMSetDepthStencilState(oldDepth, oldRef);
  ctx->RSSetState(oldRaster);
  ctx->VSSetShader(oldVs, nullptr, 0);
  ctx->PSSetShader(oldPs, nullptr, 0);
  ctx->GSSetShader(oldGs, nullptr, 0);
  ctx->VSSetConstantBuffers(0, 1, &oldVsCb);
  ctx->PSSetConstantBuffers(0, 1, &oldPsCb);
  ctx->PSSetShaderResources(0, 1, &oldSrv);
  ctx->PSSetSamplers(0, 1, &oldSamp);
  ctx->IASetPrimitiveTopology(oldTopo);
  ctx->IASetInputLayout(oldLayout);
  for (IUnknown* u : std::initializer_list<IUnknown*>{oldRtv, oldDsv, oldBlend, oldDepth, oldRaster, oldVs, oldPs, oldGs,
                                                      oldVsCb, oldPsCb, oldSrv, oldSamp, oldLayout})
    if (u) u->Release();
  ctx->Release();

  g_complete = 0;
  for (int gi = 0; gi < data.constellations; ++gi) g_complete += complete(gi);
  g_allocated = 0;
  for (size_t i = 0; i < data.skills.size() && look.known; ++i) g_allocated += look.have[i];
}

void LoadGuarded() {
  __try {
    Load();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_failed = true;
    Log("devotion: loading the map crashed (unexpected game file contents?)");
  }
}

}  // namespace

void Start() {
  if (g_started.exchange(true)) return;
  SetStatus("loading the devotion map from the game files...");
  ResolveGameApi();
  g_loader = std::thread(LoadGuarded);
}

void SetLook(float incompleteBrightness, float glow) {
  if (g_drawn.incomplete != incompleteBrightness || g_drawn.glow != glow) g_dirty = true;
  g_drawn.incomplete = incompleteBrightness;
  g_drawn.glow = glow;
}

void* MapSrv(IDXGISwapChain* swapChain) {
  if (g_failed || !swapChain) return g_mapSrv;
  ID3D11Device* device = nullptr;
  if (!g_map) {
    if (!g_loaded) return nullptr;
    {
      std::lock_guard lock(g_mutex);
      g_map = std::move(g_data);
    }
    if (!g_map) return nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
    if (!CreateGpu(device, *g_map)) {
      SetStatus("drawing the devotion map failed");
      g_failed = true;
      device->Release();
      return nullptr;
    }
    Log("devotion: map texture %ux%u, %zu images", g_size, g_size, g_images.size());
    g_dirty = true;
  }
  // The player's progress, every 2 seconds.
  static ULONGLONG lastQuery = 0;
  if (g_api.ok && GetTickCount64() - lastQuery > 2000) {
    lastQuery = GetTickCount64();
    std::vector<uint8_t> have(g_map->skills.size(), 0);
    uint32_t spent = 0;
    const bool known = QueryStars(g_map->skillIndex, have, spent);
    if (known != g_drawn.known || (known && have != g_drawn.have)) {
      g_drawn.known = known;
      g_drawn.have = std::move(have);
      g_spent = spent;
      g_dirty = true;
    }
  }
  if (g_dirty) {
    if (!device && FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) return g_mapSrv;
    g_dirty = false;
    Redraw(device, *g_map, g_drawn);
    char status[160];
    if (g_drawn.known)
      snprintf(status, sizeof(status), "devotion map: %d constellations, %d complete, %d stars allocated (game says %u)",
               g_map->constellations, g_complete, g_allocated, g_spent);
    else
      snprintf(status, sizeof(status), "devotion map: %d constellations (no character loaded)", g_map->constellations);
    SetStatus(status);
    Log("devotion: %s", status);
  }
  if (device) device->Release();
  return g_mapSrv;
}

std::string Status() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

void Shutdown() {
  if (g_loader.joinable()) g_loader.join();
  ReleaseGpu();
  g_map.reset();
}

}  // namespace gdvr::devotion
