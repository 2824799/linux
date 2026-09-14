#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

source_dir=$(cd "$(dirname "$0")/.." && pwd)
output_dir=${1:?usage: build-xaga.sh OUTPUT_DIR INITRAMFS_DIR}
initramfs_dir=${2:?usage: build-xaga.sh OUTPUT_DIR INITRAMFS_DIR}
mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
initramfs_dir=$(cd "$initramfs_dir" && pwd)

if [[ "$output_dir" == "$source_dir" ]]; then
    echo "Use a separate output directory" >&2
    exit 1
fi

make_args=(-C "$source_dir" O="$output_dir" ARCH=arm64 LLVM=1 LLVM_IAS=1 LOCALVERSION=)
make "${make_args[@]}" xaga_nahida_defconfig
"$source_dir/scripts/config" --file "$output_dir/.config" \
    --set-str INITRAMFS_SOURCE "$initramfs_dir"
make "${make_args[@]}" olddefconfig
make "${make_args[@]}" -j"${JOBS:-12}" \
    mediatek/mt6895-xiaomi-xaga.dtb Image.gz modules

release=$(cat "$output_dir/include/config/kernel.release")
[[ "$release" == "7.2.0-linux-nahida" ]] || {
    echo "Unexpected kernel release: $release" >&2
    exit 1
}

python3 - "$source_dir" "$output_dir" <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import sys

source, output = map(Path, sys.argv[1:])
files = [".config", "arch/arm64/boot/Image.gz",
         "arch/arm64/boot/dts/mediatek/mt6895-xiaomi-xaga.dtb",
         "arch/arm64/kernel/xaga-board.dtb", "Module.symvers"]
files += sorted(str(p.relative_to(output)) for p in output.rglob("*.ko"))
manifest = {
    "commit": subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
    "dirty": bool(subprocess.check_output(
        ["git", "-C", str(source), "status", "--porcelain"])),
    "release": (output / "include/config/kernel.release").read_text().strip(),
    "sha256": {name: hashlib.sha256((output / name).read_bytes()).hexdigest()
               for name in files},
}
(output / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY
echo "BUILD_OK release=$release output=$output_dir"
