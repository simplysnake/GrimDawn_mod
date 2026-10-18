#include "steam_shortcut.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace gdvr::steam {

namespace {

constexpr char kAppName[] = "Grim Dawn VR";

// ---- Steam's binary key-value format ---------------------------------------------------------------
// Each entry: type byte, key (zero-terminated), value. 0x00 = nested list (ends with 0x08), 0x01 = string,
// 0x02 = int32; the others (float, pointer, color, uint64) are kept as raw bytes.
struct Node {
  uint8_t type = 0;
  std::string key;
  std::string value;  // string, or the raw bytes of a fixed-size value
  std::vector<Node> children;
};

size_t FixedSize(uint8_t type) {
  switch (type) {
    case 2: case 3: case 4: case 6: return 4;
    case 7: return 8;
    default: return 0;
  }
}

bool ReadCString(const std::string& d, size_t& pos, std::string& out) {
  const size_t end = d.find('\0', pos);
  if (end == std::string::npos) return false;
  out = d.substr(pos, end - pos);
  pos = end + 1;
  return true;
}

bool Parse(const std::string& d, size_t& pos, std::vector<Node>& out) {
  while (pos < d.size()) {
    const uint8_t type = uint8_t(d[pos++]);
    if (type == 8) return true;
    Node n;
    n.type = type;
    if (!ReadCString(d, pos, n.key)) return false;
    if (type == 0) {
      if (!Parse(d, pos, n.children)) return false;
    } else if (type == 1) {
      if (!ReadCString(d, pos, n.value)) return false;
    } else if (size_t size = FixedSize(type)) {
      if (pos + size > d.size()) return false;
      n.value = d.substr(pos, size);
      pos += size;
    } else {
      return false;  // unknown type: leave the file alone
    }
    out.push_back(std::move(n));
  }
  return false;  // ran out before the closing 0x08
}

void Write(const std::vector<Node>& nodes, std::string& out) {
  for (const Node& n : nodes) {
    out += char(n.type);
    out += n.key;
    out += '\0';
    if (n.type == 0) {
      Write(n.children, out);
    } else {
      out += n.value;
      if (n.type == 1) out += '\0';
    }
  }
  out += char(8);
}

Node Str(const char* key, const std::string& v) { return {1, key, v, {}}; }
Node Int(const char* key, uint32_t v) { return {2, key, std::string(reinterpret_cast<const char*>(&v), 4), {}}; }

std::string Narrow(const std::wstring& w) {
  std::string out(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), int(out.size()), nullptr, nullptr);
  return out;
}

std::wstring Widen(const std::string& s) {
  std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), int(out.size()));
  return out;
}

uint32_t Crc32(const std::string& s) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char c : s) {
    crc ^= c;
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

Node* Find(std::vector<Node>& nodes, const char* key) {
  for (Node& n : nodes)
    if (_stricmp(n.key.c_str(), key) == 0) return &n;
  return nullptr;
}

std::string Unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

bool SamePath(const std::string& a, const fs::path& b) {
  std::error_code ec;
  const fs::path pa = Widen(Unquote(a));
  return fs::equivalent(pa, b, ec) || _wcsicmp(pa.wstring().c_str(), b.wstring().c_str()) == 0;
}

// An earlier install's entry: same launcher name, but that file is gone.
bool StaleOwnEntry(const std::string& exe) {
  const fs::path p = Widen(Unquote(exe));
  std::error_code ec;
  return _wcsicmp(p.filename().wstring().c_str(), L"gdvr_launch.exe") == 0 && !fs::exists(p, ec);
}

Node MakeEntry(const std::string& index, const fs::path& exe) {
  const std::string quoted = "\"" + Narrow(exe.wstring()) + "\"";
  const std::string dir = "\"" + Narrow(exe.parent_path().wstring()) + "\\\"";
  Node e{0, index, {}, {}};
  e.children = {
      Int("appid", Crc32(quoted + kAppName) | 0x80000000u),
      Str("AppName", kAppName),
      Str("Exe", quoted),
      Str("StartDir", dir),
      Str("icon", Narrow(exe.wstring())),
      Str("ShortcutPath", ""),
      Str("LaunchOptions", ""),
      Int("IsHidden", 0),
      Int("AllowDesktopConfig", 1),
      Int("AllowOverlay", 1),
      Int("OpenVR", 1),  // listed with the VR games
      Int("Devkit", 0),
      Str("DevkitGameID", ""),
      Int("DevkitOverrideAppID", 0),
      Int("LastPlayTime", 0),
      Str("FlatpakAppID", ""),
      Str("sortas", ""),
      Node{0, "tags", {}, {Str("0", "VR")}},
  };
  return e;
}

std::string ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

bool WriteAll(const fs::path& p, const std::string& data) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << data;
  return bool(f);
}

