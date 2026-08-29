# NF — DouBi 内辅插件

DNF S4A21 客户端内辅插件（`NF.dll`），由 GameGaurd 插件加载器加载，随游戏进程启动。
集成自动拾取、一键装备处理、顺图、自动评分四大功能，自带游戏原生 XUI 配置面板（F11 呼出）、游戏内热键与公告输出。

- 全部地址为 **S4A21 偏移**（见 `NFAddresses.h`），客户端更新后须在 IDA 重新标定。
- 所有游戏内存访问与 CALL 均带 SEH（`__try/__except`）保护，异常不会击穿游戏线程。
- 所有地址经 `mem::ClientAddress()` 按运行时基址路由，天然抗 ASLR / 基址重定位。

---

## 1. 功能一览

| 功能 | 说明 | 开关方式 |
|------|------|----------|
| 自动拾取 | 两种模式：**吸物**（改写掉落物坐标到脚下）、**发包**（组拾取包）。支持过滤ID黑/白名单，间隔可调（默认 500ms，下限 50ms）。城镇不工作 | 热键 / 面板 |
| 一键装备处理 | 扫描装备栏（56 格），按品质（白/蓝/紫/粉/史诗/异界）执行 保留/卖物/分解，支持名称过滤（`-` 分隔，子串匹配）。候选槽位复验后整批排队，每槽只发一次，最后统一整理并验证完整背包 | 热键 / 面板 |
| 顺图 | 两种模式：**坐标顺图**（读相邻房间门坐标走虚表移动 CALL，两步过图）、**强制顺图**（直接调过图 CALL）。支持上/下/左/右四方向。城镇不工作 | 热键 / 面板方向按钮 |
| 自动评分 | 副本内每 5 秒轮询（开关打开后预热 5 秒），当前评分 < 5201314 时加密写入 9999999 | 面板 / 热键 |
| GM 模式 | 开启时改写 `0x01763ED0` 处 GM 权限判定代码段（写入 16 字节补丁赋予 GM 权限），并备份原 16 字节；关闭时还原备份。一次性修补/还原模型，非轮询 | 面板 |
| 原生 XUI 面板 | 游戏原生界面（非外部窗口），F11 呼出；显示配置状态、上次处理结果；可切换全部模式与规则；每次刷新运行时直写勾选/单选状态偏移，视觉与配置实时一致 | F11 |
| 游戏内热键 | 只在游戏窗口内生效（钩子方式），不抢全局按键；8 个热键均可通过 NF.ini 修改 | — |
| 游戏内公告 | 走客户端喇叭公告 CALL（类型 30），随机鲜艳颜色；加载公告优先走 GameNative 的 postLoadNotice | — |

---

## 2. 模块结构

| 文件 | 职责 |
|------|------|
| `NF.cpp` | DLL 入口。`ClientPatchPluginInit` 启动控制线程：初始化配置、绑定 GameNative、释放内置 XUI 布局并 mount、注册窗口工厂、安装游戏线程钩子与热键钩子、100ms 定时器驱动界面轮询。另装未处理异常过滤器（崩溃写 NF.log） |
| `NFAddresses.h` | S4A21 基址/偏移/CALL/包头常量表（假定基址 0x00400000 的 VA）；新增 `kGmPermissionMode = 0x01763ED0`（GM 权限模式代码段） |
| `NFConfig.h/.cpp` | 配置读写与热重载（见下文"配置"） |
| `NFMemory.h/.cpp` | 进程内内存读写（VirtualQuery 保护）、宽字符串读取、**超级解密/加密**（游戏加密物品ID与评分的对称算法）；新增 `ReadBytes`/`WriteBytes`（代码段写入经 `VirtualProtect` 临时提权） |
| `NFRuntime.h/.cpp` | 运行时数据入口：人物/地图/背包指针、坐标、房间号、掉落物计数、副本判定 |
| `NFPacket.h/.cpp` | 发包封装：BeginPacket / WriteField（1/2/4 字节加密写入）/ EndPacket；拾取、卖物、分解、整理背包、强制过图 |
| `NFGameThread.h/.cpp` | 游戏主线程任务派发：`Post`（异步）/ `RunSync`（同步带超时）。泵送用 WH_CALLWNDPROC + WH_GETMESSAGE 双钩子 + PostThreadMessageW，任务执行套 SEH |
| `NFHotkey.h/.cpp` | 游戏窗口内热键：WH_GETMESSAGE（吃消息）+ WH_CALLWNDPROC（兜底检测）双钩子挂在游戏窗口线程，只拦当前进程游戏主窗口及子窗口的按键；跳过自动重复 |
| `NFAutoPickup.h/.cpp` | 自动拾取工作线程：按间隔把扫描任务投递到游戏线程执行；过滤判定含解密失败（ID=-1）的兜底策略 |
| `NFEquipProcessor.h/.cpp` | 一键装备处理核心（在游戏线程执行）：冻结初始槽位身份、发包前复验品质/名称/规则、整批请求后只整理一次；完整背包验证结束前保持防重入，失败项不自动重试；产出 Result 供面板显示 |
| `NFMapMove.h/.cpp` | 顺图：坐标模式两步移动（门口外侧→房间中心，间隔 200ms）；强制模式直接过图 CALL |
| `NFScore.h/.cpp` | 自动评分轮询（仅副本、仅加密写） |
| `NFGmMode.h/.cpp` | GM 模式：面板开关触发 `gm::Toggle()`，开启时备份并改写 `0x01763ED0` 处 16 字节 GM 权限判定；关闭还原（一次性修补/还原，非轮询） |
| `NFNativeUi.h/.cpp` | 原生 XUI 面板：窗口工厂、控件查找、文本写入（CNUIControlText 虚表 +0x110 直写 + 0xE8 布局刷新）、事件虚表（+0x17C）收点击、安装前地址签名校验 |
| `NFNotice.h/.cpp` | 游戏内公告：聊天管理器 → 调喇叭 CALL（类型 30） |
| `NFLog.h/.cpp` | 诊断日志 NF.log（UTF-16LE，每行开-关文件，崩溃也不丢）；受 `debug` 开关控制 |
| `NF.rc` | 资源文件：把 `NF.xui` 打包为 `NF_XUI` RCDATA 资源嵌入 DLL |
| `NF.xui` | XUI 布局源文件（CRLF 行尾、UTF-8 无 BOM；编辑后需重新编译才会生效） |
| `NF.ini` | 用户配置（UTF-16LE BOM，与 DLL 同目录） |
| `NF.vcxproj` | VS2022 v143 Win32 DLL 工程 |

