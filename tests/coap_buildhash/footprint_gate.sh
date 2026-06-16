#!/usr/bin/env bash
#
# CoAP frontend footprint gate.
#
# Asserts that an app built with CONFIG_ZEPHLETS_COAP=n carries a runtime
# image (.text/.rodata/.data/.bss) byte-identical to the recorded
# baseline, i.e. the CoAP frontend adds zero footprint when disabled. The
# only frontend-aware change to core infra is the ZEPHLET_NEW_PRIO hook,
# which expands to nothing when no frontend is opted in; this gate is what
# keeps that promise honest as the frontend grows.
#
# Hash function:
#   objcopy --only-section .text --only-section .rodata \
#           --only-section .data  --only-section .bss   in.elf out
#   sha256(out)
#
# Reproducibility:
#   - Board mps2/an385 is an ARM cross-compile target, so the image is
#     independent of the build host's architecture.
#   - CONFIG_BUILD_OUTPUT_STRIP_PATHS=y (Zephyr default) keeps __FILE__
#     paths out of .rodata, so the image is independent of the build dir.
#   - objcopy is taken from the build's own CMakeCache, so the gate uses
#     the exact toolchain that produced the ELF.
# The baseline is therefore tied to the pinned Zephyr SDK only.
# Regenerate after an intentional SDK bump (see README.md).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="mps2_an385"
BASELINE="${HERE}/baseline.${BOARD}.txt"

usage() {
	echo "usage: $0 {hash|check|update} <build_dir>" >&2
	exit 2
}

[ $# -eq 2 ] || usage
cmd="$1"
build_dir="$2"

elf="${build_dir}/zephyr/zephyr.elf"
cache="${build_dir}/CMakeCache.txt"
[ -f "$elf" ]   || { echo "error: ELF not found: $elf" >&2; exit 1; }
[ -f "$cache" ] || { echo "error: CMakeCache not found: $cache" >&2; exit 1; }

objcopy="$(sed -n 's/^CMAKE_OBJCOPY:FILEPATH=//p' "$cache")"
{ [ -n "$objcopy" ] && [ -x "$objcopy" ]; } || {
	echo "error: objcopy not resolved from $cache (got '${objcopy}')" >&2
	exit 1
}

sha256_of() {
	# Portable: coreutils sha256sum (Linux/CI) or shasum -a 256 (macOS).
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	else
		shasum -a 256 "$1" | awk '{print $1}'
	fi
}

compute_hash() {
	local out
	out="$(mktemp)"
	trap 'rm -f "$out"' RETURN
	"$objcopy" --only-section .text --only-section .rodata \
	           --only-section .data --only-section .bss \
	           "$elf" "$out"
	sha256_of "$out"
}

case "$cmd" in
hash)
	compute_hash
	;;
update)
	h="$(compute_hash)"
	printf '%s\n' "$h" > "$BASELINE"
	echo "baseline updated (${BOARD}): ${h}"
	;;
check)
	[ -f "$BASELINE" ] || {
		echo "error: no baseline at $BASELINE — run '$0 update <build_dir>'" >&2
		exit 1
	}
	want="$(cat "$BASELINE")"
	got="$(compute_hash)"
	if [ "$got" = "$want" ]; then
		echo "PASS: =n section hash matches baseline (${BOARD}): ${got}"
	else
		echo "FAIL: =n section hash drift (${BOARD})" >&2
		echo "  baseline: ${want}" >&2
		echo "  current:  ${got}" >&2
		echo "CONFIG_ZEPHLETS_COAP=n added footprint vs the recorded baseline." >&2
		echo "If this is an intentional SDK/toolchain change, regenerate the" >&2
		echo "baseline (see tests/coap_buildhash/README.md)." >&2
		exit 1
	fi
	;;
*)
	usage
	;;
esac
