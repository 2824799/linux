# Xaga handset kernel

`main` is the complete source for the local handset: device adaptations,
USB and charging fixes, display support, VCP codecs and current power
management work. Keep the local `main` tracking `origin/main`.

The remote `视频编解码器` branch is a contribution series based on
`MT6895-Mainline/linux`, branch `7.2-mt6895-xiaomi-xaga`. Its reviewed
commits are also ancestors of `main`. `scripts/xaga-video-commits` records
their order; each commit can be selected directly from `main`.

Develop and commit features on `main` first. Update the contribution
series only with complete, tested changes and all required dependencies.
Use a temporary detached worktree to prepare it, and remove that worktree
after publishing. Keep only `main` as a local development branch.

## Reproducible handset build

The handset has the L16_42 panel. `xaga_nahida_defconfig` preserves its
working configuration and enables the real DVFSRC provider. It excludes
unrelated SoC audio platforms which previously needed command-line
overrides. Both panel variants remain in the source; switching hardware
requires matching the configuration, DSI graph and framebuffer metadata.

With Clang, LLVM and LLD on PATH, build from the repository root:

```sh
scripts/build-xaga.sh /absolute/build/output /absolute/initramfs/root
```

The initramfs, firmware and userspace are separate inputs. The repository
does not embed paths to one workstation. The script builds the board DTB,
Image.gz and modules and writes `build-manifest.json` with source revision
and artifact hashes. The release is `7.2.0-linux-nahida`.

The embedded DTB is built in the output directory through Kbuild, from
the same board source as the standalone DTB. The handset reserved-memory
layout and framebuffer handover data are part of `main`.

## Codec contribution scope

The current series contains DT bindings, the IOMMU fixes needed by the
codecs, the VCP transport, H.264 encoding and decoding, and visible-size
and resolution-change handling. Commits are separated by subsystem and
ordered so their build dependencies precede their users.

The decoder currently supports one active H.264 8-bit 4:2:0 session, with
CPU conversion from firmware MM21 reference frames to NV12/NV12M. The
encoder uses coherent staging copies. Other codec formats and zero-copy
decoding remain future work.

Dynamic codec voltage/frequency selection remains on `main`. The current
contribution series uses the earlier fixed-rate implementation, which
does not depend on the unmerged DVFSRC changes. A future DVFS contribution
must include its provider, bindings, DT configuration and validated
shared-rail behavior as a complete series.

## The 725 mV VCORE constraint

The `regulator-min-microvolt = <725000>` setting in the handset DTS is a
temporary board policy retained from the working device build. VCORE is
shared by multimedia consumers. This setting clamps voltage requests,
including a codec request below 725 mV, to the board floor. It is not the
minimum voltage intrinsically required by the video codec.

The historical measurements do not establish 725 mV as the minimum safe
voltage in every display state. Display runtime OPP voting and voltage
requirements still need investigation. Preserve the working handover
until that work is validated; this constraint is outside the codec
contribution series.
