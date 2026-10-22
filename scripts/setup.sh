#!/usr/bin/env bash
# One-command, repeatable development setup for the INF2004 robot firmware.
#
#   bash scripts/setup.sh                  # pinned Pico SDK + ARM toolchain
#   bash scripts/setup.sh --toolchain apt  # Ubuntu packages (needs sudo) instead of the Arm tarball
#
# The repository is the course project template: the micro T-Kernel port,
# drivers and build files are committed here and the robot application is in
# app_program/. Setup only fetches what the template expects from outside:
# the Pico C SDK (with its lwIP and CYW43 submodules) and the Arm compiler.
# They live OUTSIDE the repository (default ~/inf2004-deps).
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: bash scripts/setup.sh [--deps DIR] [--toolchain auto|download|path|apt|none]
                             [--quiet]

  --deps DIR        Directory for the Pico SDK and toolchain.
                    Default: $ROBOT_DEPS_DIR, else ~/inf2004-deps.
                    Must not contain spaces (GNU Make limitation).
                    scripts/dev.sh reads only $ROBOT_DEPS_DIR (default
                    ~/inf2004-deps); export ROBOT_DEPS_DIR when you use --deps.
  --toolchain MODE  auto (default): use arm-none-eabi-gcc from PATH only if
                    it is the pinned Arm GNU Toolchain 14.2.Rel1; otherwise
                    use or download the pinned tarball in DIR/toolchain
                    (checksum verified, no root required).
                    download: always use the pinned tarball in DIR/toolchain.
                    path: use whatever arm-none-eabi-gcc is on PATH (warns
                    when it is not the pinned toolchain that CI uses).
                    apt: sudo apt-get install gcc-arm-none-eabi (Ubuntu/Debian;
                    not the pinned toolchain).
                    none: provide no ARM compiler (host tests only).
  --quiet           Less progress output.
USAGE
}

log() { printf '[setup] %s\n' "$*"; }
die() { printf '[setup] ERROR: %s\n' "$*" >&2; exit 1; }

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        printf 'This project builds with GNU Make, GCC sanitizers and a Linux\n'
        printf 'shell. On Windows, open Ubuntu (WSL), change to this repository\n'
        printf '(for example cd /mnt/c/<path-to-repository>) and run the same\n'
        printf 'command there:  bash scripts/setup.sh\n'
        printf 'README.md describes the WSL setup.\n'
        exit 2 ;;
esac

deps_dir="${ROBOT_DEPS_DIR:-$HOME/inf2004-deps}"
toolchain_mode=auto
quiet=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --deps) [[ $# -ge 2 ]] || die "--deps needs a directory"; deps_dir=$2; shift 2 ;;
        --toolchain) [[ $# -ge 2 ]] || die "--toolchain needs a mode"; toolchain_mode=$2; shift 2 ;;
        --quiet) quiet=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option: $1" ;;
    esac
done
case "$toolchain_mode" in auto|download|path|apt|none) ;; *) die "bad --toolchain mode: $toolchain_mode" ;; esac
[[ "$deps_dir" =~ [[:space:]] ]] && die "dependency directory must not contain spaces: $deps_dir"

# ---- prerequisites --------------------------------------------------------
missing=()
for tool in git python3 gcc g++ make tar; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    printf '[setup] Missing tools: %s\n' "${missing[*]}" >&2
    printf 'On Ubuntu/WSL install them with:\n' >&2
    printf '  sudo apt-get update && sudo apt-get install -y build-essential python3 git xz-utils curl\n' >&2
    exit 1
fi
python3 - <<'PY' || die "Python 3.10 or newer is required"
import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)
PY

# ---- pins -----------------------------------------------------------------
# Pico SDK 2.2.0 and the lwIP / CYW43 driver revisions it ships with. The
# template README asks for SDK 2.2.0; firmware images are compared on these.
SDK_URL="https://github.com/raspberrypi/pico-sdk.git"
SDK_COMMIT="a1438dff1d38bd9c65dbd693f0e5db4b9ae91779"
LWIP_COMMIT="77dcd25a72509eb83f72b033d219b1d40cd8eb95"
CYW43_COMMIT="dd7568229f3bf7a37737b9e1ef250c26efe75b23"

