// NFHotkey.cpp - 游戏窗口内生效的热键（WH_GETMESSAGE + WH_CALLWNDPROC）
#define NOMINMAX
#include "NFHotkey.h"

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>

namespace hotkey {

namespace {

HMODULE g_module = nullptr;
DWORD g_game_pid = 0;
DWORD g_game_thread_id = 0;
HWND g_game_window = nullptr;
HHOOK g_callwndproc_hook = nullptr;
HHOOK g_getmessage_hook = nullptr;
ActionFn g_action = nullptr;
Hotkeys g_keys;

// ---------------------------------------------------------------------------
// 字符串工具（直接搬 EquipmentSwap）
// ---------------------------------------------------------------------------

std::wstring Trim(const std::wstring& value)
{
    size_t first = 0;
    while (first < value.size() && value[first] && iswspace(value[first]))
        ++first;
    size_t last = value.size();
    while (last > first && value[last - 1] && iswspace(value[last - 1]))
        --last;
    return value.substr(first, last - first);
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

UINT VirtualKeyFromToken(const std::wstring& token)
{
    if (token.size() == 1)
    {
        const wchar_t ch = token[0];
        if (ch >= L'A' && ch <= L'Z') return ch;
        if (ch >= L'0' && ch <= L'9') return ch;
        if (ch >= L'a' && ch <= L'z') return ch - L'a' + L'A';
    }
    if (token.size() == 2 && token[0] == L'F')
    {
        const int n = token[1] - L'0';
        if (n >= 1 && n <= 9) return VK_F1 + n - 1;
    }
    if (token.size() == 3 && token[0] == L'F')
    {
        const int n = (token[1] - L'0') * 10 + (token[2] - L'0');
        if (n >= 10 && n <= 24) return VK_F1 + n - 1;
    }
    if (EqualsIgnoreCase(token, L"Space")) return VK_SPACE;
    if (EqualsIgnoreCase(token, L"Enter") || EqualsIgnoreCase(token, L"Return")) return VK_RETURN;
    if (EqualsIgnoreCase(token, L"Tab")) return VK_TAB;
    if (EqualsIgnoreCase(token, L"Esc") || EqualsIgnoreCase(token, L"Escape")) return VK_ESCAPE;
    if (EqualsIgnoreCase(token, L"Insert")) return VK_INSERT;
    if (EqualsIgnoreCase(token, L"Delete") || EqualsIgnoreCase(token, L"Del")) return VK_DELETE;
    if (EqualsIgnoreCase(token, L"Home")) return VK_HOME;
    if (EqualsIgnoreCase(token, L"End")) return VK_END;
    if (EqualsIgnoreCase(token, L"PageUp")) return VK_PRIOR;
    if (EqualsIgnoreCase(token, L"PageDown")) return VK_NEXT;
    if (EqualsIgnoreCase(token, L"Up")) return VK_UP;
    if (EqualsIgnoreCase(token, L"Down")) return VK_DOWN;
    if (EqualsIgnoreCase(token, L"Left")) return VK_LEFT;
    if (EqualsIgnoreCase(token, L"Right")) return VK_RIGHT;
    return 0;
}

UINT CurrentModifierMask()
{
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= MOD_WIN;
    return m;
}

bool Matches(const HotkeyBinding& hotkey, UINT vk)
{
    return hotkey.IsValid() &&
        hotkey.virtualKey == vk &&
        hotkey.modifiers == CurrentModifierMask();
}

void Consume(MSG* msg)
{
    if (!msg) return;
    msg->message = WM_NULL;
    msg->wParam = 0;
    msg->lParam = 0;
}

bool IsGameInputWindow(HWND hwnd)
{
    return hwnd && g_game_window && IsWindow(g_game_window) &&
        (hwnd == g_game_window || IsChild(g_game_window, hwnd));
}

// ---------------------------------------------------------------------------
// EnumWindows：按当前进程 ID 找顶层可见的游戏主窗口
// ---------------------------------------------------------------------------

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid || pid != g_game_pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    // 跳过不可用窗口/弹出菜单/子窗口；挑一个 HWND_MESSAGE 之外的最顶层。
    if (GetParent(hwnd) != nullptr) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, _countof(cls));
    if (wcscmp(cls, L"#32770") == 0) return TRUE;   // 对话框跳过
    if (wcscmp(cls, L"static") == 0) return TRUE;
    g_game_window = hwnd;
    g_game_thread_id = tid;
    return FALSE;
}

bool ResolveGameWindow(DWORD forced_tid)
{
    g_game_pid = GetCurrentProcessId();
    if (forced_tid != 0)
    {
        g_game_thread_id = forced_tid;
        // 通过 tid 反查窗口
        EnumWindows([](HWND hwnd, LPARAM tid_ptr) -> BOOL
        {
            DWORD pid = 0;
            const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
            if (tid == *reinterpret_cast<DWORD*>(tid_ptr) && pid == g_game_pid &&
                IsWindowVisible(hwnd) && GetParent(hwnd) == nullptr)
            {
                wchar_t cls[64] = {};
                GetClassNameW(hwnd, cls, _countof(cls));
                if (wcscmp(cls, L"#32770") != 0)
                {
                    g_game_window = hwnd;
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&forced_tid));
    }
    else
    {
        EnumWindows(FindGameWindowProc, 0);
    }
    return g_game_window != nullptr;
}

// ---------------------------------------------------------------------------
// 钩子过程
// ---------------------------------------------------------------------------

bool TryHandleCore(MSG* msg)
{
    if (!msg || !g_action) return false;
    const UINT m = msg->message;
    if (m != WM_KEYDOWN && m != WM_SYSKEYDOWN) return false;
    // 跳过自动重复（lParam bit30 = 前一次键仍按下）
    if (msg->lParam & (1u << 30)) return false;
    if (!IsGameInputWindow(msg->hwnd)) return false;

    const UINT vk = static_cast<UINT>(msg->wParam);

    if (Matches(g_keys.toggle_ui, vk))
    {
        g_action(kToggleUi, 0);
        Consume(msg);
        return true;
    }
    if (Matches(g_keys.move_left, vk))
    {
        g_action(kMapMoveLeft, 0);
        Consume(msg);
        return true;
    }
    if (Matches(g_keys.move_right, vk))
    {
        g_action(kMapMoveRight, 0);
        Consume(msg);
        return true;
    }
    if (Matches(g_keys.move_up, vk))
    {
        g_action(kMapMoveUp, 0);
        Consume(msg);
        return true;
    }
    if (Matches(g_keys.move_down, vk))
    {
        g_action(kMapMoveDown, 0);
        Consume(msg);
        return true;
    }
    if (Matches(g_keys.toggle_pickup, vk))
    {
        g_action(kTogglePickup, 0);
        Consume(msg);
        return true;
    }
    if (Matches(g_keys.process_equip, vk))
    {
        g_action(kProcessEquip, 0);
        Consume(msg);
        return true;
    }
    if (Matches(g_keys.toggle_auto_grade, vk))
    {
        g_action(kToggleAutoGrade, 0);
        Consume(msg);
        return true;
    }
    return false;
}

LRESULT CALLBACK GameGetMessageHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam & PM_REMOVE)
    {
        MSG* msg = reinterpret_cast<MSG*>(lParam);
        TryHandleCore(msg);
    }
    return CallNextHookEx(g_getmessage_hook, code, wParam, lParam);
}

LRESULT CALLBACK GameCallWndProcHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        auto* cwp = reinterpret_cast<CWPSTRUCT*>(lParam);
        MSG msg = {};
        msg.hwnd = cwp->hwnd;
        msg.message = cwp->message;
        msg.wParam = cwp->wParam;
        msg.lParam = cwp->lParam;
        if (TryHandleCore(&msg))
        {
            // 发送侧：不回写 CWPSTRUCT（没法拦截已经在队里的 SendMessage，
            // 主要依赖 PM_REMOVE 钩子吃消息，这里作为辅助兜底检测）。
        }
    }
    return CallNextHookEx(g_callwndproc_hook, code, wParam, lParam);
}

