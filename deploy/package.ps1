# Builds the distributables of DCS ILARTS from a finished Release build.
#   powershell -ExecutionPolicy Bypass -File deploy\package.ps1 -Version 1.0.27 [-NoInstaller]
# -Version: numbers and dots (mod managers read it from the file name). Output in dist\:
#   DCS-ILARTS_v<version>.zip          Open Mod Manager (OMM): one folder named like the zip, holding the Saved
#                                      Games\DCS layout; OMM takes the version from "_v<version>".
#                                      modpack.xml at its root: description (mod-description.md), thumbnail.png.
#   ovgme\DCS-ILARTS.zip               OvGME / JSGME / manual: one folder "DCS-ILARTS" holding the same layout
#                                      (a stable mod name across versions); README.txt (mod-description.txt) and
#                                      VERSION.txt at its root.
#   DCS-ILARTS-Setup-<version>.exe     Windows installer (Inno Setup 6, deploy\ILARTS.iss): the same files into
#                                      Saved Games\DCS, the hook's permissions, Start menu entries, optional virtual
#                                      cameras, uninstall.
# The layout (what all three install):
#   Mods\tech\ILARTS\bin\ILARTS.dll, ILARTS.exe, ILARTSCam.dll (virtual cameras), ILARTS-hook.lua (the copy REPAIR
#     restores the hook from), ui\..., licence notices
#   Mods\tech\ILARTS\README.txt
#   Scripts\Hooks\ILARTS-hook.lua
#   Missions\ILARTS Benchmark.miz (the benchmark's scene, from deploy\Missions; the installer's uninstall removes
#     it with the rest)
# Nothing at the root: a mod manager would copy it into Saved Games\DCS itself.
# The hook's development switches (VERBOSE and the test / investigation flags) must be off; the script refuses to
# package otherwise; the island and deck cameras (EXTRA_CAMERAS) must be on. (Experimental features such as FLIR are
# switched on by the user in SETTINGS > Experimental, not by the package.)
param([string]$Version = '1.0.0', [switch]$NoInstaller)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+(\.\d+)*$') { throw "Version must be numbers and dots (mod managers parse it): $Version" }
$root = Split-Path -Parent $PSScriptRoot
$rel = Join-Path $root 'build\Release'
$bin = 'Mods\tech\ILARTS\bin'
$hookSrc = Join-Path $PSScriptRoot 'Scripts\Hooks\ILARTS-hook.lua'
$files = [ordered]@{
    "$bin\ILARTS.dll"         = Join-Path $rel 'ILARTS.dll'
    "$bin\ILARTS.exe"         = Join-Path $rel 'ILARTS.exe'
    "$bin\ILARTSCam.dll"      = Join-Path $rel 'ILARTSCam.dll'
    "$bin\ILARTS-hook.lua"    = $hookSrc
    'Scripts\Hooks\ILARTS-hook.lua' = $hookSrc
    'Mods\tech\ILARTS\README.txt'   = Join-Path $PSScriptRoot 'README.txt'
}
# The console interface (WebView2 page) next to ILARTS.exe; without it the viewer says to reinstall and exits.
$ui = Join-Path $root 'ui'
foreach ($f in @('index.html', 'ilarts-console.js', 'fonts\fonts.css', 'fonts\saira-variable.woff2', 'fonts\ibm-plex-mono-400.woff2',
                 'fonts\ibm-plex-mono-500.woff2', 'fonts\ibm-plex-mono-600.woff2', 'fonts\OFL-Saira.txt', 'fonts\OFL-IBMPlexMono.txt')) {
    $files["$bin\ui\$f"] = Join-Path $ui $f
}
# Licence notices of what is linked in: Microsoft's WebView2 loader, the Spout2 SDK.
$files["$bin\LICENSE-WebView2.txt"] = Join-Path $root 'third_party\webview2\LICENSE.txt'
$files["$bin\LICENSE-Spout2.txt"] = Join-Path $root 'third_party\spout\LICENSE.txt'
# The user guide, as a PDF and as one self-contained web page (docs\guide\build-guide.ps1 makes both).
$files['Mods\tech\ILARTS\DCS-ILARTS-Guide.pdf'] = Join-Path $root 'docs\DCS-ILARTS-Guide.pdf'
$files['Mods\tech\ILARTS\DCS-ILARTS-Guide.html'] = Join-Path $root 'docs\DCS-ILARTS-Guide.html'
# The benchmark mission (SETTINGS > Benchmark): into Saved Games\DCS\Missions.
$files['Missions\ILARTS Benchmark.miz'] = Join-Path $PSScriptRoot 'Missions\ILARTS Benchmark.miz'
foreach ($src in $files.Values) { if (-not (Test-Path -LiteralPath $src)) { throw "missing: $src (build Release first)" } }
# The binaries carry their version (CMake's DCS_ILARTS_VERSION, src/version.rc): it must be the one being packaged.
foreach ($b in @('ILARTS.dll', 'ILARTS.exe', 'ILARTSCam.dll')) {
    $have = (Get-Item (Join-Path $rel $b)).VersionInfo.ProductVersion
    if ($have -ne $Version) {
        throw "build\Release\$b is version '$have', not $Version. Build it with: cmake -S . -B build -A x64 `"-DDCS_ILARTS_VERSION=$Version`" (quoted in PowerShell), then cmake --build build --config Release"
    }
}

$hook = [System.IO.File]::ReadAllText($hookSrc)
$mustBe = [ordered]@{
    'VERBOSE' = 'false'; 'CAPTURE_ONCE' = 'false'; 'FLOLS_LOG' = 'false'; 'GLIDESLOPE_MEMSCAN' = 'false'
    'SHIP_POSE_TEST' = 'false'; 'HULL_SCAN' = 'false'; 'DECK_TRACE' = 'false'; 'HDG_TRACE' = 'false'
    'PLAT_ORIGIN' = "'on'"; 'CLOUD_REBASE' = "'on'"; 'HULL_REFRESH' = "'on'"
    'EXTRA_CAMERAS' = 'true'   # the island and deck cameras ship (user's decision, 2026-11-15)
}
foreach ($k in $mustBe.Keys) {
    if ($hook -notmatch ("(?m)^local $k = " + [regex]::Escape($mustBe[$k]) + '(?=\s|$)')) {
        throw "hook setting $k is not $($mustBe[$k]) - not packaging a development configuration"
    }
}

# the layout, staged once
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'stage'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
foreach ($dst in $files.Keys) {
    $to = Join-Path $stage $dst
    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
    Copy-Item -Force -LiteralPath $files[$dst] -Destination $to
}
$head = [System.IO.File]::ReadAllBytes($hookSrc)[0..2]
if ($head[0] -eq 0xEF -and $head[1] -eq 0xBB -and $head[2] -eq 0xBF) { throw 'the hook starts with a UTF-8 BOM, which DCS''s Lua 5.1 rejects' }

# A zip of the layout inside one folder, plus files at the zip's root (the mod managers' description files, which
# they read and do not install). Written here rather than with Compress-Archive: Windows PowerShell 5.1's
# Compress-Archive stores "\" in entry names, and OMM / OvGME look entries up by "/" paths. Every folder gets its own
# entry (OvGME accepts a zip only with a folder entry named like the zip).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$utf8 = New-Object System.Text.UTF8Encoding $false
function Zip-As([string]$folder, [string]$zip, $rootFiles) {
    if (Test-Path $zip) { Remove-Item -Force $zip }
    $fs = [System.IO.File]::Open($zip, 'CreateNew')
    $za = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $rel = { param($p) $p.Substring($stage.Length + 1).Replace('\', '/') }
        [void]$za.CreateEntry("$folder/")
        Get-ChildItem -Recurse -Directory $stage | Sort-Object FullName | ForEach-Object { [void]$za.CreateEntry("$folder/$(& $rel $_.FullName)/") }
        Get-ChildItem -Recurse -File $stage | Sort-Object FullName | ForEach-Object {
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $_.FullName, "$folder/$(& $rel $_.FullName)", 'Optimal')
        }
        foreach ($name in $rootFiles.Keys) {
            $w = New-Object System.IO.BinaryWriter($za.CreateEntry($name, 'Optimal').Open())
            $w.Write($utf8.GetBytes($rootFiles[$name])); $w.Dispose()
        }
    } finally { $za.Dispose(); $fs.Dispose() }
    Write-Output "Packaged $zip"
}
$descMd = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'mod-description.md'))
$descTxt = [System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'mod-description.txt'))
$thumb = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'thumbnail.png')))
# OMM: modpack.xml at the root (root element Open_Mod_Manager_Package; <install> = the folder; description shown as
# Markdown; thumbnail a PNG data URI on one line). OMM takes the version from the file name, "_v<version>".
$ommFolder = "DCS-ILARTS_v$Version"
$modpack = "<?xml version=`"1.0`" encoding=`"UTF-8`"?>`r`n<Open_Mod_Manager_Package>`r`n" +
    "  <install>$ommFolder</install>`r`n  <category>Plugin</category>`r`n" +
    "  <description>$([System.Security.SecurityElement]::Escape($descMd.Replace("`r`n", "`n")))</description>`r`n" +
    "  <thumbnail>data:image/png;base64,$thumb</thumbnail>`r`n</Open_Mod_Manager_Package>`r`n"
Zip-As $ommFolder (Join-Path $dist "$ommFolder.zip") ([ordered]@{ 'modpack.xml' = $modpack })
# OvGME: README.txt (description, read in the ANSI code page: keep it ASCII) and VERSION.txt at the root. The zip and
# its folder keep one name across versions so OvGME sees an update of the same mod.
if ($descTxt -match '[^\x00-\x7F]') { throw 'deploy\mod-description.txt must be ASCII (OvGME reads it in the ANSI code page)' }
New-Item -ItemType Directory -Force (Join-Path $dist 'ovgme') | Out-Null
Zip-As 'DCS-ILARTS' (Join-Path $dist 'ovgme\DCS-ILARTS.zip') ([ordered]@{ 'README.txt' = $descTxt.Replace("`r`n", "`n").Replace("`n", "`r`n"); 'VERSION.txt' = $Version })

if (-not $NoInstaller) {
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup), or use -NoInstaller' }
    & $iscc /Q "/DVersion=$Version" "/DStage=$stage" "/DOutDir=$dist" (Join-Path $PSScriptRoot 'ILARTS.iss')
    if ($LASTEXITCODE) { throw "Inno Setup failed ($LASTEXITCODE)" }
    Write-Output "Packaged $(Join-Path $dist "DCS-ILARTS-Setup-$Version.exe")"
}
Get-ChildItem -Recurse -File $stage | ForEach-Object { '  ' + $_.FullName.Substring($stage.Length + 1) + '  (' + $_.Length + ' bytes)' }
