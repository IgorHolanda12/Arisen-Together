param(
    [Parameter(Mandatory = $true)][string]$Toolchain,
    [Parameter(Mandatory = $true)][string]$MinHook
)
$ErrorActionPreference = "Stop"
$cxx = Join-Path $Toolchain "bin\i686-w64-mingw32-clang++.exe"
$cc = Join-Path $Toolchain "bin\i686-w64-mingw32-clang.exe"
$out = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force $out | Out-Null

$mhSrc = @("src\buffer.c", "src\hook.c", "src\trampoline.c", "src\hde\hde32.c") | ForEach-Object { Join-Path $MinHook $_ }
$objs = @()
foreach ($c in $mhSrc) {
    $o = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($c) + ".o")
    & $cc -c -O2 -I (Join-Path $MinHook "include") $c -o $o
    if ($LASTEXITCODE) { throw "compile failed: $c" }
    $objs += $o
}
$src = Get-ChildItem (Join-Path $PSScriptRoot "src\*.cpp") | ForEach-Object { $_.FullName }
& $cxx -shared -O2 -std=c++17 -Wall -Wno-unused-function -static -static-libgcc -static-libstdc++ `
    -I (Join-Path $MinHook "include") $src $objs `
    "-Wl,--kill-at" "-Wl,--enable-stdcall-fixup" -lws2_32 -lgdi32 `
    -o (Join-Path $out "dinput8.dll")
if ($LASTEXITCODE) { throw "link failed" }

$rc = Join-Path $Toolchain "bin\i686-w64-mingw32-windres.exe"
$res = Join-Path $out "launcher_res.o"
Push-Location (Join-Path $PSScriptRoot "launcher")
& $rc launcher.rc -O coff -o $res
$rcFailed = $LASTEXITCODE
Pop-Location
if ($rcFailed) { throw "windres failed" }
& $cxx -O2 -std=c++17 -Wall -municode -mwindows -static -static-libgcc -static-libstdc++ `
    (Join-Path $PSScriptRoot "launcher\launcher.cpp") $res `
    -lcomctl32 -lwinhttp -lbcrypt -lshell32 -lole32 -luuid -lgdiplus -ldwmapi -luxtheme -lgdi32 -lws2_32 -liphlpapi `
    -o (Join-Path $out "ArisenTogether.exe")
if ($LASTEXITCODE) { throw "launcher link failed" }

$pkg = Join-Path $out "ArisenTogether"
if (Test-Path $pkg) { Get-ChildItem $pkg -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force } }
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item (Join-Path $out "ArisenTogether.exe"), (Join-Path $out "dinput8.dll"), `
    (Join-Path $PSScriptRoot "dist\coop.ini"), (Join-Path $PSScriptRoot "dist\README.txt"), `
    (Join-Path $PSScriptRoot "dist\LICENCES.txt") $pkg
Compress-Archive -Path (Join-Path $pkg "*") -DestinationPath (Join-Path $out "ArisenTogether.zip") -Force
Get-Item (Join-Path $out "dinput8.dll"), (Join-Path $out "ArisenTogether.exe"), (Join-Path $out "ArisenTogether.zip") |
    Select-Object Name, Length
