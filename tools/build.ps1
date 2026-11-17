# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of FPKGi+. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

# Builds the PS4 package from Windows: runs tools/build.sh in WSL, then
# validates the .pkg with PkgTool and prints its SHA-256.
#   .\tools\build.ps1                    (the default WSL distribution)
#   .\tools\build.ps1 -Distribution Ubuntu-24.04
param([string]$Distribution = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$linuxRoot = '/mnt/' + $root.Substring(0, 1).ToLower() + $root.Substring(2).Replace('\', '/')
if ($Distribution) { & wsl -d $Distribution -- bash "$linuxRoot/tools/build.sh" }
else { & wsl -- bash "$linuxRoot/tools/build.sh" }
if ($LASTEXITCODE -ne 0) { throw "PS4 build failed ($LASTEXITCODE)" }
$env:DOTNET_ROLL_FORWARD = 'LatestMajor'
$version = Get-Content (Join-Path $root 'version.json') -Raw | ConvertFrom-Json
$folder = $version.release_directory
$pkg = Join-Path $root ('dist\' + $folder + '\IV0000-FPKP00001_00-FPKGIPLUS0000000.pkg')
# The OpenOrbis toolchain: this project's, or the one shared with Librarian.
$tc = Join-Path $root 'tools\toolchain'
if (-not (Test-Path $tc)) { $tc = Join-Path (Split-Path $root -Parent) 'Librarian\tools\toolchain' }
& (Join-Path $tc 'OpenOrbis\PS4Toolchain\bin\windows\PkgTool.Core.exe') pkg_validate $pkg
if ($LASTEXITCODE -ne 0) { throw 'PKG validation failed' }
Get-FileHash -LiteralPath $pkg -Algorithm SHA256