# Arm GNU Toolchain used for local and CI target builds (no root required).
# SHA-256 values are checked before extraction. Empty means "not pinned yet".
# TOOLCHAIN_BANNER is the text the pinned compiler prints in `--version`;
# firmware/scripts/build_target.sh records whether a build used it.
TOOLCHAIN_VERSION="14.2.rel1"
TOOLCHAIN_BANNER="14.2.Rel1"
toolchain_sha256() {
    case "$1" in
        x86_64)  printf '%s' "62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823" ;;
        aarch64) printf '%s' "87330bab085dd8749d4ed0ad633674b9dc48b237b61069e3b481abd364d0a684" ;;
        *) printf '' ;;
    esac
}

sdk_dir="$deps_dir/pico-sdk"
toolchain_dir="$deps_dir/toolchain"
mkdir -p "$deps_dir"

# ---- Pico SDK -------------------------------------------------------------
ensure_checkout() {
    local label=$1 url=$2 dir=$3 commit=$4 head=""
    if [[ -d "$dir/.git" ]]; then
        head=$(git -C "$dir" rev-parse --verify --quiet HEAD 2>/dev/null || true)
    else
        rm -rf -- "$dir"
        log "cloning $label"
        git clone --quiet "$url" "$dir"
    fi
    if [[ "$head" != "$commit" ]]; then
        git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null || git -C "$dir" fetch --quiet origin
        git -C "$dir" checkout --quiet --detach "$commit"
    fi
    [[ "$(git -C "$dir" rev-parse HEAD)" == "$commit" ]] || die "$label is not at $commit"
    log "$label at $(git -C "$dir" rev-parse --short HEAD) ($dir)"
}

