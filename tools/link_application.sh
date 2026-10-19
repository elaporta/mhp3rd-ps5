#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Uses the pinned PS5_Vulkan link recipe, Copyright (C) 2026 Mihawk-99.
set -eu
root=$1; build=$2; sdk=$3; radv=$4; aot=$5; tool=$6; entry=$7
shift 7
driver="$root/deps/PS5_Vulkan"
out="$build/link"
mkdir -p "$out/stubs" "$out/driver-link/tooling/native" "$out/driver-link/tooling/psbc"
export PS5_PAYLOAD_SDK="$sdk" PS5_CLANG=clang-18
export PATH="$sdk/bin:/usr/lib/llvm-18/bin:$PATH"
cc() { sh "$driver/tooling/prospero-clang18" "$@"; }
for item in 'tooling/native/ps5-pie.ld' 'tooling/native/app-symbols.map' 'tooling/psbc/ps5-pie-unwind.ld'; do
    cp "$driver/$item" "$out/driver-link/$item"
done
for module in libSceAgc libSceAgcDriver; do
    source=agc_canary_link_stub.c
    [ "$module" != libSceAgcDriver ] || source=agc_driver_canary_link_stub.c
    cc -x c -O2 -fPIC -c "$driver/vendor/ps5/sdk/stubs/$source" -o "$out/stubs/$module.o"
    "$sdk/bin/prospero-lld" --shared -soname "$module.prx" "$out/stubs/$module.o" -o "$out/stubs/$module.so"
done
source "$driver/tools/radv-link.sh"
radv_link_recipe "$out/driver-link" "$sdk" "$radv/lib/libvulkan_radeon.ps5.a"
radv_link_flags+=(--gc-sections --wrap=lstat --wrap=exit --version-script "$root/tools/native-symbols.map")
objects=("$@")
link_app() {
    "$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" "$@" \
        --version-script "$driver/tooling/native/app-symbols.map" --exclude-libs=ALL -e _start \
        -Map="$out/link.map" -o "$out/requirements.elf" "${objects[@]}" "$entry" "$build/game_tls.o" \
        --start-group "$build/libfirst_frame_profile.a" "$aot" "$build/libfirst_frame_framework.a" \
        "$build/libfirst_frame_files.a" "$build/codecs/install/lib/libavcodec.a" "$build/codecs/install/lib/libavutil.a" --end-group \
        "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" "${radv_link_inputs[@]}" \
        --no-as-needed "$sdk/target/lib/libSceImeDialog.so" --as-needed "$sdk"/target/lib/*.so
}
link_app
llvm-nm-18 -D --undefined-only "$out/requirements.elf" > "$out/first-imports.txt"
python3 "$root/tools/native_weak_symbols.py" "$out/first-imports.txt" "$out/optional-weak.ld"
link_app -T "$out/optional-weak.ld"
llvm-nm-18 -D --undefined-only "$out/requirements.elf" > "$out/imports.txt"
python3 "$root/tools/native_weak_symbols.py" --validate "$out/imports.txt" --global-ime
llvm-readelf-18 -h -l -d -S "$out/requirements.elf" > "$out/elf-inspect.txt"
llvm-nm-18 --defined-only "$out/requirements.elf" > "$out/symbols.txt"
for symbol in __eh_frame_start __eh_frame_end __eh_frame_hdr_start __eh_frame_hdr_end _Unwind_RaiseException __cxa_throw; do
    grep -q "$symbol" "$out/symbols.txt"
done
"$tool" link --in "$out/requirements.elf" --out "$out/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$out/stubs/libSceAgc.so" --stub "$out/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$out/eboot.elf" --out "$out/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$out/eboot.bin" > "$out/self-inspect.txt"