依赖：`../GameNative/GameNativeApi.h`（GameNative.dll 提供 mount / 窗口ID预留 / 窗口工厂注册 / 载入公告等接口）。

---

## 3. 配置（NF.ini）

`NF.ini` 与 `NF.dll` 同目录，**UTF-16LE BOM** 编码。首次启动自动生成并补齐缺省键。
支持**热重载**：文件被保存后约 120ms 内重新读取生效，无需重启游戏。

### 3.1 持久化字段（写 ini）

| 节 | 键 | 默认值 | 说明 |
|----|----|--------|------|
| `[热键]` | 呼出界面 | `F11` | 开/关配置面板 |
| `[热键]` | 顺图上 / 顺图下 / 顺图左 / 顺图右 | `Alt+Up` / `Alt+Down` / `Alt+Left` / `Alt+Right` | 顺图移动 |
| `[热键]` | 拾取开关 | `Alt+Q` | 开/关自动拾取 |
| `[热键]` | 一键处理 | `Alt+F` | 触发一键装备处理 |
| `[热键]` | 自动评分 | `Alt+R` | 开/关自动评分（面板与热键均生效） |
| `[调试]` | debug | `1` | 1=写 NF.log，0=完全静默 |
| `[自动拾取]` | 过滤ID | （空） | 物品ID列表，逗号分隔。配合黑/白名单模式（见下） |
| `[自动拾取]` | 拾取模式 | `0` | 拾取模式：`0`=吸物，`1`=发包。面板切换也落盘，重启保持 |
| `[顺图]` | 顺图模式 | `0` | 顺图模式：`0`=坐标，`1`=强制。面板切换也落盘，重启保持 |
| `[装备处理]` | 名称过滤 | （空） | 装备名称关键字，**多个用 `-` 分隔**（子串匹配，命中则跳过不处理） |
| `[装备处理]` | 白装 / 蓝装 / 紫装 / 粉装 / 史诗 / 异界 | `2` / `2` / `2` / `0` / `0` / `0` | 各品质动作：`0`=保留，`1`=卖物，`2`=分解。面板切换也落盘，重启保持 |

热键写法：`Ctrl+Alt+Shift+Win` 修饰键组合 + 键名（`A-Z`、`0-9`、`F1-F24`、`Space/Enter/Tab/Esc/Insert/Delete/Home/End/PageUp/PageDown/Up/Down/Left/Right`）。`None` 或 `无` 表示禁用。

过滤ID语义：
- **黑名单模式**（默认）：列表内的物品不捡，其余都捡；解密失败（ID=-1）的物品**捡**。
- **白名单模式**：只捡列表内的物品，列表为空则什么都不捡；解密失败的**不捡**。

### 3.2 纯内存字段（不写 ini，重启后回到默认值）

以下项目仅存于内存，当次游戏会话有效，重启游戏后恢复默认（与面板首次打开的默认显示一致）：

| 项 | 默认值 | 可选值 |
|----|--------|--------|
| 过滤模式 | 黑名单 | 黑名单 / 白名单 |
| 拾取间隔 | 500ms | ≥50ms |
| GM 模式 | 关 | 关 / 开 |

在面板里修改这些项立即生效（内存路径），但不落盘；热重载也**不会**用 ini 冲掉当次会话的这些设置。
其余模式/规则（拾取模式、顺图模式、6 品质动作）均已持久化到 `NF.ini`（见 3.1），重启后保持。

---

## 4. 原生界面（XUI 面板）

