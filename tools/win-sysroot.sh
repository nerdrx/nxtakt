#!/usr/bin/env bash
# Populate build-mingw/sysroot/ with the Windows libraries the GUI needs.
#
#   tools/win-sysroot.sh                 fetch + extract into build-mingw/sysroot
#   tools/win-sysroot.sh --dest DIR      somewhere else
#   tools/win-sysroot.sh --verify        do not fetch; just report what is there
#   tools/win-sysroot.sh --toolchain     ALSO fetch a cross-compiler (see below)
#
# Why this exists
# ---------------
# Debian/Ubuntu and Arch cross-package a compiler but no cross-built libraries:
# there is no mingw freetype, no mingw sndfile, no mingw samplerate anywhere in
# apt or pacman. docs/PORTING.md used to recommend vcpkg for this. MSYS2 turns
# out to be much cheaper, and the reason the old note rejected it -- "pacman
# only runs on Windows" -- is simply not true of the *packages*: an MSYS2
# package is a zstd-compressed tar of a `ucrt64/` prefix, served over plain
# HTTPS, with no installer and no Windows host in the loop. curl and tar are
# the whole tool requirement.
#
# UCRT, not msvcrt
# ----------------
# This fetches from mingw/ucrt64, NOT mingw/mingw64, and that choice is load
# bearing. Which C runtime a mingw-w64 toolchain targets is fixed when the CRT
# is built; current Arch (mingw-w64-crt 14) defaults to UCRT, Debian's
# mingw-w64 has also moved to UCRT. Check yours with:
#
#     echo 'int main(){}' | x86_64-w64-mingw32-gcc -x c - -o /tmp/t.exe
#     x86_64-w64-mingw32-objdump -p /tmp/t.exe | grep -i 'DLL Name'
#
# api-ms-win-crt-*.dll means UCRT (use ucrt64, as pinned here); msvcrt.dll means
# the old runtime (switch REPO to mingw/mingw64 and repin, or the two halves of
# the program will end up with two heaps and two stdio states).
#
# Pinning
# -------
# Every package is pinned to an exact version AND its sha256, so this script
# fetches the same bytes today and in a year. MSYS2's mirrors delete old
# versions eventually; when a URL 404s, bump the version and the hash together
# and say so in the commit. Nothing here auto-resolves "latest" on purpose --
# a build that silently changes its own dependencies is not reproducible.
#
# What the closure is, and why it is bigger than four libraries
# -------------------------------------------------------------
# NxTakt itself wants freetype, sndfile and samplerate (glew is not used --
# src/gfx/gl.h loads GL entry points directly). The rest of this list is what
# those three drag in:
#
#   freetype   -> harfbuzz, libpng, zlib, bzip2, brotli
#   harfbuzz   -> glib2, graphite2, and (being C++) libstdc++/libgcc
#   glib2      -> gettext-runtime (libintl), libiconv, pcre2, libffi
#   sndfile    -> FLAC, ogg, vorbis, opus, mpg123, lame
#
# MSYS2 builds freetype against harfbuzz -- that is where the glib tail comes
# from, and it is not optional short of building freetype ourselves. It costs
# a few MB of DLLs next to the .exe and nothing else.
#
# Shared, not static, also on purpose: a static libsndfile needs the whole
# FLAC/ogg/vorbis/opus/mpg123 link order to be right, and a static freetype
# still wants harfbuzz's symbols. The import libraries plus a `make -f
# Makefile.mingw dlls` copy step sidestep the entire question.
set -uo pipefail
cd "$(dirname "$0")/.."

REPO=https://mirror.msys2.org/mingw/ucrt64
DEST=build-mingw/sysroot
VERIFY_ONLY=0
WANT_TOOLCHAIN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dest) DEST="$2"; shift 2 ;;
        --verify) VERIFY_ONLY=1; shift ;;
        --toolchain) WANT_TOOLCHAIN=1; shift ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1"; exit 2 ;;
    esac
done

