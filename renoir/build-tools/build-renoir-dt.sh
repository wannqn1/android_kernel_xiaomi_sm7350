#!/usr/bin/env bash
set -euo pipefail

tools=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$tools/../../.." && pwd)
kernel=$root/redwood-6.18-kernel
tree=$root/redwood-6.18-devicetrees
src=$tree/renoir
out=${1:-"$root/work/renoir-dt"}
mkdir -p "$out"

compile() {
    "${CC:-clang}" -E -P -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
        -I "$src/include" -I "$tree/bindings" -I "$kernel/include" \
        "$src/qcom/$1" -o "$out/$2.pp"
    "${DTC:-dtc}" -q -@ -I dts -O dtb -o "$out/$2" "$out/$2.pp"
}

case "${2:-}" in
    "") compile shima.dts shima.dtb ;;
    cvp) compile shima-cvp-test.dts shima.dtb ;;
    *) echo "Unknown Renoir DT variant: $2" >&2; exit 2 ;;
esac
compile renoir-sm7350-overlay.dts renoir.dtbo
"${FDTOVERLAY:-fdtoverlay}" -i "$out/shima.dtb" -o "$out/renoir-merged.dtb" "$out/renoir.dtbo"
fdtget_tool=${FDTGET:-fdtget}
[[ $("$fdtget_tool" -t x "$out/shima.dtb" / qcom,msm-id) == '1c2 10000' ]]
[[ $("$fdtget_tool" -t x "$out/renoir.dtbo" / qcom,msm-id) == '1c2 10000' ]]
[[ $("$fdtget_tool" -t x "$out/renoir.dtbo" / qcom,board-id) == '22 0' ]]
[[ $("$fdtget_tool" -t x "$out/renoir.dtbo" / xiaomi,board-id) == '6 0' ]]
[[ $("$fdtget_tool" -t s "$out/renoir-merged.dtb" /soc/ufshc@1d84000 status) == ok ]]
cvp_status=$("$fdtget_tool" -t s "$out/renoir-merged.dtb" /soc/qcom,cvp@ab00000 status)
if [[ ${2:-} == cvp ]]; then
    [[ "$cvp_status" == okay ]]
else
    [[ "$cvp_status" == disabled ]]
fi
python3 "$root/references/libufdt/utils/src/mkdtboimg.py" create "$out/dtbo.img" "$out/renoir.dtbo"
