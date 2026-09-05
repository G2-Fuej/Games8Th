# PWA 屏蔽器（PWA Blocker）

> **Games8Th.Team 团队开发项目** | © 2026 Games8Th.Team
>
> 依据以下分析资料制作的本机屏蔽工具：
> - 当前安装包分析：`PWA_analysis`
> - 同类项目动态分析：`PWA36_analysis`

## 团队开发说明

本项目由 **Games8Th.Team** 开发与维护，聚焦游戏客户端组件分析与本机安全屏蔽工具链：

| 角色 | 说明 |
|------|------|
| 项目立项 | Games8Th.Team —— 组件筛查与拦截工具需求 |
| 逆向分析 | 基于 `PWA_analysis` / `PWA36_analysis` 报告还原组件清单与哈希 |
| 工具开发 | C# / .NET 10 实现自动检测、实时循环拦截、日志窗口 |
| 测试验证 | 编译发布、运行验证、可逆性检查 |

- 仓库：<https://github.com/G2-Fuej/Games8Th>
- 本模块目录：`PwaBlocker/`
- 品牌与版权：Games8Th.Team，详见仓库根 [LICENSE](../LICENSE)

## 功能

- **自动检测 PWA 安装路径**：优先从 `MessageTransfer` 服务驱动路径反推，再检查卸载注册表、运行中进程和常见固定安装位置；只有通过报告哈希证据的目录才会被认定为有效目标。显式传入 `--root` 时以该路径为准。
- **等待平台启动**：启动后先显示"等待完美世界平台启动..."，检测到完美世界竞技平台进程后才开始筛查（对齐 `PWA36_analysis` 的"检测到进程再动手"思路）。
- **实时循环拦截**：默认进入持续循环模式，每隔 `--interval` 秒筛查 + 尝试停止匹配进程 + 禁用 `MessageTransfer` 服务，**一直循环不自动结束**；结束方式为关闭日志窗口或 `Ctrl+C`。
- **实时日志输出**：逐行实时刷新（屏幕与日志文件同步），每轮输出组件路径、当前哈希、期望哈希、匹配状态和是否拦截。
- **彩色状态**：匹配显示绿色；未纳入规则 / 不匹配 / 缺失显示红色；不适用拦截的 DLL / NODE 组件显示黄色。
- **可逆操作**：`block` / `stop` / `unblock` 会保存服务快照，防火墙规则只管理 `AC-Block-PWA-` 前缀规则，`unblock` 可按快照恢复。

## 处理清单

默认处理报告中已核验的核心可执行组件，并处理已确认注册为 `MessageTransfer` 的驱动服务：

- `完美世界竞技平台.exe`
- `plugin\dota2Assist.exe`
- `plugin\pwautil.exe`
- `plugin\pwautil64.exe`
- `plugin\WmgpOptimizer.exe`
- `plugin\WmgpOptimizer_x64.exe`
- `plugin\resource\7z\7za.exe`
- `resources\elevate.exe`
- `plugin\MessageTransfer.sys` 对应的 `MessageTransfer` 服务

> DLL、NODE 文件不是独立进程，Windows 防火墙程序规则不能直接按 DLL/NODE 文件匹配，因此它们用于组件清单与哈希核验，不单独创建程序规则（日志中显示黄色"不适用"）。

## 构建

```powershell
dotnet publish 'PwaBlocker\PwaBlocker.csproj' `
  -c Release `
  -r win-x64 `
  --self-contained true `
  -o 'PwaBlocker\publish'
```

发布产物：`PwaBlocker\publish\PWA屏蔽器.exe`（需要管理员权限，程序清单自动请求提升）。

## 使用

```powershell
# 默认：实时循环拦截（等待平台进程 → 持续筛查 + 拦截）
.\PWA屏蔽器.exe

# 单次筛查（不循环）
.\PWA屏蔽器.exe status

# 预演筛查 / 只校验清单
.\PWA屏蔽器.exe scan

# 一次性屏蔽：创建出站阻断规则 + 停止进程 + 禁用服务
.\PWA屏蔽器.exe block

# 停止匹配进程与服务（不创建防火墙规则）
.\PWA屏蔽器.exe stop

# 持续监控并按间隔核验拦截
.\PWA屏蔽器.exe watch --interval 3

# 恢复：删除本工具创建的规则并恢复服务快照
.\PWA屏蔽器.exe unblock
```

选项：

```text
--root <目录>       显式指定 PWA 安装目录（覆盖自动检测）
--all-exe           将报告清单中所有已核验 EXE 纳入阻断
--dry-run           只显示拟执行动作，不改系统状态
--what-if           --dry-run 的别名
--interval <秒>     watch 间隔，范围 1-3600，默认 3 秒
```

## 日志

- 日志文件保存在 `%ProgramData%\AC\PwaBlocker\logs`，每次运行生成独立的 UTF-8 日志。
- 完成后弹出独立的 CMD 日志窗口（`/k` 保持打开），用户手动关闭即完成本轮审查。
- `MessageTransfer` 服务首次被修改前，会保存原始状态到 `%ProgramData%\AC\PwaBlocker\MessageTransfer.state.json`。

## 授权与免责

- 工具不删除、覆盖或隔离 PWA 原文件；所有操作均可通过 `unblock` 或系统管理恢复。
- 仅供授权研究与本机自用场景使用。
