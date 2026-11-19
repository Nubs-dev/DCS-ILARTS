# Prints docs\guide\guide.html to docs\DCS-ILARTS-Guide.pdf with Microsoft Edge (headless).
#   powershell -ExecutionPolicy Bypass -File docs\guide\build-guide.ps1 -Version 1.0.27
param([string]$Version = '1.0.27')
$ErrorActionPreference = 'Stop'
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }
$src = Join-Path $PSScriptRoot 'guide.html'
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'DCS-ILARTS-Guide.pdf'
# the version goes into a temporary copy next to the source (its relative links stay valid)
$tmp = Join-Path $PSScriptRoot '_print.html'
$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($tmp, [System.IO.File]::ReadAllText($src).Replace('{{VERSION}}', $Version), $utf8)
$edgeProfile = Join-Path $env:TEMP 'ilarts-guide-edge'
try {
    $p = Start-Process -FilePath $edge -Wait -PassThru -WindowStyle Hidden -ArgumentList @(
        '--headless=new', '--disable-gpu', "--user-data-dir=`"$edgeProfile`"", '--no-pdf-header-footer',
        '--virtual-time-budget=5000', "--print-to-pdf=`"$out`"", ('"file:///' + $tmp.Replace('\', '/') + '"'))
    if (-not (Test-Path $out)) { throw "Edge did not write $out (exit $($p.ExitCode))" }
} finally { Remove-Item -Force $tmp -ErrorAction SilentlyContinue }
Write-Output "Wrote $out"