# name-version-rel                                sha256
# ---------------------------------------------------------------------------
# Pinned 2026-08-15 against mirror.msys2.org/mingw/ucrt64.
PKGS=(
  "freetype-2.14.3-1                              f502bdf9ed07aa95e223e1aba17ce8cb2d975aab5066eec17a2d7dc4fb8a1c20"
  "harfbuzz-14.3.1-1                              b1dc4a9b4e7ae5449f7bbe75b7127ddba18a85bd8e55c5265ad146cde6a9a8bb"
  "graphite2-1.3.15-1                             34ca1f88bff10ef182a6b1c8e5622d5029f13b05748e361af13b552b422ea849"
  "glib2-2.88.3-1                                 0733a674ecf5282088158b8e458cb2ad3359bf42fb7dbe9561b4bcfe0267f5bf"
  "gettext-runtime-1.0-1                          ba693dda4ac375af76ce481ff3a6e7481286546cc7dc6d56c7021dae34084157"
  "libiconv-1.19-1                                9a500f38c2b91808741c62fae746b3e9110b33a1ecf5c30fa0c66dbedddf7e16"
  "pcre2-10.47-1                                  839bc4642f94c44e94e331c9092c6d186b1edc54dfdf6a81cb2062f638417023"
  "libffi-3.8.0-1                                 99ad12f4ecfa00a889ef9c5c0188592368a5bac109d0c508ce4e04f29ea95a76"
  "libpng-1.6.58-1                                bbfb6eb6246b01df90d82d6cff84f0a4e98dad029a4a2753898c96fcaadb66c7"
  "zlib-1.3.2-2                                   841401182976d2f9e17e5c0ebaac51f2a8014140ea53d67625e91c8fb3c85ea0"
  "bzip2-1.0.8-4                                  f03a2174034ddd2d96cecd34f617c5f8e2ef86c812b8d2bb3b8875257f2c8bfa"
  "brotli-1.2.0-1                                 9cc89665496ea504751476eafba15c71c554aaf3004babc2004f7c07ffd60514"
  "libsndfile-1.2.2-1                             a882301af8cb1f645c45b1dcbf9634a9bf5a92af47f0cef95a88e83d3a3f444b"
  "flac-1.5.0-2                                   28e8637795bd7d80289ddbb582d6ad350b73eac64390b877149717dd3757ad79"
  "libogg-1.3.6-1                                 110a34239e8f122da15e041b2601e2f3c6d4c909d738bdf978700601e0f64449"
  "libvorbis-1.3.7-3                              d013d2c7489b175299f9ea8bd9b70cf456337943b0f13e6eb470beb6c3fc251f"
  "opus-1.6.1-1                                   7b8f176f9cc0431b52006861c0a2e083d73d4b47a73ab749755b5379ff673252"
  "mpg123-1.33.7-1                                7d4efa8ab7682476f0b8b048658596e9b64d8115cd9706afbba0a5410792e240"
  "lame-3.100-3                                   6bc4bf7d2f39a941a695b228818a8c846a53c43c20117e0707e08977b0dd0da9"
  "libsamplerate-0.2.2-1                          05a678fb77cc8787e4073907b36357dd985fe06788adc8cc3216a851ee243bf8"
  "gcc-libs-16.2.0-3                              5763fabf86fa13a4449ee765006d3446384ed66af7bf827459710eb777e0b11c"
  "libwinpthread-14.0.0.r262.g5ea8e9fac-1         320c204dc7d91988037c61cac93b513ce65504ab9d6f8a841e9f8782997a5c7a"
)

# Optional, and NOT part of the normal path: a cross-compiler, for a machine
# that has none and no way to install one (no root, or a distro that does not
# package mingw-w64 at all). CI installs mingw-w64 from apt and never touches
# this. Arch's packages are used because they are relocatable -- GCC computes
# its own prefix from argv[0], so an extracted usr/ tree works from anywhere as
# long as it stays intact.
TC_REPO=https://geo.mirror.pkgbuild.com/extra/os/x86_64
TC_PKGS=(
  "mingw-w64-binutils-2.46.0-1-x86_64"
  "mingw-w64-headers-14.0.0-1-any"
  "mingw-w64-crt-14.0.0-1-any"
  "mingw-w64-winpthreads-14.0.0-1-any"
  "mingw-w64-gcc-16.1.0-1-x86_64"
)

CACHE="$DEST/.cache"
mkdir -p "$CACHE"

need() { command -v "$1" >/dev/null || { echo "*** $1 is required"; exit 1; }; }
need curl
need tar
need sha256sum

fetch() {  # fetch <url> <path> <sha256|->
    local url="$1" out="$2" want="$3"
    if [[ -f "$out" && "$want" != "-" ]]; then
        local got; got=$(sha256sum "$out" | cut -d' ' -f1)
        [[ "$got" == "$want" ]] && return 0
        echo "    cached copy has the wrong hash, refetching"
        rm -f "$out"
    fi
    curl -sSfL --retry 3 --retry-delay 2 -o "$out" "$url" || {
        echo "*** download failed: $url"
        echo "    MSYS2 prunes old versions; if this is a 404, repin the version AND its"
        echo "    sha256 in $0 together."
        return 1
    }
    if [[ "$want" != "-" ]]; then
        local got; got=$(sha256sum "$out" | cut -d' ' -f1)
        [[ "$got" == "$want" ]] || {
            echo "*** sha256 mismatch for $(basename "$out")"
            echo "    expected $want"
            echo "    got      $got"
            rm -f "$out"
            return 1
        }
    fi
}

