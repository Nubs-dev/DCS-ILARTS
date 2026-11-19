# Installs (or with -Uninstall removes) DCS ILARTS into DCS Saved Games, from this build (development).
#   powershell -ExecutionPolicy Bypass -File deploy\install.ps1 [-SavedGames <path>] [-Uninstall]
# Touches only:
#   <SavedGames>\Mods\tech\ILARTS\bin\ILARTS.dll (+ .pdb, viewer_path.txt)
#   <SavedGames>\Scripts\Hooks\ILARTS-hook.lua
#   <SavedGames>\Missions\ILARTS Benchmark.miz (the benchmark's scene: copied when missing or different; -Uninstall
#   removes it only while it is still this one)
#   and removes the install under the old name (Mods\tech\LSODiagnostic, Scripts\Hooks\LSODiagnostic-hook.lua):
#   two hooks would load the mod twice.
# For distribution use deploy\package.ps1 instead (the viewer then lives in the mod's bin folder).
param(
    [string]$SavedGames = (Join-Path $env:USERPROFILE 'Saved Games\DCS'),
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $SavedGames 'Mods\tech\ILARTS\bin'
$hook = Join-Path $SavedGames 'Scripts\Hooks\ILARTS-hook.lua'
$oldMod = Join-Path $SavedGames 'Mods\tech\LSODiagnostic'
$oldHook = Join-Path $SavedGames 'Scripts\Hooks\LSODiagnostic-hook.lua'
$mizSrc = Join-Path $PSScriptRoot 'Missions\ILARTS Benchmark.miz'
$miz = Join-Path $SavedGames 'Missions\ILARTS Benchmark.miz'
function Test-SameFile([string]$a, [string]$b) {
    (Test-Path -LiteralPath $a) -and (Test-Path -LiteralPath $b) -and
        (Get-FileHash -LiteralPath $a).Hash -eq (Get-FileHash -LiteralPath $b).Hash
}

if (-not (Test-Path $SavedGames)) { throw "DCS Saved Games folder not found: $SavedGames" }

foreach ($old in @($oldHook, $oldMod)) {
    if (Test-Path $old) {
        Remove-Item -Recurse -Force $old
        Write-Output "Removed the old install: $old"
    }
}

if ($Uninstall) {
    Remove-Item -Force -ErrorAction SilentlyContinue $hook
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $SavedGames 'Mods\tech\ILARTS')
    if (Test-SameFile $mizSrc $miz) { Remove-Item -Force -LiteralPath $miz; Write-Output "Removed $miz" }
    Write-Output "Removed $hook and $binDir"
    return
}

$dll = Join-Path $root 'build\Release\ILARTS.dll'
if (-not (Test-Path $dll)) { throw "Build first: $dll not found" }
New-Item -ItemType Directory -Force $binDir | Out-Null
New-Item -ItemType Directory -Force (Split-Path $hook) | Out-Null
Copy-Item -Force $dll $binDir
$pdb = Join-Path $root 'build\Release\ILARTS.pdb'
if (Test-Path $pdb) { Copy-Item -Force $pdb $binDir }
Copy-Item -Force (Join-Path $PSScriptRoot 'Scripts\Hooks\ILARTS-hook.lua') $hook
# Where the DLL finds the viewer to start it with the mission (development: it stays in the build
# folder, next to its ILARTS.ini; without this file the DLL starts ILARTS.exe from its own folder).
$viewer = Join-Path $root 'build\Release\ILARTS.exe'
if (Test-Path $viewer) {
    [System.IO.File]::WriteAllText((Join-Path $binDir 'viewer_path.txt'), $viewer, (New-Object System.Text.UTF8Encoding $false))
}
Write-Output "Installed:`n  $binDir\ILARTS.dll`n  $hook"
# The benchmark mission: copied when it is missing or differs from this one (an identical copy stays as it is).
if ((Test-Path -LiteralPath $mizSrc) -and -not (Test-SameFile $mizSrc $miz)) {
    New-Item -ItemType Directory -Force (Split-Path $miz) | Out-Null
    Copy-Item -Force -LiteralPath $mizSrc -Destination $miz
    Write-Output "  $miz"
}
# The hook reads the carrier, aircraft and wind from the mission scripting state through
# net.dostring_in, which DCS only allows for the states listed in Config\autoexec.cfg. Only checked
# here, never edited (other mods may keep their own settings in that file).
$autoexec = Join-Path $SavedGames 'Config\autoexec.cfg'
$cfg = if (Test-Path $autoexec) { [System.IO.File]::ReadAllText($autoexec) } else { '' }
# Either the plain lines, or the block REPAIR writes (allow(net.allow_..., "...") adds to what is allowed).
$unsafeOk = $cfg -match 'net\.allow_unsafe_api\s*=\s*(\{[^}]*|allow\(\s*net\.allow_unsafe_api\s*,\s*)"userhooks"'
$serverOk = $cfg -match 'net\.allow_dostring_in\s*=\s*(\{[^}]*|allow\(\s*net\.allow_dostring_in\s*,\s*)"server"'
if (-not ($unsafeOk -and $serverOk)) {
    Write-Warning ("$autoexec does not allow what the hook needs in single player / when hosting. Add (or merge into existing lines):`n" +
                   '  net.allow_unsafe_api = {"userhooks"}' + "`n" +
                   '  net.allow_dostring_in = {"mission", "server"}')
}
