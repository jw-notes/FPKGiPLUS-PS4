#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of FPKGi+. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

# PS4 build: compile, sign (fself), assemble and package.
# Run inside WSL from anywhere; tools/build.ps1 calls it from Windows.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -n "${TOOLCHAIN:-}" ]]; then TC="$TOOLCHAIN"
elif [[ -d tools/toolchain ]]; then TC="$PWD/tools/toolchain"
else TC="$(cd ../Librarian/tools/toolchain && pwd)"; fi
export TOOLCHAIN="$TC"
SDK="$TC/OpenOrbis/PS4Toolchain"
export OO_PS4_TOOLCHAIN="$SDK"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
RELEASE_DIR="dist/$(python3 -c 'import json;print(json.load(open("version.json"))["release_directory"])')"
ELF="build/ps4/FPKGiPlus.elf"

make -j"$(nproc)" ps4
# The host log backend writes routine diagnostics to stderr. Capture those
# during validation so PowerShell's Stop policy does not treat a passing
# native-service fixture as a WSL invocation failure.
if [[ -d tests ]]; then
  if ! make -j"$(nproc)" native-tests > build/native-installer-validation.log 2>&1; then
    cat build/native-installer-validation.log
    exit 1
  fi
  echo "Native installer adapter checks passed"
else
  echo "tests/ not present: native installer adapter checks skipped"
fi
rm -rf build/pkg
mkdir -p build/pkg "$RELEASE_DIR"
"$SDK/bin/linux/create-fself" -in="$ELF" -out="${ELF%.elf}.oelf" --eboot build/pkg/eboot.bin \
  --paid 0x3800000000000011
python3 tools/validate_imports.py "${ELF%.elf}.oelf"
python3 tools/package.py
python3 tools/check_tls_assets.py build/pkg
cd build/pkg
if [[ -f /proc/sys/fs/binfmt_misc/WSLInterop ]]; then
  # The Windows PkgTool (needs the .NET runtime on Windows).
  export DOTNET_ROLL_FORWARD=LatestMajor
  export WSLENV="${WSLENV:+$WSLENV:}DOTNET_ROLL_FORWARD"
  "$SDK/bin/windows/PkgTool.Core.exe" pkg_build project.gp4 "../../$RELEASE_DIR"
else
  "$SDK/bin/linux/PkgTool.Core" pkg_build project.gp4 "../../$RELEASE_DIR"
fi
cd ../..
echo "Package written to $RELEASE_DIR"