bool SteamRunning() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W pe = {sizeof(pe)};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
    found = _wcsicmp(pe.szExeFile, L"steam.exe") == 0;
  CloseHandle(snap);
  return found;
}

// GDVR_TEST_STEAM_DIR (tests): a folder that stands in for Steam's (no check for a running Steam).
fs::path TestSteamDir() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"GDVR_TEST_STEAM_DIR", buf, MAX_PATH);
  return n > 0 && n < MAX_PATH ? fs::path(buf) : fs::path();
}

fs::path SteamDir() {
  if (fs::path test = TestSteamDir(); !test.empty()) return test;
  wchar_t buf[MAX_PATH];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) !=
      ERROR_SUCCESS)
    return {};
  return buf;
}

// Applies `edit` to the shortcut list of every Steam account on this PC.
template <typename Edit>
Result ForEachAccount(Edit edit, std::wstring& detail) {
  const fs::path steam = SteamDir();
  std::error_code ec;
  if (steam.empty() || !fs::exists(steam / L"userdata", ec)) {
    detail = L"Steam was not found.";
    return Result::NoSteam;
  }
  if (TestSteamDir().empty() && SteamRunning()) {
    detail = L"Steam is running. Close Steam (Steam menu, Exit), then try again.";
    return Result::SteamRunning;
  }
  int changed = 0;
  for (const auto& user : fs::directory_iterator(steam / L"userdata", ec)) {
    const fs::path config = user.path() / L"config";
    if (!fs::is_directory(config, ec)) continue;
    const fs::path file = config / L"shortcuts.vdf";
    std::vector<Node> root;
    if (fs::exists(file, ec)) {
      const std::string data = ReadAll(file);
      size_t pos = 0;
      if (!Parse(data, pos, root)) {
        detail += L"Could not read " + file.wstring() + L" (left unchanged).\n";
        continue;
      }
      fs::copy_file(file, fs::path(file).concat(L".gdvr-backup"), fs::copy_options::overwrite_existing, ec);
    }
    Node* list = Find(root, "shortcuts");
    if (!list) {
      root.push_back(Node{0, "shortcuts", {}, {}});
      list = &root.back();
    }
    if (!edit(list->children)) continue;
    for (size_t i = 0; i < list->children.size(); ++i) list->children[i].key = std::to_string(i);  // 0, 1, 2...
    std::string out;
    Write(root, out);
    if (!WriteAll(file, out)) {
      detail += L"Could not write " + file.wstring() + L".\n";
      return Result::Failed;
    }
    ++changed;
  }
  detail += std::to_wstring(changed) + L" Steam account(s) updated.";
  return Result::Ok;
}

}  // namespace

Result AddShortcut(const fs::path& exe, std::wstring& detail) {
  return ForEachAccount(
      [&](std::vector<Node>& list) {
        for (Node& e : list) {
          Node* path = Find(e.children, "Exe");
          if (path && (SamePath(path->value, exe) || StaleOwnEntry(path->value))) {
            e = MakeEntry(e.key, exe);  // update in place (keeps its position)
            return true;
          }
        }
        list.push_back(MakeEntry(std::to_string(list.size()), exe));
        return true;
      },
      detail);
}

Result RemoveShortcut(const fs::path& exe, std::wstring& detail) {
  return ForEachAccount(
      [&](std::vector<Node>& list) {
        const size_t before = list.size();
        std::erase_if(list, [&](Node& e) {
          Node* path = Find(e.children, "Exe");
          return path && SamePath(path->value, exe);
        });
        return list.size() != before;
      },
      detail);
}

bool RoundTrip(const fs::path& in, const fs::path& out) {
  const std::string data = ReadAll(in);
  std::vector<Node> root;
  size_t pos = 0;
  if (!Parse(data, pos, root)) return false;
  std::string written;
  Write(root, written);
  return WriteAll(out, written) && written == data;
}

}  // namespace gdvr::steam
