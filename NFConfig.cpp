// NFConfig.cpp - 配置读写与热重载
// NF.ini 放在 DLL 自身目录，UTF-16LE BOM，ReadDirectoryChangesW 热重载。
#include "NFConfig.h"

#include <windows.h>

#include <mutex>
#include <thread>

namespace config {
namespace {

const wchar_t kSecPickup[] = L"自动拾取";
const wchar_t kSecMove[] = L"顺图";
const wchar_t kSecEquip[] = L"装备处理";
const wchar_t kSecHotkey[] = L"热键";
const wchar_t kSecDebug[] = L"调试";

const wchar_t* const kQualityKeys[kQualityCount] = {
    L"白装", L"蓝装", L"紫装", L"粉装", L"史诗", L"异界"};

std::wstring g_ini_path;
std::wstring g_ini_dir;
std::mutex g_mutex;
Settings g_settings;
std::vector<std::function<void(const Settings&)>> g_listeners;

HANDLE g_watch_stop = nullptr;
std::thread g_watch_thread;
volatile LONG g_saving = 0;

// 取 DLL 自身目录（修掉参考用 nullptr 取 EXE 目录的 bug）。
std::wstring ModuleDirectory(HMODULE module) {
  wchar_t buf[MAX_PATH] = {0};
  if (GetModuleFileNameW(module, buf, MAX_PATH) == 0) return L".\\";
  std::wstring path(buf);
  const size_t pos = path.find_last_of(L'\\');
  if (pos == std::wstring::npos) return L".\\";
  return path.substr(0, pos + 1);
}

int ReadInt(const wchar_t* sec, const wchar_t* key, int def) {
  return static_cast<int>(
      GetPrivateProfileIntW(sec, key, def, g_ini_path.c_str()));
}

std::wstring ReadStr(const wchar_t* sec, const wchar_t* key,
                     const wchar_t* def) {
  wchar_t buf[1024] = {0};
  GetPrivateProfileStringW(sec, key, def, buf, 1024, g_ini_path.c_str());
  return std::wstring(buf);
}

void WriteInt(const wchar_t* sec, const wchar_t* key, int value) {
  wchar_t buf[32] = {0};
  _snwprintf_s(buf, _TRUNCATE, L"%d", value);
  WritePrivateProfileStringW(sec, key, buf, g_ini_path.c_str());
}

void WriteStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
  WritePrivateProfileStringW(sec, key, v.c_str(), g_ini_path.c_str());
}

// 确保 ini 存在且为 UTF-16LE（Windows Profile API 才会以宽字符写入）。
void EnsureIniFile() {
  const DWORD attr = GetFileAttributesW(g_ini_path.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES) return;

  HANDLE h = CreateFileW(g_ini_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  const unsigned char bom[2] = {0xFF, 0xFE};
  DWORD written = 0;
  WriteFile(h, bom, 2, &written, nullptr);
  CloseHandle(h);
}

void NotifyListeners(const Settings& s) {
  std::vector<std::function<void(const Settings&)>> copy;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    copy = g_listeners;
  }
  for (auto& cb : copy) {
    if (cb) cb(s);
  }
}

// 从 ini 读取需要持久化的部分（热键 + 调试 + 过滤ID/名称过滤 +
// 拾取模式 + 顺图模式 + 6 品质装备动作）。
// 过滤ID/名称过滤无界面输入入口，仍由 ini 提供并支持热重载；
// 拾取模式持久化: 0=吸物（默认）1=发包，缺省/非法值回落吸物；
// 装备动作: 白装/蓝装/紫装/粉装/史诗/异界 = 0保留/1卖物/2分解，非法值回落各自默认；
// 顺图模式持久化: 0=坐标顺图（默认）1=强制顺图，缺省/非法值回落坐标顺图；
// 其余（过滤模式、拾取间隔）为纯内存，
// 不读文件、不写文件，初始即 Settings 默认值（与 XUI 布局默认一致）。
void LoadFromFile() {
  Settings s;
  // [热键] —— 从 ini 读原始字符串，缺项落回 Settings 默认值，随后写回补齐。
  s.hotkey_toggle_ui     = ReadStr(kSecHotkey, L"呼出界面",   s.hotkey_toggle_ui.c_str());
  s.hotkey_move_up       = ReadStr(kSecHotkey, L"顺图上",     s.hotkey_move_up.c_str());
  s.hotkey_move_down     = ReadStr(kSecHotkey, L"顺图下",     s.hotkey_move_down.c_str());
  s.hotkey_move_left     = ReadStr(kSecHotkey, L"顺图左",     s.hotkey_move_left.c_str());
  s.hotkey_move_right    = ReadStr(kSecHotkey, L"顺图右",     s.hotkey_move_right.c_str());
  s.hotkey_toggle_pickup = ReadStr(kSecHotkey, L"拾取开关",   s.hotkey_toggle_pickup.c_str());
  s.hotkey_process_equip = ReadStr(kSecHotkey, L"一键处理",   s.hotkey_process_equip.c_str());
  s.hotkey_auto_grade    = ReadStr(kSecHotkey, L"自动评分",   s.hotkey_auto_grade.c_str());

  // [调试]
  s.debug_enabled = ReadInt(kSecDebug, L"debug", 1);
  if (s.debug_enabled != 0) s.debug_enabled = 1;

  // [自动拾取]: 过滤ID（无 UI 输入，用户直接改 ini）与拾取模式持久化；
  // [装备处理]: 名称过滤 + 6 品质动作持久化（动作键名 白装/蓝装/紫装/粉装/史诗/异界）。
  s.filter_ids  = ReadStr(kSecPickup, L"过滤ID", s.filter_ids.c_str());
  s.pickup_mode = ReadInt(kSecPickup, L"拾取模式", 0);
  if (s.pickup_mode != 0) s.pickup_mode = 1;
  s.name_filter = ReadStr(kSecEquip,  L"名称过滤", s.name_filter.c_str());
  for (int q = 0; q < kQualityCount; ++q) {
    const int v = ReadInt(kSecEquip, kQualityKeys[q], s.equip_action[q]);
    if (v >= 0 && v <= 2) s.equip_action[q] = v;
  }

  // [顺图]: 顺图模式持久化 (0=坐标顺图（默认）1=强制顺图)。
  s.move_mode = ReadInt(kSecMove, L"顺图模式", 0);
  if (s.move_mode != 0) s.move_mode = 1;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settings = s;
  }
}

