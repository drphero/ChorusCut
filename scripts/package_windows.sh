#!/usr/bin/env bash
set -euo pipefail

# Build a self-contained Windows ChorusCut zip: the exe, its Qt deployment, MLT
# modules and data, licenses, and every non-system DLL in the dependency closure.
# Layout, which the exe-relative lookups in src/main.cpp and MLT itself require:
#   ChorusCut/bin/ChorusCut.exe  choruscut-mcp.exe  align.py  Qt6*.dll  <plugin dirs>
#   ChorusCut/lib/mlt/*.dll      (MLT modules — MLT finds them at <exedir>/../lib/mlt)
#   ChorusCut/share/mlt/...      (profiles, presets — MLT_DATA / MLT_PROFILES_PATH)
#   ChorusCut/LICENSE  THIRD_PARTY_LICENSES.txt
# Run inside an MSYS2 UCRT64 shell.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${REPO_ROOT}/build"
VERSION=""
OUT_DIR="${REPO_ROOT}/dist"
ARCH="x64"
SKIP_ZIP=0
SKIP_INSTALLER=0
CHECKSUMS_ONLY=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --build <dir>            Build tree holding bin/ChorusCut.exe (default: build)
  --version <ver>          Release version, e.g. 0.1.0 (default: 0.1.0)
  --out <dir>              Output directory (default: dist)
  --arch <arch>            Architecture label for the zip filename (default: x64)
  --skip-zip               Stage the folder but do not write the .zip
  --skip-installer         Do not build the Inno Setup installer even if iscc is present
  --checksums-only         Only regenerate SHA256SUMS.txt in --out
  -h, --help               Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build|--build-dir) BUILD_DIR="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        --arch) ARCH="$2"; shift 2 ;;
        --skip-zip) SKIP_ZIP=1; shift ;;
        --skip-installer) SKIP_INSTALLER=1; shift ;;
        --checksums-only) CHECKSUMS_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

