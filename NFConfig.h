// NFConfig.h - 配置读写与热重载
// NF.ini 放在 DLL 自身目录（ModuleDirectory(module)），UTF-16LE BOM。
// 热重载: ReadDirectoryChangesW 监视 + RegisterListener 实时生效。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

namespace config {

// 装备处理方式
enum EquipAction : int {
  kKeep = 0,        // 保留
  kSell = 1,        // 卖商店
  kDisassemble = 2  // 分解
};

// 品质索引（对应内存中装备属性值 0-5）
enum Quality : int {
  kWhite = 0,
  kBlue = 1,
  kPurple = 2,
  kPink = 3,
  kEpic = 4,
  kOtherworld = 5,
  kQualityCount = 6
};

struct Settings {
  // [自动拾取]
  int pickup_mode = 0;    // 0=吸物 1=发包
  int filter_mode = 0;    // 0=黑名单 1=白名单
  std::wstring filter_ids;  // 物品ID列表，逗号分隔
  int pickup_interval = 500;  // 毫秒

  // [顺图]
  int move_mode = 0;  // 0=坐标顺图 1=强制顺图

  // [装备处理]
  int equip_action[kQualityCount] = {1, 2, 2, 0, 0, 0};  // 默认: 白=卖 蓝/紫=分解 粉/史诗/异界=保留
  std::wstring name_filter;  // 多个名称用 - 分隔

  // [热键] —— 字符串形式直接存 NF.ini，方便用户编辑；首次加载写回补齐缺项。
  std::wstring hotkey_toggle_ui   = L"F11";
  std::wstring hotkey_move_up     = L"Alt+Up";
  std::wstring hotkey_move_down   = L"Alt+Down";
  std::wstring hotkey_move_left   = L"Alt+Left";
  std::wstring hotkey_move_right  = L"Alt+Right";
  std::wstring hotkey_toggle_pickup = L"Alt+Q";
  std::wstring hotkey_process_equip = L"Alt+F";
  std::wstring hotkey_auto_grade  = L"Alt+R";  // 自动评分开关

  // [调试]
  int debug_enabled = 0;  // 1=写NF.log；0=关闭所有日志写

  // 解析后的过滤ID列表
  std::vector<int32_t> ParsedFilterIds() const;
  // 解析后的名称过滤列表
  std::vector<std::wstring> ParsedNameFilters() const;
};

// 初始化：传入 DLL module 以确定 ini 路径，加载配置、启动监控线程。
void Initialize(HMODULE module);

// 停止监控线程。
void Shutdown();

// 获取当前配置的只读引用（线程安全的值拷贝）。
Settings Get();

// 覆盖配置并立即写盘（界面修改时调用）。
void Set(const Settings& s);

// 从文件重新加载。
void Reload();

// 写入文件。
void Save();

// 注册配置变更回调（热重载 / 界面修改后触发）。
void RegisterListener(std::function<void(const Settings&)> cb);

// ini 完整路径。
const std::wstring& IniPath();

}  // namespace config
