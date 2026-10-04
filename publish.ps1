param(
    [string]$MSBuildPath = '',
    [string]$UnlockerIslandRoot = (Join-Path $PSScriptRoot '..\FufuLauncher.UnlockerIsland')
)
$ErrorActionPreference = 'Stop'

$configText = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'config.ini') -Encoding UTF8 -Raw
if ($configText -match '(?im)^\s*\[(InputDiagnostics|DebugLog)\]') {
    throw 'Release config.ini must not contain InputDiagnostics or DebugLog sections.'
}
$versionMatch = [regex]::Match($configText, '(?m)^Version\s*=\s*(\d+\.\d+\.\d+)\s*$')
if (!$versionMatch.Success) { throw 'config.ini must contain a valid Version = x.y.z.' }
$version = $versionMatch.Groups[1].Value
& (Join-Path $PSScriptRoot 'build.ps1') -MSBuildPath $MSBuildPath -UnlockerIslandRoot $UnlockerIslandRoot
$releasePath = Join-Path $PSScriptRoot "bin\publish\$version"
New-Item -ItemType Directory -Path $releasePath -Force | Out-Null
$packageFiles = @()
foreach ($name in @('XboxGamepadPlugin.dll','config.ini','README.md','LICENSE','THIRD_PARTY_NOTICES.md')) {
    $sourcePath = if ($name -eq 'XboxGamepadPlugin.dll') {
        Join-Path $PSScriptRoot "bin\x64\Release\$name"
    } else { Join-Path $PSScriptRoot $name }
    $destinationPath = Join-Path $releasePath $name
    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    $packageFiles += $destinationPath
}
$zipPath = Join-Path (Split-Path $releasePath -Parent) "XboxGamepadPlugin-$version.zip"
Compress-Archive -LiteralPath $packageFiles -DestinationPath $zipPath -Force
Get-Item -LiteralPath $zipPath
