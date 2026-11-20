#!/usr/bin/env bash
set -euo pipefail

tools=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$tools/../../.." && pwd)
work=${BUILD_DIR:-"$root/work/renoir-build"}
kernel=$root/redwood-6.18-kernel
modules=$root/redwood-6.18-modules
out=$work/out

export PATH="${CLANG_BIN:-/usr/bin}:$root/work/toolchain/cargo/bin:$root/work/toolchain/arch-tools/usr/bin:$PATH"
export RUSTUP_HOME="${RUSTUP_HOME:-$root/work/toolchain/rustup}"
export CARGO_HOME="${CARGO_HOME:-$root/work/toolchain/cargo}"
export RUSTUP_TOOLCHAIN="${RUSTUP_TOOLCHAIN:-1.88.0}"
export LIBCLANG_PATH="${LIBCLANG_PATH:-/usr/lib}"
export JOBS=${JOBS:-8}
export PAHOLE=${PAHOLE:-$(command -v pahole)}
export CLANG=${CLANG_BIN:-/usr/bin}
export KBUILD_BUILD_USER=renoir KBUILD_BUILD_HOST=renoir
export R=$work
export LD_LIBRARY_PATH="$root/references/dtc/libfdt${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export FDTOVERLAY="${FDTOVERLAY:-$(command -v fdtoverlay || printf %s "$root/references/dtc/fdtoverlay")}"
export FDTGET="${FDTGET:-$(command -v fdtget || printf %s "$root/references/dtc/fdtget")}"

if [[ "$root" =~ [[:space:]] || "$work" =~ [[:space:]] ]]; then
    echo "The inherited Qualcomm module builders require paths without spaces." >&2
    exit 1
fi

mkdir -p "$out" "$work/logs" "$work/vb"
kmake() {
    make -C "$kernel" O="$out" ARCH=arm64 LLVM=1 LLVM_IAS=1 LOCALVERSION= PAHOLE="$PAHOLE" "$@"
}

usage() {
    echo "Usage: $0 [kernel|dt|modules|stage|cvp] ..." >&2
    exit 2
}

if [[ $# == 0 ]]; then
    set -- kernel dt modules stage
fi

for step in "$@"; do
    case "$step" in
        kernel)
            kmake -s gki_defconfig vendor/redwood.config vendor/renoir.config >"$work/logs/config.log" 2>&1
            for option in CONFIG_RUST=y CONFIG_ASHMEM=y CONFIG_IOMMU_IO_PGTABLE_FAST=y \
                CONFIG_PINCTRL_SHIMA=m CONFIG_INTERCONNECT_QCOM_SHIMA=m CONFIG_SM_GCC_7350=m \
                CONFIG_PCI_MSM=m CONFIG_MHI_BUS=m; do
                grep -qx "$option" "$out/.config" || { echo "Missing kernel option $option" >&2; exit 1; }
            done
            kmake -j"$JOBS" Image modules scripts_dtc 2>&1 | tee "$work/logs/kernel.log"
            ;;
        dt)
            CC="$CLANG/clang" "$tools/build-renoir-dt.sh" "$work/dt"
            ;;
        modules)
            test -s "$out/Module.symvers"
            ln -sfnT "$kernel" "$work/redwood-6.18-kernel"
            ln -sfnT "$modules" "$work/redwood-6.18-modules"
            for task in vendor618 data618 wlan618 misc618; do
                WLAN_PROFILE=lahaina_gki_qca6490 bash "$modules/bringup/build-$task.sh"
            done 2>&1 | tee "$work/logs/vendor.log"
            bash "$modules/bringup/build-redwood618.sh" xiaomitouch btpower btfmslim \
                ioncompat irspi mithermal subsys_compat adsprpc video 2>&1 | tee -a "$work/logs/vendor.log"
            for driver in fpc goodix focaltech tfa9874 pm8008 mmhardware epss-l3; do
                source_dir=$modules/renoir/$driver
                touch_symbols=()
                case "$driver" in
                    goodix|focaltech)
                        touch_symbols=("KBUILD_EXTRA_SYMBOLS=$work/vb/rw/xiaomitouch/Module.symvers")
                        ;;
                esac
                kmake -j"$JOBS" M="$source_dir" MO="$work/vb/renoir/$driver" \
                    "${touch_symbols[@]}" modules 2>&1 | tee "$work/logs/$driver.log"
            done
            ;;
        cvp)
            source_dir=$modules/renoir/cvp
            cvp_build=$work/vb/renoir/cvp
            kmake -j"$JOBS" M="$source_dir/synx" MO="$cvp_build/synx" modules
            kmake -j"$JOBS" M="$source_dir/driver" MO="$cvp_build/driver" \
                "KBUILD_EXTRA_SYMBOLS=$cvp_build/synx/Module.symvers" modules
            CC="$CLANG/clang" "$tools/build-renoir-dt.sh" "$work/dt-cvp" cvp
            "$tools/stage-renoir-kernel.py" "$root" "$work" --cvp
            ;;
        stage)
            "$tools/stage-renoir-kernel.py" "$root" "$work"
            ;;
        *) usage ;;
    esac
done
