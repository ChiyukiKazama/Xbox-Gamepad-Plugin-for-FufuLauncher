# 构建与发布指引

## 环境

- Windows；Visual Studio 2026 C++ Build Tools，安装 C++ 桌面开发组件、`v145` 工具集与 Windows SDK。
- Windows PowerShell 5.1 或 PowerShell 7。
- 默认源码布局：`XboxGamepadPlugin` 与 `FufuLauncher.UnlockerIsland` 放在同一父目录。目前分别位于 `C:\Repos\XboxGamepadPlugin` 与 `C:\Repos\FufuLauncher.UnlockerIsland`。

构建只读取主插件仓库内的 `Patterns/Patterns.h`、`MinHook/MinHook.h` 和 `MinHook/libMinHook.x64.lib`；不要求先编译主插件，也不会修改它。`.tools` 中的本地分析工具、真实游戏文件和旧部署文件均不是构建依赖。

## 修改文本

- `config.ini`：插件名称、描述、开发者、版本号、设置标题和默认值。可修改 `[General]` 的展示内容及各设置的 `Name`；保留节名、`File`、`Type` 及三个热重载参数。使用 UTF-8 保存。
- `README.md`：随正式包发布的使用说明。
- `src/`：运行实现与日志文本，仅在需要修改代码时编辑。
- `docs/development-notes.md`：历史分析和诊断记录，不进入发布包。
- `diagnostics.example.ini`：开发排障示例，不进入发布包；不要把两个诊断节加回正式 `config.ini`。

更改版本号时同步修改 `README.md` 的当前版本和 `src/Plugin.cpp` 的启动日志版本字符串。打包文件名以 `config.ini` 的 `Version = x.y.z` 为准。历史记录中的旧版本号不必改动。

## 手动构建发布包

保存修改后，双击项目根目录的 **`publish.cmd`**。它执行 Release x64 构建与正式打包，不依赖测试目录、不运行自动化测试；失败时停止，不输出新的成功提示，也不会覆盖游戏安装目录。控制台保留结果，便于查看错误。

也可在 PowerShell 中运行：

```powershell
Set-Location 'C:\Repos\XboxGamepadPlugin'
.\publish.ps1
```

如果 PowerShell 阻止执行本地脚本，可使用 `publish.cmd`；它仅为本次脚本进程指定执行策略，不更改系统或用户策略。

脚本自动使用 Visual Studio 安装查询工具寻找 C++ 构建环境。无法找到时明确指定：

```powershell
.\publish.ps1 -MSBuildPath 'C:\VSTool\VSBuildTools2026\MSBuild\Current\Bin\MSBuild.exe'
```

主插件源码在其他位置时再加：

```powershell
.\publish.ps1 -UnlockerIslandRoot 'D:\Repos\FufuLauncher.UnlockerIsland'
```

只编译、不打包：双击 **`build.cmd`**，或运行 `.\build.ps1`。两个 `.cmd` 也允许传入上述参数。

## 产物

- `bin/x64/Release/XboxGamepadPlugin.dll`：编译的 DLL，旁边的配置来自项目根目录。
- `bin/publish/<版本号>/`：本次正式包的五个文件。
- `bin/publish/XboxGamepadPlugin-<版本号>.zip`：可上传的发布包。

ZIP 根目录严格只包含：`XboxGamepadPlugin.dll`、`config.ini`、`README.md`、`LICENSE`、`THIRD_PARTY_NOTICES.md`。诊断项只从正式配置移除，不删除排障代码；缺省关闭。若正式配置含诊断节，打包脚本会拒绝发布。

`bin/`、`obj/`、`.tools/`、`tests/` 和 `docs/` 等已在 `.gitignore` 中排除。测试与历史记录仅在本地保留，不影响克隆后的构建或打包；源码、构建脚本、根目录说明文档、正式配置及许可应一并提交。`bin/backups/` 中旧文件保持可恢复，不作为发布输入。不要把 PDB、测试程序、日志或游戏文件手动加进发布包。

## 发布前验收

发布流程仅验证编译及打包，不执行自动化测试，也不证明游戏内 Hook 和可见效果。更新 DLL 后须完全重启游戏，再验证：

1. 地图开关、按键在运行中修改后生效，按住旧绑定键时改绑不误触。
2. 从启动系数 `1.0` 热改为 `2.0`，再改回 `1.0`；手柄速度正确，键鼠不受影响。
3. 修改游戏内灵敏度、保存设置及进出菜单后，系数不叠乘、不污染原设置。
4. 正式配置不显示两个诊断项，实验性全局热切换保持关闭。

发布对应版本源码与许可证。用户安装到启动器的 `Plugins/GamepadPlugin`；升级时提醒其保留个人配置。构建/打包入口不注入、不启动或关闭游戏，也不自动部署。