if [[ $VERIFY_ONLY -eq 0 ]]; then
    echo "sysroot   : $DEST"
    echo "repository: $REPO"
    echo ""
    fail=0
    for entry in "${PKGS[@]}"; do
        set -- $entry
        pkg="$1"; sha="$2"
        file="mingw-w64-ucrt-x86_64-$pkg-any.pkg.tar.zst"
        printf '  %-46s' "$pkg"
        if fetch "$REPO/$file" "$CACHE/$file" "$sha"; then
            # Packages unpack as ucrt64/{bin,include,lib,share}; strip that one
            # component so the sysroot is a plain {bin,include,lib} prefix,
            # which is the shape Makefile.mingw's WIN_DEPS expects.
            tar --zstd -xf "$CACHE/$file" -C "$DEST" --strip-components=1 ucrt64 2>/dev/null \
              || { echo "extract failed"; fail=1; continue; }
            echo "ok"
        else
            fail=1
        fi
    done
    [[ $fail -eq 0 ]] || { echo ""; echo "*** sysroot is incomplete"; exit 1; }

    if [[ $WANT_TOOLCHAIN -eq 1 ]]; then
        TCDIR="$DEST/../toolchain"
        mkdir -p "$TCDIR/.cache" "$TCDIR/root"
        echo ""
        echo "toolchain : $TCDIR/root/usr/bin  (add to PATH)"
        for pkg in "${TC_PKGS[@]}"; do
            file="$pkg.pkg.tar.zst"
            printf '  %-46s' "$pkg"
            fetch "$TC_REPO/$file" "$TCDIR/.cache/$file" "-" || { echo "failed"; exit 1; }
            tar --zstd -xf "$TCDIR/.cache/$file" -C "$TCDIR/root" 2>/dev/null
            echo "ok"
        done
    fi
fi

# ---- report ---------------------------------------------------------------
echo ""
echo "=== $DEST ==="
for f in include/freetype2/ft2build.h include/sndfile.h include/samplerate.h \
         lib/libfreetype.dll.a lib/libsndfile.dll.a lib/libsamplerate.dll.a; do
    if [[ -e "$DEST/$f" ]]; then echo "  present : $f"; else echo "  MISSING : $f"; fi
done
ndll=$(ls "$DEST"/bin/*.dll 2>/dev/null | wc -l)
echo "  runtime : $ndll DLLs in $DEST/bin"
echo "  size    : $(du -sh "$DEST" 2>/dev/null | cut -f1) ($(du -sh "$CACHE" 2>/dev/null | cut -f1) of it cached packages)"

# The check that matters: every non-system DLL an imported DLL asks for must
# itself be in bin/. A missing one is a LoadLibrary failure at startup, and the
# error Windows gives for it names no file at all.
OBJDUMP=${OBJDUMP:-x86_64-w64-mingw32-objdump}
if command -v "$OBJDUMP" >/dev/null && [[ $ndll -gt 0 ]]; then
    have=$(ls "$DEST"/bin/*.dll | xargs -n1 basename | tr 'A-Z' 'a-z' | sort -u)
    want=$(for d in "$DEST"/bin/*.dll; do
             "$OBJDUMP" -p "$d" | sed -n 's/.*DLL Name: //p'
           done | tr 'A-Z' 'a-z' | sort -u)
    # Everything below ships with Windows (and with Wine).
    sysre='^(api-ms-|kernel32|user32|gdi32|advapi32|shell32|shlwapi|ole32|oleaut32|'
    sysre+='ws2_32|bcrypt|crypt32|dnsapi|iphlpapi|winmm|secur32|ntdll|ucrtbase|psapi|'
    sysre+='rpcrt4|setupapi|wldap32|userenv|version|dbghelp|powrprof|comdlg32|winspool|'
    sysre+='dwrite|usp10|opengl32|avrt|ksuser|mfplat|imm32|comctl32)'
    missing=$(comm -23 <(echo "$want" | grep -Ev "$sysre") <(echo "$have"))
    if [[ -n "$missing" ]]; then
        echo "  *** unresolved DLL imports inside the sysroot:"
        echo "$missing" | sed 's/^/        /'
        exit 1
    fi
    echo "  imports : closed (every non-system DLL import resolves inside bin/)"
else
    echo "  imports : not checked ($OBJDUMP not on PATH)"
fi