// ---------------------------------------------------------------------------
// ini 辅助
// ---------------------------------------------------------------------------

std::wstring ModuleDir()
{
    wchar_t path[MAX_PATH] = {};
    DWORD len = g_module ? GetModuleFileNameW(g_module, path, _countof(path)) : 0;
    if (!len || len >= _countof(path)) return L".";
    std::wstring p(path, len);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring ReadIniStr(const std::wstring& ini, LPCWSTR sec, LPCWSTR key, LPCWSTR def)
{
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(sec, key, def, buf, _countof(buf), ini.c_str());
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------

HotkeyBinding Parse(const std::wstring& text, bool& valid)
{
    valid = false;
    HotkeyBinding result;
    const std::wstring value = Trim(text);
    if (value.empty() || EqualsIgnoreCase(value, L"None") ||
        EqualsIgnoreCase(value, L"无"))
    {
        valid = true;
        return result;
    }
    size_t start = 0;
    while (start <= value.size())
    {
        const size_t end = value.find(L'+', start);
        const std::wstring token = Trim(value.substr(start,
            end == std::wstring::npos ? std::wstring::npos : end - start));
        if (EqualsIgnoreCase(token, L"Ctrl") || EqualsIgnoreCase(token, L"Control"))
            result.modifiers |= MOD_CONTROL;
        else if (EqualsIgnoreCase(token, L"Alt"))
            result.modifiers |= MOD_ALT;
        else if (EqualsIgnoreCase(token, L"Shift"))
            result.modifiers |= MOD_SHIFT;
        else if (EqualsIgnoreCase(token, L"Win") || EqualsIgnoreCase(token, L"Windows"))
            result.modifiers |= MOD_WIN;
        else if (!result.virtualKey)
            result.virtualKey = VirtualKeyFromToken(token);
        else
            return result;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    valid = result.virtualKey != 0;
    return result;
}

Hotkeys LoadFromIni(const std::wstring& ini_path)
{
    const std::wstring ini = ini_path.empty() ? (ModuleDir() + L"\\NF.ini") : ini_path;
    Hotkeys h;
    bool ok = false;
    h.toggle_ui       = Parse(ReadIniStr(ini, L"热键", L"呼出界面",   L"F11"), ok);
    h.move_up         = Parse(ReadIniStr(ini, L"热键", L"顺图上",     L"Alt+Up"), ok);
    h.move_down       = Parse(ReadIniStr(ini, L"热键", L"顺图下",     L"Alt+Down"), ok);
    h.move_left       = Parse(ReadIniStr(ini, L"热键", L"顺图左",     L"Alt+Left"), ok);
    h.move_right      = Parse(ReadIniStr(ini, L"热键", L"顺图右",     L"Alt+Right"), ok);
    h.toggle_pickup   = Parse(ReadIniStr(ini, L"热键", L"拾取开关",   L"Alt+Q"), ok);
    h.process_equip   = Parse(ReadIniStr(ini, L"热键", L"一键处理",   L"Alt+F"), ok);
    h.toggle_auto_grade = Parse(ReadIniStr(ini, L"热键", L"自动评分", L"Alt+R"), ok);
    return h;
}

bool Install(HMODULE module, DWORD game_thread_id, ActionFn on_action)
{
    Shutdown();
    g_module = module;
    g_action = on_action;
    g_keys = LoadFromIni(L"");
    if (!g_action) return false;
    if (!ResolveGameWindow(game_thread_id)) return false;
    if (g_game_thread_id == 0) return false;

    g_callwndproc_hook = SetWindowsHookExW(WH_CALLWNDPROC, GameCallWndProcHook,
        g_module, g_game_thread_id);
    g_getmessage_hook = SetWindowsHookExW(WH_GETMESSAGE, GameGetMessageHook,
        g_module, g_game_thread_id);

    return g_callwndproc_hook && g_getmessage_hook;
}

void Shutdown()
{
    if (g_getmessage_hook)     { UnhookWindowsHookEx(g_getmessage_hook);     g_getmessage_hook = nullptr; }
    if (g_callwndproc_hook)    { UnhookWindowsHookEx(g_callwndproc_hook);    g_callwndproc_hook = nullptr; }
    g_game_window = nullptr;
    g_game_thread_id = 0;
    g_action = nullptr;
    g_keys = {};
}

bool TryHandleMessage(MSG* message, bool is_getmessage)
{
    (void)is_getmessage;
    return TryHandleCore(message);
}

}  // namespace hotkey
