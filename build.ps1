param(
    [string]$MSBuildPath = '',
    [string]$UnlockerIslandRoot = (Join-Path $PSScriptRoot '..\FufuLauncher.UnlockerIsland')
)
$ErrorActionPreference = 'Stop'
if (-not $MSBuildPath) {
    $taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $taskVsWhere) {
        $MSBuildPath = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    }
}
if (-not $MSBuildPath -or -not (Test-Path -LiteralPath $MSBuildPath)) {
    throw 'MSBuild not found. Install Visual Studio 2026 C++ Build Tools, or specify -MSBuildPath.'
}
if (-not (Test-Path -LiteralPath $UnlockerIslandRoot -PathType Container)) {
    throw 'Main plugin source directory not found. Specify -UnlockerIslandRoot.'
}
$UnlockerIslandRoot = (Resolve-Path -LiteralPath $UnlockerIslandRoot).Path
foreach ($dependency in @('Patterns\Patterns.h', 'MinHook\MinHook.h', 'MinHook\libMinHook.x64.lib')) {
    $dependencyPath = Join-Path $UnlockerIslandRoot "FufuLauncher.UnlockerIsland\$dependency"
    if (-not (Test-Path -LiteralPath $dependencyPath -PathType Leaf)) {
        throw "Required build dependency not found: $dependencyPath"
    }
}
& $MSBuildPath (Join-Path $PSScriptRoot 'XboxGamepadPlugin.vcxproj') /m /t:Build /p:Configuration=Release /p:Platform=x64 "/p:UnlockerIslandRoot=$UnlockerIslandRoot" /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "Plugin build failed: $LASTEXITCODE" }
Get-Item -LiteralPath (Join-Path $PSScriptRoot 'bin\x64\Release\XboxGamepadPlugin.dll')
