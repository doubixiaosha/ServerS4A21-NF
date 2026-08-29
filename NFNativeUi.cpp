// NFNativeUi.cpp - 原生 XUI 配置面板
// 移植自 EquipmentSwapNativeUi.cpp（同 S4A21 客户端地址），
// 去掉物品格子 / 页签 / 快捷键逻辑，保留窗口工厂与文本控件写入。
#define NOMINMAX
#include "NFNativeUi.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "NFLog.h"

namespace nf_ui {
namespace {
constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr uintptr_t kAllocateAddress = 0x027C42E0;
constexpr uintptr_t kWideStringConstructorAddress = 0x0042B800;
// wstring::assign(const wchar_t*, size)。42B800 构造成功后走这里写入内容。
constexpr uintptr_t kWideStringAssignAddress = 0x0042ACB0;
constexpr uintptr_t kWindowConstructorAddress = 0x01A26E80;
constexpr uintptr_t kControlLookupAddress = 0x01A26BB0;
constexpr uintptr_t kWindowManagerGlobalAddress = 0x03A5C9B8;
// 02304C70：OpenWindow thunk，thiscall(manager, windowId, 0, 0)。
constexpr uintptr_t kOpenWindowAddress = 0x02304C70;
constexpr uintptr_t kCloseWindowAddress = 0x02304790;
constexpr uintptr_t kHasWindowAddress = 0x02301820;
constexpr uintptr_t kBaseEventVtableAddress = 0x032F9D24;
constexpr size_t kWindowSize = 0x1A8;
constexpr size_t kEventVtableOffset = 0x17C;
constexpr int kMaxWindowId = 838;
constexpr int kButtonClickEvent = 13;

// 控件 ID（与 NF.xui 布局一致）。按钮只接收点击事件，文字静态；
// 动态状态一律写 CNUIControlText 标签。
// CNRadioButton 内的 CNCheckBox 也走 kButtonClickEvent，ID 即选项索引。
constexpr int kPickupButton = 10;
constexpr int kAutoGradeButton = 11;
constexpr int kGmButton = 12;
constexpr int kProcessButton = 13;
constexpr int kMapMoveFirst = 20;      // 20左 21右 22上 23下
// 单选选项 ID（CNCheckBox inside CNRadioButton）
constexpr int kPickupModeRadioFirst = 60;   // 60吸物 61发包
constexpr int kFilterModeRadioFirst = 62;   // 62黑名单 63白名单
constexpr int kMoveModeRadioFirst = 64;     // 64坐标 65强制
constexpr int kQualityRadioFirst = 70;      // 70-87: 6品质×3动作(保留/卖/分)
constexpr int kQualityRadioCount = 18;
constexpr int kFilterIdsLabel = 51;
constexpr int kNameFilterLabel = 52;
constexpr int kStatusLabel = 50;

// A21 CNUIControlText 虚表 0x0360881C。槽 54 是 0 参 getter，
// 按钮等控件写字走虚表 +220（槽 55）；显示字符串在 this+0x110。
constexpr uintptr_t kControlTextVtableAddress = 0x0360881C;
constexpr uintptr_t kControlTextDrawAddress = 0x027CC060;
constexpr uintptr_t kTextLayoutVtableAddress = 0x0360FDBC;
constexpr uintptr_t kTextLayoutDtorAddress = 0x0282B8A0;
constexpr size_t kControlTextDrawSlot = 9;
constexpr size_t kControlTextWriteSlot = 55;
constexpr size_t kControlTextStringOffset = 0x110;
constexpr size_t kControlTextLayoutOffset = 0xE8;

using AllocateFn = void* (__cdecl*)(size_t size);
using WideStringConstructorFn = void* (__thiscall*)(void* value,
    const wchar_t* text);
using WideStringAssignFn = void* (__thiscall*)(void* value,
    const wchar_t* text, unsigned int length);
using WindowConstructorFn = void* (__thiscall*)(void* window, void* manager,
    int windowId, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3,
    uint32_t p4, uint32_t p5, uint32_t p6, uint32_t p7);
using OpenWindowFn = void* (__thiscall*)(void* manager, int windowId,
    int argument, int flag);
using CloseWindowFn = int(__thiscall*)(void* manager, int windowId,
    int reason, int argument);
using HasWindowFn = bool(__thiscall*)(void* manager, int windowId);
using ControlLookupFn = void* (__thiscall*)(void* window, void* result,
    int controlId);
using ControlTextWriteFn = int(__thiscall*)(void* control,
    const wchar_t* text);

CommandHandler g_handler = nullptr;
std::wstring g_layoutPath;
int g_windowId = -1;
void* g_window = nullptr;
// 工厂返回值走全局，避免 Release + __try 把 EAX 弄成 EXCEPTION_EXECUTE_HANDLER(1)。
void* g_factoryWindow = nullptr;
void* g_eventVtable[1] = {};
bool g_installed = false;
bool g_textAssignReady = false;
UiState g_pendingState;
bool g_hasPendingState = false;
UiState g_appliedState;

struct ClientSharedPtr
{
    void* object = nullptr;
    void* controlBlock = nullptr;
};

bool GetControl(int controlId, ClientSharedPtr& result);
void Dispatch(Command command, int argument);

uintptr_t ClientAddress(uintptr_t preferredAddress)
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base + (preferredAddress - kPreferredImageBase);
}

bool MatchesVtable(void* object, uintptr_t vtableAddress,
    size_t slot, uintptr_t functionAddress)
{
    if (!object)
        return false;
    __try
    {
        auto** vtable = *reinterpret_cast<void***>(object);
        return reinterpret_cast<uintptr_t>(vtable) ==
                ClientAddress(vtableAddress) &&
            reinterpret_cast<uintptr_t>(vtable[slot]) ==
                ClientAddress(functionAddress);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

const wchar_t* ClientWideCStr(void* value)
{
    if (!value)
        return L"";
    auto* fields = static_cast<uint32_t*>(value);
    if (fields[6] < 8)
        return reinterpret_cast<const wchar_t*>(fields + 1);
    return reinterpret_cast<const wchar_t*>(fields[1]);
}

bool IsClientCodeAddress(const void* pointer)
{
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return address >= base + 0x1000 && address < base + 0x03C00000;
}

bool LooksLikeThiscallOneArg(const void* pointer)
{
    if (!IsClientCodeAddress(pointer))
        return false;
    bool matched = false;
    __try
    {
        const auto* bytes = static_cast<const unsigned char*>(pointer);
        if (bytes[0] != 0x55 || bytes[1] != 0x8B || bytes[2] != 0xEC)
            return false;
        for (int index = 3; index < 96; ++index)
        {
            if (bytes[index] == 0xC2 && bytes[index + 1] == 0x04 &&
                bytes[index + 2] == 0x00)
            {
                matched = true;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        matched = false;
    }
    return matched;
}

void ReleaseClientSharedPtr(ClientSharedPtr& value)
{
    void* block = value.controlBlock;
    value.object = nullptr;
    value.controlBlock = nullptr;
    if (!block)
        return;
    __try
    {
        auto* uses = reinterpret_cast<volatile long*>(
            static_cast<unsigned char*>(block) + 4);
        if (InterlockedDecrement(uses) == 0)
        {
            auto** vtable = *reinterpret_cast<void***>(block);
            reinterpret_cast<void(__thiscall*)(void*)>(vtable[0])(block);
            auto* weaks = reinterpret_cast<volatile long*>(
                static_cast<unsigned char*>(block) + 8);
            if (InterlockedDecrement(weaks) == 0)
                reinterpret_cast<void(__thiscall*)(void*)>(vtable[1])(block);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void ResetAppliedState()
{
    g_appliedState = {};
}

bool GetControl(int controlId, ClientSharedPtr& result)
{
    result = {};
    if (!g_window)
        return false;
    __try
    {
        auto lookup = reinterpret_cast<ControlLookupFn>(
            ClientAddress(kControlLookupAddress));
        lookup(g_window, &result, controlId);
        return result.object != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = {};
        return false;
    }
}

bool SetControlText(int controlId, const std::wstring& text)
{
    ClientSharedPtr control;
    if (!GetControl(controlId, control) || !control.object)
        return false;

    bool updated = false;
    __try
    {
        auto* bytes = static_cast<unsigned char*>(control.object);
        auto** vtable = *reinterpret_cast<void***>(control.object);
        const bool isTextControl = MatchesVtable(control.object,
            kControlTextVtableAddress, kControlTextDrawSlot,
            kControlTextDrawAddress);

        if (isTextControl && g_textAssignReady)
        {
            auto assign = reinterpret_cast<WideStringAssignFn>(
                ClientAddress(kWideStringAssignAddress));
            void* value = bytes + kControlTextStringOffset;
            assign(value, text.c_str(),
                static_cast<unsigned int>(text.size()));
            void* layout = bytes + kControlTextLayoutOffset;
            if (MatchesVtable(layout, kTextLayoutVtableAddress, 0,
                    kTextLayoutDtorAddress))
            {
                *reinterpret_cast<const wchar_t**>(
                    static_cast<unsigned char*>(layout) + 8) =
                    ClientWideCStr(value);
                *reinterpret_cast<uint32_t*>(
                    static_cast<unsigned char*>(layout) + 12) = 1;
            }
            if (*reinterpret_cast<void**>(bytes + 0x350))
                Dispatch(Command::Diagnostic, 2000 + controlId);
            if (reinterpret_cast<uintptr_t>(vtable[13]) ==
                ClientAddress(0x027C9200))
            {
                reinterpret_cast<void(__thiscall*)(void*)>(
                    vtable[13])(control.object);
            }
            updated = true;
        }
        else
        {
            void* write = vtable[kControlTextWriteSlot];
            if (LooksLikeThiscallOneArg(write))
            {
                auto setText = reinterpret_cast<ControlTextWriteFn>(write);
                setText(control.object, text.c_str());
                updated = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        updated = false;
    }
    ReleaseClientSharedPtr(control);
    return updated;
}

// 勾选/选中状态偏移 (两会话对比 + 点击对比定位):
// 0x3B8 = 点击/活动标志, 点击时客户端原生翻转;
// 0x4F0 = 绘制状态, 解析期由 IsCheck/checkedButtonIndex 写入,
//         重开面板不重新解析, 必须每次按配置直写才能驱动视觉。
constexpr size_t kCheckStateOffset = 0x3B8;
constexpr size_t kCheckDrawOffset = 0x4F0;

void WriteCheckState(int controlId, bool checked)
{
    ClientSharedPtr control;
    if (!GetControl(controlId, control) || !control.object)
    {
        ReleaseClientSharedPtr(control);
        return;
    }
    __try
    {
        unsigned char* bytes =
            static_cast<unsigned char*>(control.object);
        bytes[kCheckStateOffset] = checked ? 1 : 0;
        bytes[kCheckDrawOffset] = checked ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    ReleaseClientSharedPtr(control);
}

// 按当前配置同步勾选框与单选组的选中态 (每次面板刷新都会执行, 幂等)。
void SyncControlStates(const UiState& state)
{
    WriteCheckState(kPickupButton, state.pickupEnabled);
    WriteCheckState(kAutoGradeButton, state.autoGradeEnabled);
    WriteCheckState(kGmButton, state.gmEnabled);  // 保留 GM 模式开关

    if (state.pickupMode == 0 || state.pickupMode == 1)
    {
        WriteCheckState(kPickupModeRadioFirst, state.pickupMode == 0);
        WriteCheckState(kPickupModeRadioFirst + 1,
            state.pickupMode == 1);
    }
    if (state.filterMode == 0 || state.filterMode == 1)
    {
        WriteCheckState(kFilterModeRadioFirst, state.filterMode == 0);
        WriteCheckState(kFilterModeRadioFirst + 1,
            state.filterMode == 1);
    }
    if (state.moveMode == 0 || state.moveMode == 1)
    {
        WriteCheckState(kMoveModeRadioFirst, state.moveMode == 0);
        WriteCheckState(kMoveModeRadioFirst + 1, state.moveMode == 1);
    }
    for (int q = 0; q < 6; ++q)
    {
        if (state.equipActions[q] < 0 || state.equipActions[q] > 2)
            continue;
        for (int a = 0; a < 3; ++a)
            WriteCheckState(kQualityRadioFirst + q * 3 + a,
                state.equipActions[q] == a);
    }
}

// 逐项刷新文本（单选/勾选视觉不强制同步，CN 控件自己会记上次点的值；
// 配置真实值在 statusText 文本里可查看）。
int ApplyPendingState()
{
    if (!g_window || !g_hasPendingState)
        return 0;

    const UiState& want = g_pendingState;
    UiState& got = g_appliedState;

    struct TextItem
    {
        const std::wstring* next;
        std::wstring* current;
        int controlId;
    };
    TextItem items[] = {
        {&want.filterIdsText, &got.filterIdsText, kFilterIdsLabel},
        {&want.nameFilterText, &got.nameFilterText, kNameFilterLabel},
        {&want.statusText, &got.statusText, kStatusLabel},
    };
    int failed = 0;
    for (TextItem& item : items)
    {
        if (*item.next == *item.current)
            continue;
        if (SetControlText(item.controlId, *item.next))
            *item.current = *item.next;
        else
            ++failed;
    }

    // 勾选/单选状态直写 (选中态在控件 +0x3B8, 布局缓存不重新解析,
    // 每次刷新都按配置重写, 保证视觉与配置一致)。
    SyncControlStates(want);

    g_hasPendingState = failed != 0;
    return failed;
}

bool IsPlausibleWindow(void* window)
{
    return reinterpret_cast<uintptr_t>(window) >= 0x10000;
}

bool ValidateBytes(uintptr_t preferredAddress,
    const unsigned char* expected, size_t length)
{
    const auto* address = reinterpret_cast<const unsigned char*>(
        ClientAddress(preferredAddress));
    __try
    {
        for (size_t index = 0; index < length; ++index)
        {
            if (address[index] != expected[index])
                return false;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void* WindowManager()
{
    __try
    {
        return *reinterpret_cast<void**>(
            ClientAddress(kWindowManagerGlobalAddress));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void Dispatch(Command command, int argument = 0)
{
    if (g_handler)
        g_handler(command, argument);
}

int __fastcall HandleControlEvent(void*, void*, int controlId, int eventType,
    int argument, int)
{
    if (eventType != kButtonClickEvent)
        return 0;

    switch (controlId)
    {
    case kPickupButton:
        Dispatch(Command::TogglePickup);
        break;
    case kAutoGradeButton:
        Dispatch(Command::ToggleAutoGrade);
        break;
    case kGmButton:
        Dispatch(Command::ToggleGmMode);
        break;
    case kProcessButton:
        Dispatch(Command::ProcessEquip);
        break;
    default:
        break;
    }

    if (controlId >= kMapMoveFirst && controlId <= kMapMoveFirst + 3)
    {
        Dispatch(Command::MapMove, controlId - kMapMoveFirst);
        return 0;
    }
    // 拾取模式单选 60-61
    if (controlId >= kPickupModeRadioFirst &&
        controlId <= kPickupModeRadioFirst + 1)
    {
        Dispatch(Command::SetPickupMode,
                 controlId - kPickupModeRadioFirst);
        return 0;
    }
    // 过滤模式单选 62-63
    if (controlId >= kFilterModeRadioFirst &&
        controlId <= kFilterModeRadioFirst + 1)
    {
        Dispatch(Command::SetFilterMode,
                 controlId - kFilterModeRadioFirst);
        return 0;
    }
    // 顺图模式单选 64-65
    if (controlId >= kMoveModeRadioFirst &&
        controlId <= kMoveModeRadioFirst + 1)
    {
        Dispatch(Command::SetMoveMode,
                 controlId - kMoveModeRadioFirst);
        return 0;
    }
    // 品质处理单选 70-87
    if (controlId >= kQualityRadioFirst &&
        controlId < kQualityRadioFirst + kQualityRadioCount)
    {
        Dispatch(Command::SetQualityAction,
                 controlId - kQualityRadioFirst);
        return 0;
    }
    return 0;
}

void* CreatePluginWindow(void* manager)
{
    auto allocate = reinterpret_cast<AllocateFn>(
        ClientAddress(kAllocateAddress));
    auto constructString = reinterpret_cast<WideStringConstructorFn>(
        ClientAddress(kWideStringConstructorAddress));
    auto constructWindow = reinterpret_cast<WindowConstructorFn>(
        ClientAddress(kWindowConstructorAddress));

    void* window = allocate(kWindowSize);
    if (!window)
    {
        Dispatch(Command::Diagnostic, 2);
        return nullptr;
    }

    std::array<uint32_t, 8> layoutString = {};
    constructString(layoutString.data(), g_layoutPath.c_str());
    void* ctorResult = constructWindow(window, manager, g_windowId,
        layoutString[0], layoutString[1], layoutString[2], layoutString[3],
        layoutString[4], layoutString[5], layoutString[6], layoutString[7]);
    nflog::Write(L"[原生界面] 窗口构造返回 %p（this=%p 布局=%s）",
        ctorResult, window, g_layoutPath.c_str());

    auto* bytes = static_cast<unsigned char*>(window);
    *reinterpret_cast<void**>(bytes + kEventVtableOffset) = g_eventVtable;
    ResetAppliedState();
    g_window = window;
    g_factoryWindow = window;
    g_hasPendingState = true;
    ApplyPendingState();
    return window;
}

}  // namespace

void* __cdecl WindowFactory(int windowId, void* manager, void*)
{
    if (!g_installed || windowId != g_windowId)
        return nullptr;

    nflog::Write(L"[原生界面] 窗口工厂被调用 id=%d", windowId);
    g_factoryWindow = nullptr;
    DWORD innerCode = 0;
    __try {
        CreatePluginWindow(manager);
    } __except (innerCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        nflog::Write(L"[原生界面] 工厂内部SEH code=0x%08X", innerCode);
        g_factoryWindow = nullptr;
    }
    if (!g_factoryWindow)
        nflog::Write(L"[原生界面] 窗口构造失败");
    return g_factoryWindow;
}

bool Install(const wchar_t* layoutPath, int windowId,
    CommandHandler handler)
{
    if (g_installed)
        return windowId == g_windowId;
    if (!layoutPath || !*layoutPath || windowId < 0 ||
        windowId > kMaxWindowId || !handler)
        return false;

    static constexpr unsigned char kAllocate[] = {
        0x55, 0x8B, 0xEC, 0x56, 0x57, 0x8B, 0x7D, 0x08,
    };
    static constexpr unsigned char kWideString[] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF,
    };
    static constexpr unsigned char kWindowCtor[] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF,
    };
    static constexpr unsigned char kGetControl[] = {
        0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x89, 0x84, 0x01, 0x00, 0x00,
    };
    static constexpr unsigned char kOpen[] = {
        0xE9,
    };
    static constexpr unsigned char kHas[] = {
        0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x3D, 0x8B, 0x03, 0x00, 0x00,
    };
    static constexpr unsigned char kBaseEvent[] = {
        0x33, 0xC0, 0xC2, 0x10, 0x00,
    };
    static constexpr unsigned char kWideStringAssign[] = {
        0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x4D, 0x08,
    };

    if (!ValidateBytes(kAllocateAddress, kAllocate, sizeof(kAllocate)) ||
        !ValidateBytes(kWideStringConstructorAddress, kWideString,
            sizeof(kWideString)) ||
        !ValidateBytes(kWindowConstructorAddress, kWindowCtor,
            sizeof(kWindowCtor)) ||
        !ValidateBytes(kControlLookupAddress, kGetControl,
            sizeof(kGetControl)) ||
        !ValidateBytes(kOpenWindowAddress, kOpen, sizeof(kOpen)) ||
        !ValidateBytes(kHasWindowAddress, kHas, sizeof(kHas)))
    {
        nflog::Write(L"[原生界面] 地址签名校验失败，未安装");
        return false;
    }

    bool handlerOk = false;
    __try
    {
        const auto handler = *reinterpret_cast<const unsigned char**>(
            ClientAddress(kBaseEventVtableAddress));
        handlerOk = handler &&
            handler[0] == kBaseEvent[0] && handler[1] == kBaseEvent[1] &&
            handler[2] == kBaseEvent[2] && handler[3] == kBaseEvent[3] &&
            handler[4] == kBaseEvent[4];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        handlerOk = false;
    }
    if (!handlerOk)
    {
        nflog::Write(L"[原生界面] 事件虚表校验失败，未安装");
        return false;
    }

    g_textAssignReady = ValidateBytes(kWideStringAssignAddress,
        kWideStringAssign, sizeof(kWideStringAssign));

    g_eventVtable[0] = reinterpret_cast<void*>(&HandleControlEvent);
    g_layoutPath = layoutPath;
    g_windowId = windowId;
    g_handler = handler;
    g_installed = true;
    return true;
}

void Uninstall()
{
    if (!g_installed || g_window)
        return;
    g_layoutPath.clear();
    g_windowId = -1;
    g_handler = nullptr;
    g_installed = false;
}

bool IsOpen()
{
    if (!g_installed)
        return false;
    void* manager = WindowManager();
    if (!manager)
        return false;
    __try
    {
        auto hasWindow = reinterpret_cast<HasWindowFn>(
            ClientAddress(kHasWindowAddress));
        const bool open = hasWindow(manager, g_windowId);
        if (!open)
        {
            ResetAppliedState();
            g_window = nullptr;
        }
        return open;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ResetAppliedState();
        g_window = nullptr;
        return false;
    }
}

void Poll()
{
    if (!g_installed || !g_window || !IsOpen())
        return;
    if (g_hasPendingState)
        ApplyPendingState();
}

void Refresh(const UiState& state)
{
    g_pendingState = state;
    g_hasPendingState = true;
    if (g_window)
        ApplyPendingState();
}

void Toggle()
{
    if (!g_installed)
        return;
    if (IsOpen())
    {
        Close();
        return;
    }

    void* manager = WindowManager();
    if (!manager)
    {
        nflog::Write(L"[原生界面] 打开失败：窗口管理器尚未就绪");
        Dispatch(Command::Diagnostic, 4);
        return;
    }
        __try
        {
            nflog::Write(L"[原生界面] 请求打开窗口 id=%d", g_windowId);
            auto openWindow = reinterpret_cast<OpenWindowFn>(
                ClientAddress(kOpenWindowAddress));
            void* opened = openWindow(manager, g_windowId, 0, 0);
            if (IsPlausibleWindow(opened))
                g_window = opened;
            else if (!IsPlausibleWindow(g_window))
                g_window = nullptr;
            int failed = -1;
            bool registered = false;
            if (g_window)
            {
                failed = ApplyPendingState();
                __try
                {
                    auto hasWindow = reinterpret_cast<HasWindowFn>(
                        ClientAddress(kHasWindowAddress));
                    registered = hasWindow(manager, g_windowId);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            nflog::Write(
                L"[原生界面] OpenWindow 返回 %p g_window=%p 注册=%d 控件刷新失败=%d",
                opened, g_window, registered ? 1 : 0, failed);
        }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        const DWORD code = GetExceptionCode();
        ResetAppliedState();
        g_window = nullptr;
        nflog::Write(L"[原生界面] 打开窗口异常（SEH 已拦截）code=0x%08X", code);
        Dispatch(Command::Diagnostic, 7);
    }
}

void Close()
{
    if (!g_installed)
        return;

    ResetAppliedState();
    void* manager = WindowManager();
    if (!manager)
    {
        g_window = nullptr;
        return;
    }
    __try
    {
        auto closeWindow = reinterpret_cast<CloseWindowFn>(
            ClientAddress(kCloseWindowAddress));
        closeWindow(manager, g_windowId, -1, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_window = nullptr;
    g_factoryWindow = nullptr;
}

}  // namespace nf_ui