write_checksums() {
    local dir="$1"
    mkdir -p "$dir"
    (
        cd "$dir"
        local files=()
        shopt -s nullglob
        for f in ChorusCut-*.zip ChorusCut-*-setup.exe; do
            files+=("$f")
        done
        if [[ ${#files[@]} -eq 0 ]]; then
            echo "Error: no release artifacts found in $dir to checksum." >&2
            exit 1
        fi
        shasum -a 256 "${files[@]}" > SHA256SUMS.txt
        echo "==> Wrote ${dir}/SHA256SUMS.txt:"
        cat SHA256SUMS.txt
    )
}

if [[ "$CHECKSUMS_ONLY" -eq 1 ]]; then
    write_checksums "$OUT_DIR"
    exit 0
fi

VERSION="${VERSION#v}"
[[ -z "$VERSION" ]] && VERSION="0.1.0"

# MSYS2 prefix that supplies the third-party DLLs. Everything we copy must come
# from $PREFIX/bin (never a Windows system dir).
PREFIX="${MSYS2_PREFIX:-/ucrt64}"
[[ -d "$PREFIX" ]] || PREFIX="/ucrt64"
if [[ ! -d "${PREFIX}/bin" ]]; then
    echo "Error: MSYS2 UCRT64 prefix not found at $PREFIX" >&2
    exit 1
fi
# Real Windows path of the prefix, for ntldd output comparisons.
PREFIX_W="$(cygpath -m "$PREFIX")"

BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
EXE="${BUILD_DIR}/bin/ChorusCut.exe"
MCP="${BUILD_DIR}/bin/choruscut-mcp.exe"
if [[ ! -f "$EXE" || ! -f "$MCP" ]]; then
    echo "Error: expected ${BUILD_DIR}/bin/ChorusCut.exe and bin/choruscut-mcp.exe" >&2
    exit 1
fi

WINDEPLOYQT="$(command -v windeployqt-qt6 || command -v windeployqt6 || command -v windeployqt || true)"
if [[ -z "$WINDEPLOYQT" ]]; then
    echo "Error: windeployqt not found on PATH" >&2
    exit 1
fi
NTLDD="$(command -v ntldd || true)"
if [[ -z "$NTLDD" ]]; then
    echo "Error: ntldd not found on PATH (pacman -S mingw-w64-ucrt-x86_64-ntldd)" >&2
    exit 1
fi

MLT_MODULE_DIR=""
for cand in "${PREFIX}/lib/mlt" "${PREFIX}/lib/mlt-7"; do
    [[ -d "$cand" ]] && MLT_MODULE_DIR="$cand" && break
done
MLT_DATA_DIR=""
for cand in "${PREFIX}/share/mlt" "${PREFIX}/share/mlt-7"; do
    [[ -d "$cand" ]] && MLT_DATA_DIR="$cand" && break
done
MLT_CORE_DLL=""
for cand in "${PREFIX}/bin/libmlt-7.dll" "${PREFIX}/bin/libmlt.dll"; do
    [[ -f "$cand" ]] && MLT_CORE_DLL="$cand" && break
done
MLT_PP_DLL=""
for cand in "${PREFIX}/bin/libmlt++-7.dll" "${PREFIX}/bin/libmlt++.dll" "${PREFIX}/bin/libmltpp-7.dll"; do
    [[ -f "$cand" ]] && MLT_PP_DLL="$cand" && break
done
if [[ -z "$MLT_MODULE_DIR" || -z "$MLT_DATA_DIR" || -z "$MLT_CORE_DLL" || -z "$MLT_PP_DLL" ]]; then
    echo "Error: could not locate MLT (modules=${MLT_MODULE_DIR:-none} data=${MLT_DATA_DIR:-none} core=${MLT_CORE_DLL:-none} mlt++=${MLT_PP_DLL:-none})" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
STAGE="${OUT_DIR}/stage/ChorusCut"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share"

echo "==> Staging exes into ${STAGE}/bin"
cp "$EXE" "$MCP" "$STAGE/bin/"
cp "${REPO_ROOT}/alignment/align.py" "$STAGE/bin/align.py"
for doc in LICENSE THIRD_PARTY_LICENSES.txt; do
    if [[ ! -f "${REPO_ROOT}/${doc}" ]]; then
        echo "Error: required license file missing: ${REPO_ROOT}/${doc}" >&2
        exit 1
    fi
    cp "${REPO_ROOT}/${doc}" "$STAGE/${doc}"
done
cp "$MLT_CORE_DLL" "$MLT_PP_DLL" "$STAGE/bin/"

echo "==> Running windeployqt (${WINDEPLOYQT})"
"$WINDEPLOYQT" --release --qmldir "${REPO_ROOT}/src" "$STAGE/bin/ChorusCut.exe"
"$WINDEPLOYQT" --release "$STAGE/bin/choruscut-mcp.exe"

# ChorusCut uses Qt's Basic style throughout; drop the other bundled Quick styles
# and the unused SQL drivers (only SQLite is used).
for style in FluentWinUI3 Fusion Imagine Material Universal Windows; do
    rm -f  "$STAGE/bin/Qt6QuickControls2${style}.dll" \
           "$STAGE/bin/Qt6QuickControls2${style}StyleImpl.dll"
    rm -rf "$STAGE/bin/qml/QtQuick/Controls/${style}"
done
for drv in qsqlibase qsqlmysql qsqlodbc qsqlpsql; do
    rm -f "$STAGE/bin/sqldrivers/${drv}.dll"
done
rm -rf "$STAGE/bin/qmltooling"

echo "==> Bundling MLT modules (${MLT_MODULE_DIR}) and data (${MLT_DATA_DIR})"
mkdir -p "$STAGE/lib/mlt" "$STAGE/share/mlt"
cp "$MLT_MODULE_DIR"/*.dll "$STAGE/lib/mlt/"
# libmltopenfx crashes inside ColourlabAi.ofx while scanning the system OFX path,
# and ChorusCut uses no OpenFX services. Never ship or load it.
rm -f "$STAGE/lib/mlt/libmltopenfx.dll"
cp -r "$MLT_DATA_DIR"/. "$STAGE/share/mlt/"

# MSYS2's SDL2.dll is sdl2-compat and dlopen()s SDL3.dll at runtime rather than
# linking it — ntldd never sees it, so bundle it by hand (same trap as macOS).
for sdl3 in "${PREFIX}/bin/SDL3.dll"; do
    if [[ -f "$sdl3" ]]; then cp "$sdl3" "$STAGE/bin/SDL3.dll"; break; fi
done

REQUIRED_FILES=(
    bin/ChorusCut.exe
    bin/choruscut-mcp.exe
    bin/align.py
    LICENSE
    THIRD_PARTY_LICENSES.txt
    lib/mlt/libmltcore.dll
    lib/mlt/libmltavformat.dll
    lib/mlt/libmltsdl2.dll
    lib/mlt/libmltnormalize.dll
    lib/mlt/libmltqt6.dll
    lib/mlt/libmltresample.dll
    bin/SDL3.dll
    bin/platforms/qwindows.dll
    bin/sqldrivers/qsqlite.dll
    bin/imageformats/qwebp.dll
    bin/multimedia/ffmpegmediaplugin.dll
    bin/Qt6Multimedia.dll
    share/mlt/profiles
)
# The offscreen platform plugin lets packaged --check / --screenshot runs work.
for cand in "${PREFIX}/share/qt6/plugins/platforms/qoffscreen.dll" "${PREFIX}/plugins/platforms/qoffscreen.dll"; do
    if [[ -f "$cand" ]]; then cp "$cand" "$STAGE/bin/platforms/qoffscreen.dll"; break; fi
done
REQUIRED_FILES+=(bin/platforms/qoffscreen.dll)

# Dependency closure + self-containment verify run in Python: hundreds of
# basename/cygpath subshells per PE hit MSYS2's flaky fork(), so the worklist
# lives in one process that only shells out to ntldd.
echo "==> Copying dependency-closure DLLs into bin/ + verifying self-containment"
python - "$(cygpath -m "$STAGE")" "$(cygpath -m "$PREFIX")" "$(cygpath -m "$NTLDD")" <<'PYEOF'
import os, re, shutil, subprocess, sys
from pathlib import Path

stage = Path(sys.argv[1]).resolve()
prefix = Path(sys.argv[2]).resolve()
prefix_bin = prefix / "bin"
ntldd = sys.argv[3]
windows = Path("C:/Windows")
stage_s = str(stage).lower()
windows_s = str(windows).lower()

# Loader-resolved API sets and OS DLLs: never copied.
SYS_PREFIX = re.compile(
    r"^(api-ms-|ext-ms-|d3d\d|mf|msvcp|vcruntime|dhcpcsvc)", re.I)
SYS_NAMES = {
    n.lower() for n in """
    kernel32.dll user32.dll gdi32.dll advapi32.dll msvcrt.dll ucrtbase.dll
    ntdll.dll kernelbase.dll ws2_32.dll shell32.dll ole32.dll oleaut32.dll
    rpcrt4.dll winmm.dll imm32.dll version.dll dwmapi.dll dxgi.dll dxcore.dll
    setupapi.dll cfgmgr32.dll comdlg32.dll comctl32.dll shlwapi.dll
    secur32.dll schannel.dll iphlpapi.dll dnsapi.dll crypt32.dll bcrypt.dll
    ncrypt.dll authz.dll netapi32.dll userenv.dll profapi.dll wldap32.dll
    normaliz.dll winspool.drv mpr.dll gdi32full.dll msvcp_win.dll
    windows.storage.dll wtsapi32.dll combase.dll shcore.dll powrprof.dll
    umpdc.dll wintypes.dll dwrite.dll ddraw.dll dsound.dll evr.dll propsys.dll
    wininet.dll urlmon.dll wintrust.dll concrt140.dll opengl32.dll glu32.dll
    hid.dll devobj.dll sspicli.dll pdh.dll psapi.dll dbghelp.dll winhttp.dll
    wlanapi.dll oleacc.dll uiautomationcore.dll msimg32.dll usp10.dll
    win32u.dll kernel.appcore.dll msctf.dll textinputframework.dll
    cryptbase.dll bcryptprimitives.dll ntmarta.dll rsaenh.dll dpapi.dll
    cryptsp.dll wship6.dll mswsock.dll nsi.dll winnsi.dll dbgmodel.dll
    resourcepolicyclient.dll dxva2.dll avicap32.dll msvfw32.dll avrt.dll
    mmdevapi.dll ksuser.dll wdmaud.drv msacm32.drv msacm32.dll midimap.dll
    acgenral.dll samcli.dll sxs.dll cabinet.dll clbcatq.dll apphelp.dll
    wlidnsp.dll ondemandconnroutehelper.dll rasadhlp.dll fwpuclnt.dll
    wshbth.dll wshqos.dll napinsp.dll pnrpnsp.dll nlaapi.dll winrnr.dll
    mfplat.dll mfreadwrite.dll mfuuid.dll dbgeng.dll
    """.split()
}

SYSTEM32 = windows / "System32"

def is_system(name):
    n = os.path.basename(name).lower()
    if n in SYS_NAMES or SYS_PREFIX.match(n):
        return True
    # Delay-load OS components report "not found" to ntldd but ship in System32.
    return (SYSTEM32 / os.path.basename(name)).is_file()

def inside(path, root_s):
    p = str(Path(path).resolve()).lower()
    return p == root_s or p.startswith(root_s + os.sep)

def deps_of(pe):
    out = subprocess.run([ntldd, "-R", str(pe)], capture_output=True,
                         text=True).stdout
    for line in out.splitlines():
        line = line.strip()
        if "=>" not in line:
            continue
        dep, resolved = (s.strip() for s in line.split("=>", 1))
        yield dep, resolved.split(" (")[0].strip()

def real(p):
    return str(Path(p).resolve()).lower()

# Worklist: each PE is scanned once; only DLLs copied this round add work.
queue = [p for root in (stage / "bin", stage / "lib")
         for p in root.rglob("*") if p.suffix.lower() in (".exe", ".dll")]
seen, bundled = set(), 0
while queue:
    pe = queue.pop()
    if real(pe) in seen:
        continue
    seen.add(real(pe))
    for dep, resolved in deps_of(pe):
        if is_system(dep):
            continue
        src = None
        if resolved == "not found" or not Path(resolved).is_file():
            cand = prefix_bin / os.path.basename(dep)
            if cand.is_file():
                src = cand
        elif not inside(resolved, stage_s) and not inside(resolved, windows_s):
            src = Path(resolved)
        if src is None:
            continue
        dest = stage / "bin" / src.name
        if not dest.exists():
            shutil.copy2(src, dest)
            print(f"    bundled {dest.name}", flush=True)
            bundled += 1
            queue.append(dest)

print(f"    closure: {bundled} DLLs bundled over {len(seen)} PEs", flush=True)

leaks = 0
for pe in (p for p in stage.rglob("*") if p.suffix.lower() in (".exe", ".dll")):
    rel = pe.relative_to(stage)
    for dep, resolved in deps_of(pe):
        if is_system(dep):
            continue
        if resolved == "not found" or not Path(resolved).is_file():
            if (stage / "bin" / os.path.basename(dep)).is_file():
                continue
            if (prefix_bin / os.path.basename(dep)).is_file():
                print(f"Error: {dep} needed by {rel} is in the toolchain bin but was not bundled", file=sys.stderr)
                leaks += 1
            else:
                # OS delay-load dep absent on this machine — resolved lazily where it exists.
                print(f"    note: {dep} needed by {rel} resolves on systems that have it")
        elif not inside(resolved, stage_s) and not inside(resolved, windows_s):
            # The loader searches the exe's directory first, so a same-named DLL in
            # bin/ wins over the copy ntldd found on the toolchain PATH.
            if not (stage / "bin" / os.path.basename(dep)).is_file():
                print(f"Error: {rel} depends on out-of-package {resolved}", file=sys.stderr)
                leaks += 1
sys.exit(1 if leaks else 0)
PYEOF

echo "==> Verifying required files"
missing=0
for rel in "${REQUIRED_FILES[@]}"; do
    if [[ ! -e "$STAGE/$rel" ]]; then
        echo "Error: required file missing from stage: $rel" >&2
        missing=1
    fi
done
[[ "$missing" == 0 ]] || exit 1

echo "==> Smoke-testing the staged app"
WORK="$(mktemp -d /tmp/choruscut-pack.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/example"
cp -R "${REPO_ROOT}/Example/Media" "$WORK/example/Media"
export QT_QPA_PLATFORM=offscreen SDL_AUDIODRIVER=disk SDL_AUDIO_DRIVER=disk QT_FORCE_STDERR_LOGGING=1
"$STAGE/bin/ChorusCut.exe" --project "$(cygpath -w "$WORK/example")" --check

echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"pack-smoke","version":"0"}}}' \
    | timeout 10 "$STAGE/bin/choruscut-mcp.exe" | head -1 | grep -q '"result"' \
    || { echo "Error: choruscut-mcp did not answer initialize" >&2; exit 1; }

if [[ "$SKIP_ZIP" == 0 ]]; then
    ZIP_NAME="ChorusCut-${VERSION}-windows-${ARCH}.zip"
    echo "==> Writing ${OUT_DIR}/${ZIP_NAME}"
    rm -f "${OUT_DIR}/${ZIP_NAME}"
    (cd "$(dirname "$STAGE")" && zip -qr -y "${OUT_DIR}/${ZIP_NAME}" "$(basename "$STAGE")")
    write_checksums "$OUT_DIR"
fi

if [[ "$SKIP_INSTALLER" == 0 ]]; then
    ISCC="$(command -v iscc || command -v ISCC || true)"
    # LOCALAPPDATA isn't always imported into the MSYS2 env — cygpath -F 28
    # (CSIDL_LOCAL_APPDATA) resolves it straight from the shell API.
    _local_appdata="$(cygpath -u "${LOCALAPPDATA:-/nonexistent}" 2>/dev/null)"
    [[ "$_local_appdata" == /nonexistent* ]] && _local_appdata="$(cygpath -F 28 2>/dev/null)"
    for cand in "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
                "/c/Program Files/Inno Setup 6/ISCC.exe" \
                "${_local_appdata}/Programs/Inno Setup 6/ISCC.exe"; do
        [[ -z "$ISCC" && -f "$cand" ]] && ISCC="$cand"
    done
    if [[ -n "$ISCC" && -f "${REPO_ROOT}/scripts/ChorusCut.iss" ]]; then
        echo "==> Building Inno Setup installer (${ISCC})"
        # //O overrides the .iss OutputDir and CHORUSCUT_STAGE its Source dir,
        # so the installer follows --out/--build-dir like everything else.
        (cd "${REPO_ROOT}/scripts" && CHORUSCUT_VERSION="$VERSION" CHORUSCUT_STAGE="$(cygpath -w "${STAGE}")" \
            "$ISCC" //Q "//O$(cygpath -w "${OUT_DIR}")" ChorusCut.iss)
        write_checksums "$OUT_DIR"
    else
        echo "==> Skipping installer (iscc not found; winget install JRSoftware.InnoSetup)"
    fi
fi

echo "==> Done: ${STAGE}"
