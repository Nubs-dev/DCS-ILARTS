# Prints docs\guide\guide.html to docs\DCS-ILARTS-Guide.pdf with Microsoft Edge (headless), and writes
# docs\DCS-ILARTS-Guide.html: the same guide as one self-contained web page (fonts and pictures embedded) for a
# browser. guide.html's `@media screen` rules lay it out for the screen.
#   powershell -ExecutionPolicy Bypass -File docs\guide\build-guide.ps1 -Version 1.0.27
param([string]$Version = '1.0.27')
$ErrorActionPreference = 'Stop'
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }
$src = Join-Path $PSScriptRoot 'guide.html'
$docs = Split-Path -Parent $PSScriptRoot
$out = Join-Path $docs 'DCS-ILARTS-Guide.pdf'
$outHtml = Join-Path $docs 'DCS-ILARTS-Guide.html'
$utf8 = New-Object System.Text.UTF8Encoding $false
$html = [System.IO.File]::ReadAllText($src).Replace('{{VERSION}}', $Version)
# the version goes into a temporary copy next to the source (its relative links stay valid)
$tmp = Join-Path $PSScriptRoot '_print.html'
[System.IO.File]::WriteAllText($tmp, $html, $utf8)
$edgeProfile = Join-Path $env:TEMP 'ilarts-guide-edge'
try {
    $p = Start-Process -FilePath $edge -Wait -PassThru -WindowStyle Hidden -ArgumentList @(
        '--headless=new', '--disable-gpu', "--user-data-dir=`"$edgeProfile`"", '--no-pdf-header-footer',
        '--virtual-time-budget=5000', "--print-to-pdf=`"$out`"", ('"file:///' + $tmp.Replace('\', '/') + '"'))
    if (-not (Test-Path $out)) { throw "Edge did not write $out (exit $($p.ExitCode))" }
} finally { Remove-Item -Force $tmp -ErrorAction SilentlyContinue }
Write-Output "Wrote $out"

# The web page: every relative file the guide uses (the font stylesheet, its fonts, the pictures) as data URIs.
function Get-DataUri([string]$path) {
    $type = switch ([System.IO.Path]::GetExtension($path).ToLowerInvariant()) {
        '.png' { 'image/png' } '.jpg' { 'image/jpeg' } '.jpeg' { 'image/jpeg' } '.woff2' { 'font/woff2' } default { throw "no type for $path" }
    }
    'data:' + $type + ';base64,' + [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($path))
}
$fontsCss = Join-Path $PSScriptRoot '..\..\ui\fonts\fonts.css'
$fontsDir = Split-Path -Parent $fontsCss
$css = [regex]::Replace([System.IO.File]::ReadAllText($fontsCss), 'url\(([^)]+)\)', {
    param($m) 'url(' + (Get-DataUri (Join-Path $fontsDir $m.Groups[1].Value.Trim("'`""))) + ')' })
$link = '<link rel="stylesheet" href="../../ui/fonts/fonts.css">'
if (-not $html.Contains($link)) { throw "guide.html no longer links $link" }
$web = $html.Replace($link, "<style>`n$css`n</style>")
$web = [regex]::Replace($web, 'src="(\.\./[^"]+)"', { param($m) 'src="' + (Get-DataUri (Join-Path $PSScriptRoot $m.Groups[1].Value)) + '"' })
if ($web -match '(src|href)="\.\./') { throw 'a relative link is left in the web page' }
[System.IO.File]::WriteAllText($outHtml, $web, $utf8)
Write-Output "Wrote $outHtml"