ensure_checkout "Pico SDK 2.2.0" "$SDK_URL" "$sdk_dir" "$SDK_COMMIT"
git -C "$sdk_dir" submodule update --quiet --init lib/cyw43-driver lib/lwip
for pair in "lib/lwip:$LWIP_COMMIT" "lib/cyw43-driver:$CYW43_COMMIT"; do
    sub=${pair%%:*}
    want=${pair#*:}
    have=$(git -C "$sdk_dir/$sub" rev-parse HEAD)
    [[ "$have" == "$want" ]] || die "SDK submodule $sub is at $have, expected $want"
done
log "SDK submodules lib/lwip and lib/cyw43-driver at their pinned commits"

# ---- ARM toolchain ---------------------------------------------------------
arm_bin=""
path_compiler_banner() {
    arm-none-eabi-gcc --version 2>/dev/null | head -n 1 || true
}
warn_not_pinned() {
    log "WARNING: $1 is not the pinned Arm GNU Toolchain $TOOLCHAIN_BANNER that CI uses."
    log "WARNING: images built with it are valid builds but their hashes are not comparable with CI."
}
provide_toolchain() {
    local arch name url archive expected actual banner
    [[ "$toolchain_mode" == none ]] && return 0
    if [[ "$toolchain_mode" == path ]]; then
        command -v arm-none-eabi-gcc >/dev/null 2>&1 || die "--toolchain path: no arm-none-eabi-gcc on PATH"
        arm_bin=$(cd -- "$(dirname -- "$(command -v arm-none-eabi-gcc)")" && pwd -P)
        banner=$(path_compiler_banner)
        log "using arm-none-eabi-gcc from PATH (--toolchain path): $banner"
        [[ "$banner" == *"$TOOLCHAIN_BANNER"* ]] || warn_not_pinned "this compiler"
        return 0
    fi
    if [[ "$toolchain_mode" == auto ]] && command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        banner=$(path_compiler_banner)
        if [[ "$banner" == *"$TOOLCHAIN_BANNER"* ]]; then
            arm_bin=$(cd -- "$(dirname -- "$(command -v arm-none-eabi-gcc)")" && pwd -P)
            log "using the pinned arm-none-eabi-gcc from PATH: $banner"
            return 0
        fi
        warn_not_pinned "arm-none-eabi-gcc on PATH ($banner)"
        log "using the pinned toolchain in $toolchain_dir instead (pass --toolchain path to use the PATH compiler anyway)"
    fi
    if [[ "$toolchain_mode" == apt ]]; then
        command -v apt-get >/dev/null 2>&1 || die "--toolchain apt needs apt-get"
        log "installing gcc-arm-none-eabi with apt (sudo)"
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
        arm_bin=$(cd -- "$(dirname -- "$(command -v arm-none-eabi-gcc)")" && pwd -P)
        banner=$(path_compiler_banner)
        [[ "$banner" == *"$TOOLCHAIN_BANNER"* ]] || warn_not_pinned "the apt compiler ($banner)"
        return 0
    fi
    arch=$(uname -m)
    name="arm-gnu-toolchain-${TOOLCHAIN_VERSION}-${arch}-arm-none-eabi"
    if [[ -x "$toolchain_dir/$name/bin/arm-none-eabi-gcc" ]]; then
        arm_bin="$toolchain_dir/$name/bin"
        log "using downloaded toolchain in $arm_bin"
        return 0
    fi
    expected=$(toolchain_sha256 "$arch")
    url="https://developer.arm.com/-/media/Files/downloads/gnu/${TOOLCHAIN_VERSION}/binrel/${name}.tar.xz"
    mkdir -p "$toolchain_dir"
    archive="$toolchain_dir/$name.tar.xz"
    if [[ ! -f "$archive" ]]; then
        log "downloading Arm GNU Toolchain ${TOOLCHAIN_VERSION} (${arch}, about 150 MB)"
        if command -v curl >/dev/null 2>&1; then
            curl -fL --retry 3 --silent --show-error -o "$archive.part" "$url"
        elif command -v wget >/dev/null 2>&1; then
            wget -q -O "$archive.part" "$url"
        else
            die "curl or wget is required to download the toolchain"
        fi
        mv -- "$archive.part" "$archive"
    fi
    actual=$(sha256sum "$archive" | cut -d' ' -f1)
    if [[ -z "$expected" ]]; then
        log "WARNING: no pinned SHA-256 for $arch; downloaded archive has SHA-256 $actual"
        log "Pin it in scripts/setup.sh (toolchain_sha256) so future runs verify the download."
    elif [[ "$actual" != "$expected" ]]; then
        rm -f -- "$archive"
        die "toolchain archive SHA-256 mismatch (got $actual); deleted, please rerun"
    else
        log "toolchain archive SHA-256 verified"
    fi
    log "extracting toolchain into $toolchain_dir"
    tar -xJf "$archive" -C "$toolchain_dir"
    [[ -x "$toolchain_dir/$name/bin/arm-none-eabi-gcc" ]] || die "toolchain extraction failed"
    arm_bin="$toolchain_dir/$name/bin"
}
provide_toolchain

# ---- environment file sourced by scripts/dev.sh ---------------------------
# PICO_SDK_PATH is the variable the template's build_make/makefile reads.
env_file="$deps_dir/env.sh"
{
    printf '# Generated by scripts/setup.sh; source this or use scripts/dev.sh.\n'
    printf 'export ROBOT_DEPS_DIR=%q\n' "$deps_dir"
    printf 'export ROBOT_SDK=%q\n' "$sdk_dir"
    printf 'export PICO_SDK_PATH=%q\n' "$sdk_dir"
    if [[ -n "$arm_bin" ]]; then
        printf 'case ":$PATH:" in *:%q:*) ;; *) export PATH=%q:"$PATH" ;; esac\n' "$arm_bin" "$arm_bin"
    fi
} > "$env_file"
if [[ -d "$deps_dir/mtk3smp-rp2040" ]]; then
    log "note: $deps_dir/mtk3smp-rp2040 (the old generated port checkout) is no longer used; you may delete it"
fi

# ---- summary ---------------------------------------------------------------
# scripts/dev.sh finds the dependencies only through $ROBOT_DEPS_DIR (default
# ~/inf2004-deps), so a --deps directory elsewhere must be exported first.
if [[ "$deps_dir" == "${ROBOT_DEPS_DIR:-$HOME/inf2004-deps}" ]]; then
    env_hint="source $env_file   (scripts/dev.sh does this for you)"
else
    env_hint="source $env_file
  Before scripts/dev.sh:  export ROBOT_DEPS_DIR=$deps_dir"
fi
if [[ $quiet -eq 0 ]]; then
    cat <<SUMMARY

Setup complete.
  Project:       $repo_root
  Dependencies:  $deps_dir
  Pico SDK:      $sdk_dir
  Toolchain:     ${arm_bin:-none (host tests only)}
  Environment:   $env_hint

Next:
  bash scripts/dev.sh host-tests        # portable module tests, release + ASan/UBSan
  bash scripts/dev.sh style             # selected BARR-C style checks
  bash scripts/dev.sh app-tests         # application scenario tests
  bash scripts/dev.sh integration       # board IRQ, image, build, radio and network host tests
  bash scripts/dev.sh build offline     # uncommissioned offline firmware image
  bash scripts/dev.sh build mqtt --compile-only
SUMMARY
fi
