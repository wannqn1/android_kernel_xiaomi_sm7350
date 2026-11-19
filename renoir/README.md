# Build Linux 6.18 for renoir

Run this from a dedicated Linux kernel workspace. The clone destination names
match those expected by the Qualcomm module build scripts.

```sh
git clone --depth 1 -b renoir-6.18 https://github.com/wannqn1/android_kernel_xiaomi_sm7350.git redwood-6.18-kernel
git clone --depth 1 -b renoir-6.18 https://github.com/wannqn1/android_kernel_xiaomi_sm7350-devicetrees.git redwood-6.18-devicetrees
git clone --depth 1 -b renoir-6.18 https://github.com/wannqn1/android_kernel_xiaomi_sm7350-modules.git redwood-6.18-modules
git clone --depth 1 -b lineage-24.0 https://github.com/LineageOS/android_system_libufdt.git references/libufdt
```

The port was built with LLVM 22.1.8, Rust 1.88.0, bindgen 0.72.1 and pahole
1.32. Install the standard Linux kernel build dependencies, rsync, kmod, and
the device-tree compiler tools (`dtc`, `fdtoverlay`, `fdtget`). The scripts
use system LLVM by default. Set `CLANG_BIN` and `LIBCLANG_PATH` for an LLVM
installation in a different location.

```sh
export RUSTUP_HOME="$PWD/work/toolchain/rustup"
export CARGO_HOME="$PWD/work/toolchain/cargo"
mkdir -p "$RUSTUP_HOME" "$CARGO_HOME"
rustup toolchain install 1.88.0 --profile minimal -c rust-src -c rustfmt
cargo +1.88.0 install --locked bindgen-cli --version 0.72.1

JOBS=8 redwood-6.18-kernel/renoir/build-tools/build-renoir-kernel.sh kernel dt modules stage
```

The output goes into `device/xiaomi/renoir-kernel-6.18` under this workspace.
Replace the contents of that path in the Android checkout with the matched
Image, DTB, DTBO, and vendor ramdisk modules.