- **呼出**：F11（可改热键），窗口 ID 由 GameNative 在菜单层之上动态预留（owner `ClientPatch.NF`）。
- **布局**：`NF.xui` 以 RCDATA 资源嵌入 DLL，运行时原样释放为 `<DLL目录>\NF_cache.xui` 并 mount 到游戏资源路径 `ui/nf.xui`。
- **控件约定**（ID 与 `NFNativeUi.cpp` 常量一一对应）：
  - 按钮：10=拾取开关、11=自动评分、12=GM模式开关、13=一键处理、20~23=顺图左/右/上/下。按钮文字静态，只收点击事件。
  - 单选：60/61=拾取模式（吸物/发包）、62/63=过滤模式（黑/白名单）、64/65=顺图模式（坐标/强制）、70~87=6品质×3动作（保留/卖/分解）。
  - 文本标签：50=状态栏（当前配置+上次处理结果）、51=过滤ID、52=名称过滤。
- **XUI 铁律**：动态内容只写 CNUIControlText 标签（按钮文字运行时不可改写）；布局文件必须 **CRLF 行尾 + UTF-8 无 BOM**，否则解析出空窗口。
- **视觉同步**：客户端按窗口缓存布局解析结果，重开面板不重新解析，勾选/单选状态停留在上次打开的样子。插件在每次面板刷新时按当前配置把对应控件的 `+0x3B8`（选中/活动标志）与 `+0x4F0`（绘制状态）直写，视觉与配置实时一致（见 `NFNativeUi.cpp` 的 `WriteCheckState`/`SyncControlStates`，全部带 SEH 保护）。

---

## 5. 构建

工程：`NF.vcxproj`（VS2022，v143，`Release|Win32`，Unicode 字符集，C++17）。
关键编译选项（游戏机缺 UCRT/VC 运行库，故须静态链接 CRT）：

- `/MT`（静态 CRT，产物仅依赖 KERNEL32/USER32）
- `/utf-8`（源码含中文注释，防 GBK 误读）
- `UNICODE/_UNICODE`（`NF.cpp` 显式使用 `FindResourceW`）
- `BufferSecurityCheck=false`（内联汇编 + 裸 CALL 环境不需要 GS 栈帧）

### 5.1 Visual Studio / MSBuild

```
msbuild NF.vcxproj /p:Configuration=Release /p:Platform=Win32
```

### 5.2 cl.exe 直编（MSBuild 不可用时的等价配方）

```
:: 1. 编译（/Fo 结尾必须是正斜杠，不能反斜杠）
cl /nologo /MT /O2 /utf-8 /Oy- /DWIN32 /DNDEBUG /D_WINDOWS /D_USRDLL ^
   /DUNICODE /D_UNICODE /I<目录> /c /Foobj/ *.cpp

:: 2. 资源
rc /fo NF.res NF.rc

:: 3. 链接
link /DLL /OUT:NF.dll obj\*.obj NF.res kernel32.lib user32.lib /MACHINE:X86
```

产物校验：`dumpbin /exports NF.dll` 应有 `ClientPatchPluginInit`；`dumpbin /dependents NF.dll` 应只有 KERNEL32/USER32。

---

## 6. 部署

1. `GameGaurd.ini`（客户端根目录，**不能带 UTF-8 BOM**）`[Plugins]` 节添加：
   ```
   Plugin6=NF.dll
   ```
2. `NF.dll` 与 `NF.ini` 放同一目录（放根目录或 `Plugins\` 二级目录均可——加载器会把相对路径拼到客户端根下解析，而 NF 用自身 DLL 目录定位 ini/日志）。
3. **加载顺序铁律**：`GameGaurd.ini` 的 `[Plugins]` 按文件书写顺序加载，**GameNative.dll 必须排在 NF.dll 之前**（NF 依赖其 mount/窗口ID/公告接口）。
4. 运行后日志在 `<DLL目录>\NF.log`（`debug=1` 时），排查加载失败先看这里；若 DllMain 都没执行（无 NF.log），通常是 CRT 依赖问题——确认用了 /MT 构建。

---

## 7. 地址标定（客户端更新时）

全部硬编码地址集中在两处：

- `NFAddresses.h`：基址/偏移/CALL/包头（数据来源：`参考/新绝对地址.txt`）。
- `NFNativeUi.cpp`：XUI 窗口管理器、控件文本虚表、构造/CALL 地址，安装前有字节签名校验，地址不符会拒绝安装面板（功能模块不受影响）。

所有值为假定 DNF.exe 基址 `0x00400000` 的 VA，运行时经 `ClientAddress()` 换算。
客户端版本变更后须在 IDA 重新标定上述常量，签名校验失败会在 NF.log 留下"地址签名校验失败"记录。


## 8. 致谢名单

<table>
  <tr>
    <td align="center">
      <a href="https://github.com/jiuzhouhan996">
        <img src="https://github.com/jiuzhouhan996.png?size=100" width="100" height="100" alt="jiuzhouhan996" />
        <br />
        jiuzhouhan996
      </a>
    </td>
    <td align="center">
      <a href="https://github.com/Milesian111">
        <img src="https://github.com/Milesian111.png?size=100" width="100" height="100" alt="Milesian111" />
        <br />
        Milesian111
      </a>
    </td>
  </tr>
</table>