// 热重载（NF.ini 被外部改动时）：只合并持久化字段（热键 + 调试 + 过滤ID/名称过滤 +
// 拾取模式 + 顺图模式 + 6 品质装备动作）到现有 g_settings，保留内存中的过滤模式，
// 避免外部改个热键就把当次会话的模式/规则冲掉。
void LoadPersisted() {
  Settings patch;
  patch.hotkey_toggle_ui     = ReadStr(kSecHotkey, L"呼出界面",   patch.hotkey_toggle_ui.c_str());
  patch.hotkey_move_up       = ReadStr(kSecHotkey, L"顺图上",     patch.hotkey_move_up.c_str());
  patch.hotkey_move_down     = ReadStr(kSecHotkey, L"顺图下",     patch.hotkey_move_down.c_str());
  patch.hotkey_move_left     = ReadStr(kSecHotkey, L"顺图左",     patch.hotkey_move_left.c_str());
  patch.hotkey_move_right    = ReadStr(kSecHotkey, L"顺图右",     patch.hotkey_move_right.c_str());
  patch.hotkey_toggle_pickup = ReadStr(kSecHotkey, L"拾取开关",   patch.hotkey_toggle_pickup.c_str());
  patch.hotkey_process_equip = ReadStr(kSecHotkey, L"一键处理",   patch.hotkey_process_equip.c_str());
  patch.hotkey_auto_grade    = ReadStr(kSecHotkey, L"自动评分",   patch.hotkey_auto_grade.c_str());
  patch.debug_enabled = ReadInt(kSecDebug, L"debug", 1);
  if (patch.debug_enabled != 0) patch.debug_enabled = 1;
  patch.filter_ids  = ReadStr(kSecPickup, L"过滤ID",  patch.filter_ids.c_str());
  patch.pickup_mode = ReadInt(kSecPickup, L"拾取模式", patch.pickup_mode);
  if (patch.pickup_mode != 0) patch.pickup_mode = 1;
  patch.name_filter = ReadStr(kSecEquip,  L"名称过滤", patch.name_filter.c_str());
  for (int q = 0; q < kQualityCount; ++q) {
    const int v = ReadInt(kSecEquip, kQualityKeys[q], patch.equip_action[q]);
    if (v >= 0 && v <= 2) patch.equip_action[q] = v;
  }
  patch.move_mode = ReadInt(kSecMove, L"顺图模式", patch.move_mode);
  if (patch.move_mode != 0) patch.move_mode = 1;

  std::lock_guard<std::mutex> lock(g_mutex);
  g_settings.hotkey_toggle_ui     = patch.hotkey_toggle_ui;
  g_settings.hotkey_move_up       = patch.hotkey_move_up;
  g_settings.hotkey_move_down     = patch.hotkey_move_down;
  g_settings.hotkey_move_left     = patch.hotkey_move_left;
  g_settings.hotkey_move_right    = patch.hotkey_move_right;
  g_settings.hotkey_toggle_pickup = patch.hotkey_toggle_pickup;
  g_settings.hotkey_process_equip = patch.hotkey_process_equip;
  g_settings.hotkey_auto_grade    = patch.hotkey_auto_grade;
  g_settings.debug_enabled        = patch.debug_enabled;
  g_settings.filter_ids           = patch.filter_ids;
  g_settings.pickup_mode          = patch.pickup_mode;
  g_settings.name_filter          = patch.name_filter;
  for (int q = 0; q < kQualityCount; ++q)
    g_settings.equip_action[q] = patch.equip_action[q];
  g_settings.move_mode            = patch.move_mode;
}

