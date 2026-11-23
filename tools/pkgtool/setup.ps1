# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of FPKGi+. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

# Builds the patched PkgTool that tools/build.sh packages FPKGi+ with, into
# tools/pkgtool/bin. Needs git and the .NET SDK (6 or later).
#
# Why: the OpenOrbis toolchain's PkgTool (LibOrbisPkg) only turns files placed
# directly in sce_sys/ into package entries, so sce_sys/changeinfo/changeinfo.xml
# never becomes the package's changeinfo entry (0x1260) and the console shows
# no Update History. changeinfo.patch adds that folder (and targets .NET 6).
# LibOrbisPkg is by maxton, LGPL-3.0: https://github.com/maxton/LibOrbisPkg
#   .\tools\pkgtool\setup.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$src = Join-Path $here 'src'
$bin = Join-Path $here 'bin'
$commit = '643477263b2644e0803e0f58b8726ea4e3f3b7d4'  # LibOrbisPkg master, 2020-07-26

if (-not (Test-Path (Join-Path $src '.git'))) {
    git clone -q https://github.com/maxton/LibOrbisPkg.git $src
    if ($LASTEXITCODE -ne 0) { throw 'Could not clone LibOrbisPkg' }
}
git -C $src fetch -q origin
git -C $src reset -q --hard $commit
if ($LASTEXITCODE -ne 0) { throw "LibOrbisPkg commit $commit is not available" }
git -C $src apply --whitespace=nowarn (Join-Path $here 'changeinfo.patch')
if ($LASTEXITCODE -ne 0) { throw 'changeinfo.patch did not apply' }
dotnet publish (Join-Path $src 'PkgTool.Core\PkgTool.Core.csproj') -c Release -o $bin --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw 'Building PkgTool failed' }
Write-Host "Patched PkgTool ready: $bin\PkgTool.Core.exe"
