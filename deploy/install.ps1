# Installs (or with -Uninstall removes) DCS ILARTS into DCS Saved Games, from this build (development).
#   powershell -ExecutionPolicy Bypass -File deploy\install.ps1 [-SavedGames <path>] [-Uninstall]
# Touches only:
#   <SavedGames>\Mods\tech\ILARTS\bin\ILARTS.dll (+ .pdb, viewer_path.txt)
#   <SavedGames>\Scripts\Hooks\ILARTS-hook.lua
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
# The hook reads the carrier, aircraft and wind from the mission scripting state through
# net.dostring_in, which DCS only allows for the states listed in Config\autoexec.cfg. Only checked
# here, never edited (other mods may keep their own settings in that file).
$autoexec = Join-Path $SavedGames 'Config\autoexec.cfg'
$cfg = if (Test-Path $autoexec) { [System.IO.File]::ReadAllText($autoexec) } else { '' }
$unsafeOk = $cfg -match 'net\.allow_unsafe_api\s*=\s*\{[^}]*"userhooks"'
$serverOk = $cfg -match 'net\.allow_dostring_in\s*=\s*\{[^}]*"server"'
if (-not ($unsafeOk -and $serverOk)) {
    Write-Warning ("$autoexec does not allow what the hook needs in single player / when hosting. Add (or merge into existing lines):`n" +
                   '  net.allow_unsafe_api = {"userhooks"}' + "`n" +
                   '  net.allow_dostring_in = {"mission", "server"}')
}