// 只持久化热键 + 调试 + 过滤ID/名称过滤 + 拾取模式 + 顺图模式 + 6 品质装备动作。
// 过滤模式、拾取间隔等其余字段为纯内存，
// 不写文件（内存路径见 Set：g_settings 全量更新，本函数只落盘持久化字段）。
void SaveToFile(const Settings& s) {
  InterlockedExchange(&g_saving, 1);
  // [热键] —— 保证 ini 里这 8 个键永远存在，用户手动改时不用猜字段名。
  WriteStr(kSecHotkey, L"呼出界面", s.hotkey_toggle_ui);
  WriteStr(kSecHotkey, L"顺图上",   s.hotkey_move_up);
  WriteStr(kSecHotkey, L"顺图下",   s.hotkey_move_down);
  WriteStr(kSecHotkey, L"顺图左",   s.hotkey_move_left);
  WriteStr(kSecHotkey, L"顺图右",   s.hotkey_move_right);
  WriteStr(kSecHotkey, L"拾取开关", s.hotkey_toggle_pickup);
  WriteStr(kSecHotkey, L"一键处理", s.hotkey_process_equip);
  WriteStr(kSecHotkey, L"自动评分", s.hotkey_auto_grade);

  // [调试]
  WriteInt(kSecDebug, L"debug", s.debug_enabled);

  // 过滤ID —— 无界面输入，持久化供用户手改（键名与读取一致）。
  WriteStr(kSecPickup, L"过滤ID",  s.filter_ids);
  // 拾取模式 —— 面板切换也走这里落盘，重启后保持上次选择（键名与读取一致）。
  WriteInt(kSecPickup, L"拾取模式", s.pickup_mode);
  // 名称过滤 —— 无界面输入，持久化供用户手改（键名与读取一致）。
  WriteStr(kSecEquip,  L"名称过滤", s.name_filter);
  // 装备动作 (0=保留 1=卖物 2=分解) —— 面板改动也走这里落盘，重启后保持。
  for (int q = 0; q < kQualityCount; ++q)
    WriteInt(kSecEquip, kQualityKeys[q], s.equip_action[q]);
  // 顺图模式 —— 面板改动也走这里落盘，重启后保持上次选择（键名与读取一致）。
  WriteInt(kSecMove, L"顺图模式", s.move_mode);

  WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_ini_path.c_str());
  InterlockedExchange(&g_saving, 0);
}

