// NF.cpp - 插件入口与控制线程
// 结构参照 EquipmentSwap：ClientPatchPluginInit 启动控制线程，
// 控制线程准备原生 XUI、注册配置监听、定时向游戏线程派发 Tick。
#define NOMINMAX
#include <windows.h>

#include <string>

#include "GameNativeApi.h"
#include "NFConfig.h"
#include "NFAutoPickup.h"
#include "NFEquipProcessor.h"
#include "NFGameThread.h"
#include "NFHotkey.h"
#include "NFLog.h"
#include "NFMapMove.h"
#include "NFPacket.h"
#include "NFNativeUi.h"
#include "NFNotice.h"
#include "NFScore.h"
#include "NFGmMode.h"

namespace {

constexpr wchar_t kControllerClass[] = L"ClientPatchNFController";
constexpr wchar_t kPluginFolderName[] = L"NF";
constexpr wchar_t kLayoutFileName[] = L"NF.xui";
constexpr wchar_t kLayoutCacheName[] = L"NF_cache.xui";
// 客户端 PVF 的资源路径按大小写查找；现有 XUI 目录使用小写。
constexpr wchar_t kLayoutResourcePath[] = L"ui/nf.xui";
constexpr wchar_t kWindowIdOwner[] = L"ClientPatch.NF";
constexpr UINT kControllerTimerId = 1;

HMODULE g_module = nullptr;
std::wstring g_module_directory;
LONG g_started = 0;
HWND g_controller_window = nullptr;
ClientPatchGameNativeApi g_native = {};
bool g_native_bound = false;
bool g_external_layout_mounted = false;
bool g_window_ids_reserved = false;
bool g_window_factory_registered = false;
int g_reserved_window_id = -1;
bool g_hotkeys_installed = false;
bool g_auto_grade_enabled = false;
bool g_load_notice_sent = false;

std::wstring ModuleDirectory(HMODULE module) {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(module, path, _countof(path));
  if (!length || length >= _countof(path)) return L".";
  std::wstring result(path, length);
  const size_t slash = result.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

bool EnsureGameNative() {
  if (g_native_bound) return true;
  g_native_bound = ClientPatchBindGameNative(&g_native);
  return g_native_bound;
}

// 给 GameNative 的加载公告挑一个颜色（和换装同款：浅蓝调，失败兜底 0）。
INT NoticeColor() {
  return EnsureGameNative() && g_native.chatRgb ? g_native.chatRgb(155, 200, 230) : 0;
}

// 游戏线程首 tick 触发一次：换装同款的 GameNative "插件已载入"公告。
// postLoadNotice（游戏顶部加载通知）优先；不支持就退 postChatNotice（聊天喇叭）。
void RequestLoadNotice() {
  if (g_load_notice_sent) return;
  if (!EnsureGameNative()) return;
  if (g_native.postLoadNotice)
    g_native.postLoadNotice(L"DouBi内辅插件已载入", NoticeColor());
  else if (g_native.postChatNotice)
    g_native.postChatNotice(L"DouBi内辅插件已载入", NoticeColor());
  else
    return;
  g_load_notice_sent = true;
  nflog::Write(L"[公告] 已交 GameNative 载入公告");
}

// ---------------------------------------------------------------------------
// 界面状态构建（任意线程）
// ---------------------------------------------------------------------------

nf_ui::UiState BuildUiState() {
  const auto cfg = config::Get();
  nf_ui::UiState state;

  state.filterIdsText =
      L"过滤ID：" +
      (cfg.filter_ids.empty() ? L"(全部)" : cfg.filter_ids);
  state.nameFilterText =
      L"名称过滤：" +
      (cfg.name_filter.empty() ? L"(无)" : cfg.name_filter);

  state.pickupEnabled = pickup::Running();
  state.pickupMode = cfg.pickup_mode;
  state.filterMode = cfg.filter_mode;
  state.moveMode = cfg.move_mode;
  for (int q = 0; q < config::kQualityCount; ++q)
    state.equipActions[q] = cfg.equip_action[q];
  state.autoGradeEnabled = g_auto_grade_enabled;
  state.gmEnabled = gm::Enabled();


  // 直接在状态栏看文本。
  const wchar_t* kPickupModeStr[] = {L"吸物", L"发包"};
  const wchar_t* kFilterModeStr[]  = {L"黑名单", L"白名单"};
  const wchar_t* kMoveModeStr[]    = {L"坐标", L"强制"};
  const wchar_t* kQualityName[]    = {L"白装", L"蓝装", L"紫装", L"粉装", L"史诗", L"异界"};
  const wchar_t* kActionStr[]      = {L"保留", L"卖物", L"分解"};
  std::wstring cfgLine;
  {
    wchar_t tmp[512] = {0};
    _snwprintf_s(tmp, _TRUNCATE,
      L"当前配置：拾取：%s｜评分：%s｜GM：%s｜模式：%s｜过滤：%s｜顺图：%s\r\n装备品级：",
      state.pickupEnabled ? L"开" : L"关",
      state.autoGradeEnabled ? L"开" : L"关",
      state.gmEnabled ? L"开" : L"关",
      (state.pickupMode >= 0 && state.pickupMode < _countof(kPickupModeStr))
        ? kPickupModeStr[state.pickupMode] : L"?",
      (state.filterMode >= 0 && state.filterMode < _countof(kFilterModeStr))
        ? kFilterModeStr[state.filterMode] : L"?",
      (state.moveMode   >= 0 && state.moveMode   < _countof(kMoveModeStr))
        ? kMoveModeStr[state.moveMode] : L"?");
    cfgLine = tmp;
  }
  for (int q = 0; q < config::kQualityCount; ++q) {
    const int a = state.equipActions[q];
    const wchar_t* as = L"?";
    if (a >= 0 && a < _countof(kActionStr)) as = kActionStr[a];
    cfgLine += kQualityName[q];
    cfgLine += L"=";
    cfgLine += as;
    if (q + 1 < config::kQualityCount) cfgLine += L" ";
  }

  const equip::Result r = equip::LastResult();
  if (r.busy) {
    state.statusText = cfgLine + L"\r\n[正在处理装备...]";
  } else if (r.scanned > 0) {
    int sold = 0, disas = 0;
    for (int q = 0; q < config::kQualityCount; ++q) {
      sold += r.sold[q];
      disas += r.disassembled[q];
    }
    wchar_t buf[128] = {0};
    _snwprintf_s(buf, _TRUNCATE,
                 r.aborted
                     ? L"上次处理中止：扫描 %d，已卖 %d，已分解 %d"
                     : L"上次处理：扫描 %d，卖 %d，分解 %d",
                 r.scanned, sold, disas);
    state.statusText = cfgLine + L"\r\n" + buf;
  } else {
    state.statusText = cfgLine + L"\r\n就绪";
  }
  return state;
}

void RefreshUi() { nf_ui::Refresh(BuildUiState()); }

// ---------------------------------------------------------------------------
// 面板命令（游戏线程回调）
// ---------------------------------------------------------------------------

void ApplyAndRefresh() {
  RefreshUi();
  if (EnsureGameNative() && g_native.refreshPluginClips)
    g_native.refreshPluginClips();
}

// ---------------------------------------------------------------------------
// 热键动作回调（在游戏线程同步调用——钩子绑在游戏线程的消息循环上）
// ---------------------------------------------------------------------------

void ReloadLayout();

void ReloadLayoutIfClosed() {
  if (!nf_ui::IsOpen())
    ReloadLayout();
}

void OnHotkeyAction(int action_id, int arg) {
  (void)arg;
  using namespace hotkey;
  switch (action_id) {
    case kToggleUi:
      gthread::Post([] { ReloadLayoutIfClosed(); nf_ui::Toggle(); RefreshUi(); });
      break;
    case kMapMoveLeft:
      gthread::Post([] { mapmove::Move(mapmove::kLeft); });
      break;
    case kMapMoveRight:
      gthread::Post([] { mapmove::Move(mapmove::kRight); });
      break;
    case kMapMoveUp:
      gthread::Post([] { mapmove::Move(mapmove::kUp); });
      break;
    case kMapMoveDown:
      gthread::Post([] { mapmove::Move(mapmove::kDown); });
      break;
    case kTogglePickup:
      gthread::Post([] {
        pickup::Toggle();
        notice::Send(pickup::Running() ? L"自动拾取已开启" : L"自动拾取已关闭");
        ApplyAndRefresh();
      });
      break;
    case kProcessEquip:
      gthread::Post([] { equip::ProcessAsync(); });
      break;
    case kToggleAutoGrade:
      gthread::Post([] {
        g_auto_grade_enabled = !g_auto_grade_enabled;
        notice::Send(g_auto_grade_enabled ? L"自动评分已开启" : L"自动评分已关闭");
        ApplyAndRefresh();
      });
      break;
  }
}

// ---------------------------------------------------------------------------
// 面板命令（游戏线程回调）
// ---------------------------------------------------------------------------

void HandlePanelCommand(nf_ui::Command command, int argument) {
  switch (command) {
    case nf_ui::Command::TogglePickup:
      pickup::Toggle();
      notice::Send(pickup::Running() ? L"自动拾取已开启"
                                     : L"自动拾取已关闭");
      ApplyAndRefresh();
      break;
    case nf_ui::Command::ToggleAutoGrade:
      g_auto_grade_enabled = !g_auto_grade_enabled;
      notice::Send(g_auto_grade_enabled ? L"自动评分已开启" : L"自动评分已关闭");
      ApplyAndRefresh();
      break;
    case nf_ui::Command::ToggleGmMode: {
      const bool on = gm::Toggle();
      notice::Send(on ? L"GM模式已开启" : L"GM模式已关闭");
      ApplyAndRefresh();
      break;
    }
    case nf_ui::Command::SetPickupMode: {
      config::Settings cfg = config::Get();
      cfg.pickup_mode = argument;
      config::Set(cfg);
      break;
    }
    case nf_ui::Command::SetFilterMode: {
      config::Settings cfg = config::Get();
      cfg.filter_mode = argument;
      config::Set(cfg);
      break;
    }
    case nf_ui::Command::SetMoveMode: {
      config::Settings cfg = config::Get();
      cfg.move_mode = argument;
      config::Set(cfg);
      break;
    }
    case nf_ui::Command::SetQualityAction: {
      const int quality = argument / 3;
      const int action = argument % 3;
      if (quality < 0 || quality >= config::kQualityCount) break;
      config::Settings cfg = config::Get();
      cfg.equip_action[quality] = action;
      config::Set(cfg);
      break;
    }
    case nf_ui::Command::ProcessEquip:
      equip::ProcessAsync();
      break;
    case nf_ui::Command::MapMove:
      mapmove::Move(static_cast<mapmove::Direction>(argument));
      break;
    case nf_ui::Command::Diagnostic:
      break;
  }
}

// ---------------------------------------------------------------------------
// 外部 XUI 准备（GameNative mount + 窗口 ID 预留）
// ---------------------------------------------------------------------------

void RollbackExternalUiPreparation() {
  if (g_window_factory_registered && g_native.unregisterWindowFactory)
    g_native.unregisterWindowFactory(kWindowIdOwner);
  nf_ui::Uninstall();
  if (g_external_layout_mounted && g_native.unmount)
    g_native.unmount(kLayoutResourcePath);
  if (g_window_ids_reserved && g_native.releaseWindowIds)
    g_native.releaseWindowIds(kWindowIdOwner);
  g_external_layout_mounted = false;
  g_window_ids_reserved = false;
  g_window_factory_registered = false;
}

// 把内置 NF_XUI 资源原样释放到临时文件供 GameNative mount 读取。
// 原样释放，不按配置改写布局：XUI 在首次 mount 时即被客户端解析并缓存，
// "释放后立马加载 根本来不及修改"，改写布局再加载的时序不可靠；
// 首开选中态交由 ApplyPendingState 在运行时以文本标签展示配置（见 NFNativeUi）。
bool WriteBuiltinXui(const std::wstring& path) {
  const HRSRC res = FindResourceW(g_module, L"NF_XUI", RT_RCDATA);
  if (!res)
    return false;
  const HGLOBAL global = LoadResource(g_module, res);
  if (!global)
    return false;
  const DWORD size = SizeofResource(g_module, res);
  const char* data = static_cast<const char*>(LockResource(global));
  if (!data || !size)
    return false;

  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  const bool ok = WriteFile(h, data, size, &written, nullptr) && written == size;
  CloseHandle(h);
  return ok;
}

bool PrepareExternalUi(const std::wstring& cache_path, int& window_id) {
  if (!EnsureGameNative() || !g_native.mount || !g_native.unmount ||
      !g_native.reserveWindowIds || !g_native.releaseWindowIds ||
      !g_native.registerWindowFactory || !g_native.unregisterWindowFactory)
    return false;

  // 内置 XUI 原样释放到临时文件（覆盖式写入，确保内容最新）。
  if (!WriteBuiltinXui(cache_path)) {
    nflog::Write(L"[原生界面] 内置布局释放失败：%s err=%lu",
                 cache_path.c_str(), GetLastError());
    return false;
  }
  nflog::Write(L"[原生界面] 内置布局已释放：%s", cache_path.c_str());

  if (!g_native.mount(kLayoutResourcePath, cache_path.c_str(),
                      CLIENT_PATCH_GAME_NATIVE_MOUNT_STRICT))
    return false;
  g_external_layout_mounted = true;

  int reserved = -1;
  if (!g_native.reserveWindowIds(kWindowIdOwner, 1,
                                 CLIENT_PATCH_WINDOW_ID_ABOVE_MENU,
                                 &reserved)) {
    RollbackExternalUiPreparation();
    return false;
  }
  g_window_ids_reserved = true;
  window_id = reserved;
  return true;
}

// 呼出前重挂载，强制下次解析读取磁盘最新 XUI（开发期热替换布局）。
// unmount+mount 替换挂载表条目；mount 表内仅登记映射，真正的读取发生在
// 客户端请求 ui/nf.xui 时走 HookXuiLoader，所以重挂载即可让新窗口读取新文件。
void ReloadLayout() {
  if (!EnsureGameNative() || !g_native.mount || !g_native.unmount ||
      !g_external_layout_mounted)
    return;

  const std::wstring cache_path = g_module_directory + L"\\" +
                                  kLayoutCacheName;
  // 重新原样释放布局（开发期热替换布局用，不按配置改写）。
  if (!WriteBuiltinXui(cache_path))
    nflog::Write(L"[原生界面] 重挂载前布局释放失败：%s", cache_path.c_str());
  g_native.unmount(kLayoutResourcePath);
  if (g_native.mount(kLayoutResourcePath, cache_path.c_str(),
                     CLIENT_PATCH_GAME_NATIVE_MOUNT_STRICT))
    nflog::Write(L"[原生界面] 重挂载完成：%s", cache_path.c_str());
  else
    nflog::Write(L"[原生界面] 重挂载失败：%s", cache_path.c_str());
}

// ---------------------------------------------------------------------------
// 控制线程
// ---------------------------------------------------------------------------

// 未处理异常捕获：进程崩溃时把异常码与出错地址写入 NF.log。
LONG WINAPI NfCrashFilter(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    const void* address = info->ExceptionRecord->ExceptionAddress;
    wchar_t module_name[MAX_PATH] = L"?";
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module) &&
        module) {
      GetModuleFileNameW(module, module_name, MAX_PATH);
    }
    const size_t slash = wcslen(module_name);
    const wchar_t* base = module_name;
    for (size_t i = 0; i < slash; ++i)
      if (module_name[i] == L'\\') base = module_name + i + 1;
    nflog::Write(L"!! 崩溃 code=0x%08X addr=%p module=%s tid=%lu",
                 info->ExceptionRecord->ExceptionCode, address, base,
                 GetCurrentThreadId());
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

LRESULT CALLBACK ControllerProc(HWND window, UINT message, WPARAM wParam,
                                LPARAM lParam) {
  static bool s_first_tick = true;
  static DWORD s_last_hook_retry = 0;
  static DWORD s_last_hotkey_retry = 0;
  switch (message) {
    case WM_TIMER:
      if (wParam == kControllerTimerId) {
        // 钩子在插件加载时游戏窗口可能尚未创建，此处周期重试。
        if (!gthread::Installed()) {
          const DWORD now = GetTickCount();
          if (now - s_last_hook_retry >= 2000) {
            s_last_hook_retry = now;
            if (gthread::Install(g_module))
              nflog::Write(L"[线程] 游戏线程钩子重试成功");
          }
        }
        // 游戏内生效的热键钩子（WH_GETMESSAGE）：等游戏窗口句柄出现后再挂。
        if (!g_hotkeys_installed) {
          const DWORD now = GetTickCount();
          if (now - s_last_hotkey_retry >= 2000) {
            s_last_hotkey_retry = now;
            if (hotkey::Install(g_module, 0, &OnHotkeyAction)) {
              g_hotkeys_installed = true;
              nflog::Write(L"[热键] 游戏窗口钩子安装成功");
            }
          }
        }
        if (s_first_tick && gthread::Installed()) {
          s_first_tick = false;
          nflog::Write(L"[线程] 首个 Tick 已在游戏线程执行");
          gthread::Post([] {
            nflog::Write(L"[线程] 游戏线程泵送确认 tid=%lu",
                         GetCurrentThreadId());
            RequestLoadNotice();
          });
        }
        // 周期 Tick：驱动界面轮询与状态刷新。
        gthread::Post([] {
          nf_ui::Poll();
          equip::Tick();
          RefreshUi();
          grade::Tick(g_auto_grade_enabled);
        });
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

DWORD WINAPI ControllerThreadProc(LPVOID) {
    //  nflog::Write(L"[启动] 控制线程开始 tid=%lu", GetCurrentThreadId());
  config::Initialize(g_module);
  // 调试开关立即生效，并且 RegisterListener 里保持实时同步。
  nflog::SetEnabled(config::Get().debug_enabled != 0);
  nflog::Write(L"[配置] 初始化完成：%s", config::IniPath().c_str());

  // 配置热重载 -> 拾取参数 + 日志开关 + 界面同步。
  config::RegisterListener([](const config::Settings& s) {
    pickup::OnConfigChanged(s);
    nflog::SetEnabled(s.debug_enabled != 0);
    RefreshUi();
  });
  notice::Bind();
  nflog::Write(L"[公告] GameNative 绑定：%s", notice::Available() ? L"成功" : L"失败");

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = g_module;
  window_class.lpfnWndProc = ControllerProc;
  window_class.lpszClassName = kControllerClass;
  RegisterClassExW(&window_class);

  HWND controller = CreateWindowExW(0, kControllerClass, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, g_module, nullptr);
  if (!controller) {
    nflog::Write(L"[启动] 控制窗口创建失败 err=%lu", GetLastError());
    config::Shutdown();
    return 0;
  }
  g_controller_window = controller;

  // 部署原生 XUI：内置布局释放到 <DLL目录>\NF_cache.xui 供 GameNative mount。
  const std::wstring layout_path = g_module_directory + L"\\" +
                                   kLayoutCacheName;
  int window_id = -1;
  nflog::Write(L"[原生界面] 布局文件：%s", layout_path.c_str());
  if (PrepareExternalUi(layout_path, window_id) &&
      nf_ui::Install(kLayoutResourcePath, window_id,
                     &HandlePanelCommand) &&
      g_native.registerWindowFactory(kWindowIdOwner, &nf_ui::WindowFactory,
                                     nullptr)) {
    g_window_factory_registered = true;
    g_reserved_window_id = window_id;
    nflog::Write(L"[原生界面] 安装成功 windowId=%d", window_id);
  } else {
    nflog::Write(L"[原生界面] 安装失败（mount=%d id=%d 已回滚）",
                 g_external_layout_mounted ? 1 : 0, window_id);
    RollbackExternalUiPreparation();
  }

  if (gthread::Install(g_module))
    nflog::Write(L"[线程] 游戏线程钩子首装成功");
  else
    nflog::Write(L"[线程] 游戏线程钩子首装失败（窗口未创建，稍后重试）");

  // 游戏内生效热键钩子：等窗口句柄就绪后再挂（WM_TIMER 里每 2 秒重试）。
  g_hotkeys_installed = false;
  if (hotkey::Install(g_module, 0, &OnHotkeyAction)) {
    g_hotkeys_installed = true;
    nflog::Write(L"[热键] 游戏窗口钩子安装成功");
  } else {
    nflog::Write(L"[热键] 热键钩子首装失败（窗口未创建，稍后重试）");
  }

  SetTimer(controller, kControllerTimerId, 100, nullptr);
  RefreshUi();
  nflog::Write(L"[启动] 控制线程就绪，进入消息循环");

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  hotkey::Shutdown();
  KillTimer(controller, kControllerTimerId);
  g_controller_window = nullptr;
  DestroyWindow(controller);

  pickup::Stop();
  gthread::Shutdown();
  if (g_window_factory_registered) {
    nf_ui::Close();
    RollbackExternalUiPreparation();
  }
  config::Shutdown();
  return 0;
}

}  // namespace

extern "C" __declspec(dllexport) BOOL ClientPatchPluginInit() {
  if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return TRUE;

  SetUnhandledExceptionFilter(NfCrashFilter);
  //nflog::Write(L"[启动] ClientPatchPluginInit");

  HANDLE thread = CreateThread(nullptr, 0, ControllerThreadProc, nullptr, 0,
                               nullptr);
  if (!thread) {
    nflog::Write(L"[启动] 控制线程创建失败 err=%lu", GetLastError());
    InterlockedExchange(&g_started, 0);
    return FALSE;
  }
  CloseHandle(thread);
  return TRUE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    g_module = module;
    g_module_directory = ModuleDirectory(module);
    nflog::Init(g_module_directory);
    //nflog::Write(L"[启动] NF.dll 加载");
  }
  return TRUE;
}
