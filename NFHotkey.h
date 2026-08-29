// NFHotkey.h - 游戏窗口内生效的热键（WH_GETMESSAGE + WH_CALLWNDPROC 双钩子）
// 不使用 RegisterHotKey，避免全局抢键（切到浏览器 F11/Alt+F 也被拦）。
// 参考 EquipmentSwap 的钩子链路，只拦截当前进程游戏主窗口及其子窗口的按键消息。
#pragma once

#include <windows.h>
#include <string>

namespace hotkey {

// 按键绑定：MOD_CONTROL / MOD_ALT / MOD_SHIFT / MOD_WIN 位或 + 虚拟键码。
struct HotkeyBinding
{
    UINT modifiers = 0;
    UINT virtualKey = 0;
    bool IsValid() const { return virtualKey != 0; }
};

// 触发回调：在游戏线程（钩子所在线程）的消息循环里同步调用。
using ActionFn = void (*)(int action_id, int argument);

enum ActionId : int
{
    kToggleUi = 0,       // F11
    kMapMoveLeft = 1,    // Alt+Left
    kMapMoveRight = 2,   // Alt+Right
    kMapMoveUp = 3,      // Alt+Up
    kMapMoveDown = 4,    // Alt+Down
    kTogglePickup = 5,   // Alt+Q
    kProcessEquip = 6,   // Alt+F
    kToggleAutoGrade = 7, // Alt+R
};

// 热键集合（对应 [热键] 段的 8 个配置键）。
struct Hotkeys
{
    HotkeyBinding toggle_ui;
    HotkeyBinding move_left;
    HotkeyBinding move_right;
    HotkeyBinding move_up;
    HotkeyBinding move_down;
    HotkeyBinding toggle_pickup;
    HotkeyBinding process_equip;
    HotkeyBinding toggle_auto_grade;
};

// 安装钩子：传入 DLL 模块句柄 + 游戏窗口线程 ID + 动作回调（在游戏线程同步调用）。
// game_thread_id 为 0 时，内部通过 EnumWindows 按当前进程 ID 自动找游戏主窗口。
bool Install(HMODULE module, DWORD game_thread_id, ActionFn on_action);

// 卸载钩子（DLL 退出前必须调用）。
void Shutdown();

// 解析热键串："Alt+F11"/"Ctrl+Q"/"F1"/"Alt+Up"。失败时 valid=false。
HotkeyBinding Parse(const std::wstring& text, bool& valid);

// 读取 [热键] 段的 8 个键。ini_path 为空则按模块目录/NF.ini 回退。
Hotkeys LoadFromIni(const std::wstring& ini_path = L"");

// 只在钩子回调或测试里用：命中热键即返回 true 并消费消息。
bool TryHandleMessage(MSG* message, bool is_getmessage);

}  // namespace hotkey