void WatchProc() {
  HANDLE dir = CreateFileW(g_ini_dir.c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                           nullptr);
  if (dir == INVALID_HANDLE_VALUE) return;

  OVERLAPPED ov = {};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) {
    CloseHandle(dir);
    return;
  }

  std::vector<char> buffer(8192);
  for (;;) {
    ResetEvent(ov.hEvent);
    DWORD returned = 0;
    if (!ReadDirectoryChangesW(dir, buffer.data(),
                               static_cast<DWORD>(buffer.size()), FALSE,
                               FILE_NOTIFY_CHANGE_LAST_WRITE |
                                   FILE_NOTIFY_CHANGE_SIZE,
                               &returned, &ov, nullptr)) {
      break;
    }

    HANDLE waits[2] = {g_watch_stop, ov.hEvent};
    const DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (r != WAIT_OBJECT_0 + 1) break;

    DWORD bytes = 0;
    if (!GetOverlappedResult(dir, &ov, &bytes, FALSE)) break;
    if (bytes == 0) continue;
    if (InterlockedCompareExchange(&g_saving, 0, 0) != 0) continue;

    // 简单去抖，等待写入完成。
    Sleep(120);
    Reload();
  }

  CancelIo(dir);
  CloseHandle(ov.hEvent);
  CloseHandle(dir);
}

}  // namespace

std::vector<int32_t> Settings::ParsedFilterIds() const {
  std::vector<int32_t> out;
  std::wstring cur;
  for (wchar_t c : filter_ids) {
    if (c == L',' || c == L'，' || c == L' ') {
      if (!cur.empty()) {
        out.push_back(static_cast<int32_t>(_wtoi(cur.c_str())));
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(static_cast<int32_t>(_wtoi(cur.c_str())));
  return out;
}

std::vector<std::wstring> Settings::ParsedNameFilters() const {
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : name_filter) {
    if (c == L'-') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

void Initialize(HMODULE module) {
  g_ini_dir = ModuleDirectory(module);
  g_ini_path = g_ini_dir + L"NF.ini";
  EnsureIniFile();

  LoadFromFile();
  // 回写一次，补齐缺失字段的默认值。
  SaveToFile(Get());

  g_watch_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_watch_thread = std::thread(WatchProc);
}

void Shutdown() {
  if (g_watch_stop != nullptr) SetEvent(g_watch_stop);
  if (g_watch_thread.joinable()) g_watch_thread.join();
  if (g_watch_stop != nullptr) {
    CloseHandle(g_watch_stop);
    g_watch_stop = nullptr;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_listeners.clear();
}

Settings Get() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_settings;
}

void Set(const Settings& s) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settings = s;
  }
  SaveToFile(s);
  NotifyListeners(s);
}

void Reload() {
  LoadPersisted();
  NotifyListeners(Get());
}

void Save() { SaveToFile(Get()); }

void RegisterListener(std::function<void(const Settings&)> cb) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_listeners.push_back(std::move(cb));
}

const std::wstring& IniPath() { return g_ini_path; }

}  // namespace config
