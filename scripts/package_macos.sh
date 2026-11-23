#!/usr/bin/env bash
set -euo pipefail

# Build a self-contained macOS ChorusCut.app, compressed .dmg, ComfyUI-ChorusCut .zip,
# and SHA256SUMS.txt with zero references to /opt/homebrew or /usr/local.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

APP_INPUT="${REPO_ROOT}/build/ChorusCut.app"
VERSION=""
OUT_DIR="${REPO_ROOT}/dist"
ARCH="$(uname -m)"
SIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
KEYCHAIN=""
DMG_ONLY=0
SKIP_DMG=0
KEEP_APP=0
CHECKSUMS_ONLY=0
BUNDLE_ID="org.choruscut.ChorusCut"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --app <path>             Built ChorusCut.app to package (default: build/ChorusCut.app)
  --version <ver>          Release version, e.g. 0.1.0 (default: Info.plist or 0.1.0)
  --out <dir>              Output directory (default: dist)
  --arch <arch>            Architecture label for DMG filename (default: uname -m)
  --sign-identity <id>     Code-signing identity (default: "-" for ad-hoc)
  --keychain <path>        Keychain path passed to codesign (optional)
  --dmg-only               Skip packaging ComfyUI-ChorusCut-<version>.zip
  --skip-dmg               Skip building the .dmg (useful with --keep-app for local app testing)
  --keep-app               Also copy the packaged, signed ChorusCut.app into --out
  --checksums-only         Only regenerate SHA256SUMS.txt in --out (e.g. after notarization)
  -h, --help               Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app) APP_INPUT="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        --arch) ARCH="$2"; shift 2 ;;
        --sign-identity) SIGN_IDENTITY="$2"; shift 2 ;;
        --keychain) KEYCHAIN="$2"; shift 2 ;;
        --dmg-only) DMG_ONLY=1; shift ;;
        --skip-dmg) SKIP_DMG=1; shift ;;
        --keep-app) KEEP_APP=1; shift ;;
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
        for f in ChorusCut-*.dmg ComfyUI-ChorusCut-*.zip; do
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

if [[ ! -d "$APP_INPUT" ]]; then
    echo "Error: app bundle not found at: $APP_INPUT" >&2
    exit 1
fi

APP_INPUT="$(cd "$(dirname "$APP_INPUT")" && pwd)/$(basename "$APP_INPUT")"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

if [[ -z "$VERSION" ]]; then
    VERSION="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "${APP_INPUT}/Contents/Info.plist" 2>/dev/null || echo "0.1.0")"
fi
VERSION="${VERSION#v}"
BUNDLE_VERSION="$(echo "$VERSION" | sed -E 's/^([0-9]+(\.[0-9]+)*).*$/\1/')"
if [[ -z "$BUNDLE_VERSION" || "$BUNDLE_VERSION" == "$VERSION" && ! "$BUNDLE_VERSION" =~ ^[0-9] ]]; then
    BUNDLE_VERSION="0.1.0"
fi

BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
QT_PREFIX="$(brew --prefix qt 2>/dev/null || echo "${BREW_PREFIX}/opt/qt")"
MACDEPLOYQT="${QT_PREFIX}/bin/macdeployqt"
if [[ ! -x "$MACDEPLOYQT" ]]; then
    MACDEPLOYQT="$(command -v macdeployqt || true)"
fi
if [[ -z "$MACDEPLOYQT" || ! -x "$MACDEPLOYQT" ]]; then
    echo "Error: macdeployqt not found (checked ${QT_PREFIX}/bin/macdeployqt and PATH)." >&2
    exit 1
fi

MLT_MODULE_DIR="$(PKG_CONFIG_PATH="${BREW_PREFIX}/lib/pkgconfig:${BREW_PREFIX}/opt/mlt/lib/pkgconfig:${PKG_CONFIG_PATH:-}" pkg-config --variable=moduledir mlt-framework-7 2>/dev/null || true)"
MLT_DATA_DIR="$(PKG_CONFIG_PATH="${BREW_PREFIX}/lib/pkgconfig:${BREW_PREFIX}/opt/mlt/lib/pkgconfig:${PKG_CONFIG_PATH:-}" pkg-config --variable=mltdatadir mlt-framework-7 2>/dev/null || true)"
if [[ -z "$MLT_MODULE_DIR" || ! -d "$MLT_MODULE_DIR" ]]; then
    for cand in "${BREW_PREFIX}/opt/mlt/lib/mlt" "${BREW_PREFIX}/opt/mlt/lib/mlt-7" "${BREW_PREFIX}/lib/mlt" "${BREW_PREFIX}/lib/mlt-7"; do
        if [[ -d "$cand" ]]; then MLT_MODULE_DIR="$cand"; break; fi
    done
fi
if [[ -z "$MLT_DATA_DIR" || ! -d "$MLT_DATA_DIR" ]]; then
    for cand in "${BREW_PREFIX}/opt/mlt/share/mlt" "${BREW_PREFIX}/opt/mlt/share/mlt-7" "${BREW_PREFIX}/share/mlt" "${BREW_PREFIX}/share/mlt-7"; do
        if [[ -d "$cand" ]]; then MLT_DATA_DIR="$cand"; break; fi
    done
fi
if [[ ! -d "$MLT_MODULE_DIR" || ! -d "$MLT_DATA_DIR" ]]; then
    echo "Error: could not locate MLT plugins (${MLT_MODULE_DIR}) or data (${MLT_DATA_DIR})." >&2
    exit 1
fi

STAGE="$(mktemp -d "/tmp/choruscut-package.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

APP="${STAGE}/ChorusCut.app"
echo "==> Staging ${APP_INPUT} -> ${APP}"
ditto "$APP_INPUT" "$APP"

# Stamp bundle metadata into the staged Info.plist so release artifacts always match --version.
PLIST="${APP}/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier ${BUNDLE_ID}" "$PLIST" 2>/dev/null || \
    /usr/libexec/PlistBuddy -c "Add :CFBundleIdentifier string ${BUNDLE_ID}" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${VERSION}" "$PLIST" 2>/dev/null || \
    /usr/libexec/PlistBuddy -c "Add :CFBundleShortVersionString string ${VERSION}" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion ${BUNDLE_VERSION}" "$PLIST" 2>/dev/null || \
    /usr/libexec/PlistBuddy -c "Add :CFBundleVersion string ${BUNDLE_VERSION}" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :NSHumanReadableCopyright Copyright (C) 2026 drphero. Licensed under GPL-3.0-or-later." "$PLIST" 2>/dev/null || \
    /usr/libexec/PlistBuddy -c "Add :NSHumanReadableCopyright string Copyright (C) 2026 drphero. Licensed under GPL-3.0-or-later." "$PLIST"

# Bundle GPL-3.0 license text and third-party library notices inside the app bundle.
mkdir -p "${APP}/Contents/Resources"
for doc in LICENSE THIRD_PARTY_LICENSES.txt; do
    if [[ ! -f "${REPO_ROOT}/${doc}" ]]; then
        echo "Error: required license file missing: ${REPO_ROOT}/${doc}" >&2
        exit 1
    fi
    cp "${REPO_ROOT}/${doc}" "${APP}/Contents/Resources/${doc}"
done

echo "==> Running macdeployqt (${MACDEPLOYQT})"
# Deploy core Qt frameworks and plugins without -qmldir: on Homebrew's monolithic Qt,
# macdeployqt -qmldir recursively copies all of QtQuick/* (including Qt3D, QtPdf,
# QtVirtualKeyboard, etc.). We deploy the exact QML modules via qmlimportscanner below.
"$MACDEPLOYQT" "$APP" \
    -executable="${APP}/Contents/MacOS/choruscut-mcp" \
    -always-overwrite \
    -no-strip \
    -no-codesign

echo "==> Deploying QML modules via qmlimportscanner"
QT_QML_SRC="${QT_PREFIX}/share/qt/qml"
QMLIMPORTSCANNER="${QT_PREFIX}/share/qt/libexec/qmlimportscanner"
if [[ ! -x "$QMLIMPORTSCANNER" ]]; then
    QMLIMPORTSCANNER="$(command -v qmlimportscanner || true)"
fi
if [[ -z "$QMLIMPORTSCANNER" || ! -x "$QMLIMPORTSCANNER" ]]; then
    echo "Error: qmlimportscanner not found (checked ${QT_PREFIX}/share/qt/libexec/qmlimportscanner)." >&2
    exit 1
fi

python3 - "$QMLIMPORTSCANNER" "${REPO_ROOT}/src" "$QT_QML_SRC" "${APP}/Contents/Resources/qml" <<'PYEOF'
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

scanner, qml_dir, qt_qml_src, dst_root = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
out = subprocess.check_output([scanner, "-rootPath", qml_dir, "-importPath", str(qt_qml_src)], text=True)
entries = json.loads(out)

# Unused non-macOS QtQuick.Controls styles (ChorusCut uses Basic style)
SKIP_PREFIXES = (
    "QtQuick/Controls/FluentWinUI3",
    "QtQuick/Controls/Imagine",
    "QtQuick/Controls/Material",
    "QtQuick/Controls/Universal",
    "QtQuick/Controls/iOS",
)

modules = sorted({
    e["relativePath"]
    for e in entries
    if e.get("type") == "module" and e.get("relativePath")
    and not any(e["relativePath"].startswith(p) for p in SKIP_PREFIXES)
})

def copy_file(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src.resolve(), dst)
    st = dst.stat()
    if not (st.st_mode & stat.S_IWUSR):
        dst.chmod(st.st_mode | stat.S_IWUSR)

def copy_non_module_tree(src_dir: Path, dst_dir: Path):
    dst_dir.mkdir(parents=True, exist_ok=True)
    for item in src_dir.iterdir():
        if item.name in ("tooling", ".DS_Store"):
            continue
        resolved = item.resolve()
        if resolved.is_dir():
            # Do not descend into child QML modules unless explicitly listed by qmlimportscanner
            if (resolved / "qmldir").exists():
                continue
            copy_non_module_tree(resolved, dst_dir / item.name)
        elif resolved.is_file():
            copy_file(resolved, dst_dir / item.name)

for rel in modules:
    src = qt_qml_src / rel
    if not src.exists():
        continue
    copy_non_module_tree(src.resolve(), dst_root / rel)

print(f"    Deployed {len(modules)} QML modules: {', '.join(modules)}")
PYEOF

# Ensure qt.conf points Qt to Contents/PlugIns and Contents/Resources/qml.
# On macOS app bundles, Qt already sets the default prefix for Contents/Resources/qt.conf
# to Contents/, so omit Prefix (setting Prefix = .. shifts lookup up to ChorusCut.app/).
cat > "${APP}/Contents/Resources/qt.conf" <<'EOF'
[Paths]
Plugins = PlugIns
Imports = Resources/qml
QmlImports = Resources/qml
Qml2Imports = Resources/qml
EOF

# Ensure essential Qt plugins are present, including offscreen platform plugin for headless checks
# and --screenshot, Darwin multimedia backend for LivePreview, and SQLite driver.
QT_PLUGINS_SRC="${QT_PREFIX}/share/qt/plugins"
ensure_qt_plugin() {
    local rel="$1"
    local src="${QT_PLUGINS_SRC}/${rel}"
    local dst="${APP}/Contents/PlugIns/${rel}"
    if [[ ! -e "$dst" ]]; then
        if [[ -e "$src" ]]; then
            mkdir -p "$(dirname "$dst")"
            cp -L "$src" "$dst"
            chmod u+w "$dst"
            echo "    Added Qt plugin: ${rel}"
        else
            echo "Error: required Qt plugin not found: ${src}" >&2
            exit 1
        fi
    fi
}

ensure_qt_plugin "platforms/libqcocoa.dylib"
ensure_qt_plugin "platforms/libqoffscreen.dylib"
ensure_qt_plugin "sqldrivers/libqsqlite.dylib"
ensure_qt_plugin "multimedia/libdarwinmediaplugin.dylib"

# Prune unused Homebrew Qt meta-package plugins that pull huge unrelated frameworks.
if [[ -e "${APP}/Contents/PlugIns/imageformats/libqpdf.dylib" ]]; then
    echo "    Pruning unused imageformats/libqpdf.dylib (pulls QtWebEngine/QtPdf)"
    rm -f "${APP}/Contents/PlugIns/imageformats/libqpdf.dylib"
    rm -rf "${APP}/Contents/Frameworks/QtPdf.framework"
fi
if [[ -e "${APP}/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib" ]]; then
    echo "    Pruning unused platforminputcontexts/libqtvirtualkeyboardplugin.dylib"
    rm -f "${APP}/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"
    rm -rf "${APP}/Contents/Frameworks/QtVirtualKeyboard.framework" \
           "${APP}/Contents/Frameworks/QtVirtualKeyboardQml.framework"
fi

echo "==> Bundling MLT 7 plugins (${MLT_MODULE_DIR}) and data (${MLT_DATA_DIR})"
MLT_PLUGINS_DST="${APP}/Contents/PlugIns/mlt-7"
MLT_DATA_DST="${APP}/Contents/Resources/mlt-7"
mkdir -p "$MLT_PLUGINS_DST" "$MLT_DATA_DST"

REQUIRED_MLT_PLUGINS=(
    libmltcore.so
    libmltavformat.so
    libmltsdl2.so
    libmltnormalize.so
    libmltqt6.so
    libmltresample.so
)
OPTIONAL_MLT_PLUGINS=(
    libmltplus.so
    libmltxml.so
)

for plugin in "${REQUIRED_MLT_PLUGINS[@]}"; do
    if [[ ! -f "${MLT_MODULE_DIR}/${plugin}" ]]; then
        echo "Error: required MLT plugin missing: ${MLT_MODULE_DIR}/${plugin}" >&2
        exit 1
    fi
    cp -L "${MLT_MODULE_DIR}/${plugin}" "${MLT_PLUGINS_DST}/${plugin}"
    chmod u+w "${MLT_PLUGINS_DST}/${plugin}"
done

for plugin in "${OPTIONAL_MLT_PLUGINS[@]}"; do
    if [[ -f "${MLT_MODULE_DIR}/${plugin}" ]]; then
        cp -L "${MLT_MODULE_DIR}/${plugin}" "${MLT_PLUGINS_DST}/${plugin}"
        chmod u+w "${MLT_PLUGINS_DST}/${plugin}"
    fi
done

ditto "${MLT_DATA_DIR}" "${MLT_DATA_DST}"

# Homebrew's sdl2-compat dlopen()s @loader_path/libSDL3.dylib at runtime rather than linking it
# via LC_LOAD_DYLIB. Copy libSDL3 beside libSDL2 in Contents/Frameworks so audio works with no Homebrew.
for sdl3_dir in "${BREW_PREFIX}/opt/sdl3/lib" "${BREW_PREFIX}/lib"; do
    if [[ -f "${sdl3_dir}/libSDL3.0.dylib" ]]; then
        mkdir -p "${APP}/Contents/Frameworks"
        cp -L "${sdl3_dir}/libSDL3.0.dylib" "${APP}/Contents/Frameworks/libSDL3.0.dylib"
        chmod u+w "${APP}/Contents/Frameworks/libSDL3.0.dylib"
        ln -sf "libSDL3.0.dylib" "${APP}/Contents/Frameworks/libSDL3.dylib"
        echo "    Bundled runtime dlopen dependency: libSDL3.0.dylib (+ libSDL3.dylib symlink)"
        break
    fi
done

echo "==> Recursively bundling non-system shared libraries and rewriting install names"
python3 - "$APP" "$BREW_PREFIX" "$QT_PREFIX" "$MLT_MODULE_DIR" <<'PYEOF'
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
from pathlib import Path

app = Path(sys.argv[1]).resolve()
brew_prefix = Path(sys.argv[2]).resolve()
qt_prefix = Path(sys.argv[3]).resolve()
mlt_module_dir = Path(sys.argv[4]).resolve()

contents = app / "Contents"
frameworks_dir = contents / "Frameworks"
frameworks_dir.mkdir(parents=True, exist_ok=True)

MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
}

def is_macho(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with path.open("rb") as f:
            magic = f.read(4)
        return magic in MACHO_MAGICS
    except OSError:
        return False

def make_writable(path: Path) -> None:
    st = path.stat()
    if not (st.st_mode & stat.S_IWUSR):
        path.chmod(st.st_mode | stat.S_IWUSR)

def run_cmd(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"Command failed ({res.returncode}): {' '.join(cmd)}\n{res.stderr}")
    return res.stdout

def otool_deps(path: Path):
    out = run_cmd(["otool", "-L", str(path)])
    id_out = run_cmd(["otool", "-D", str(path)]).splitlines()
    own_id = id_out[1].strip() if len(id_out) >= 2 else None
    deps = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        dep = line.split(" (", 1)[0]
        if own_id and dep == own_id:
            continue
        deps.append(dep)
    return own_id, deps

def otool_rpaths(path: Path):
    out = run_cmd(["otool", "-l", str(path)])
    rpaths = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" in line:
            for j in range(i + 1, min(i + 6, len(lines))):
                m = re.match(r"\s*path\s+(.*?)\s+\(offset\s+\d+\)", lines[j])
                if m:
                    rpaths.append(m.group(1))
                    break
    return rpaths

def is_system_lib(dep: str) -> bool:
    return dep.startswith("/System/Library/") or dep.startswith("/usr/lib/")

def is_external_abs(dep: str) -> bool:
    return dep.startswith("/") and not is_system_lib(dep)

# Build search directories for resolving @rpath / @loader_path dependencies not yet in Contents/Frameworks.
search_lib_dirs = [
    frameworks_dir,
    brew_prefix / "lib",
    brew_prefix / "opt/mlt/lib",
    brew_prefix / "opt/ffmpeg/lib",
    brew_prefix / "opt/qtkeychain/lib",
    qt_prefix / "lib",
    mlt_module_dir.parent,
]
opt_dir = brew_prefix / "opt"
if opt_dir.is_dir():
    for entry in sorted(opt_dir.iterdir()):
        lib = entry / "lib"
        if lib.is_dir():
            search_lib_dirs.append(lib)

# Track realpath -> installed canonical @rpath target (e.g. "@rpath/libavcodec.63.dylib"
# or "@rpath/QtWidgets.framework/Versions/A/QtWidgets")
installed_by_realpath = {}
# Map installed bundle file path -> original source directory (to resolve @loader_path deps of copied dylibs)
origin_dir_by_bundle_path = {}

# Seed already-present frameworks & dylibs in Contents/Frameworks
def scan_existing_frameworks():
    for item in frameworks_dir.iterdir():
        if item.suffix == ".framework" and item.is_dir() and not item.is_symlink():
            name = item.stem
            bin_path = item / "Versions/A" / name
            if bin_path.is_file():
                installed_by_realpath[str(bin_path.resolve())] = f"@rpath/{name}.framework/Versions/A/{name}"
        elif item.is_file() and not item.is_symlink() and is_macho(item):
            installed_by_realpath[str(item.resolve())] = f"@rpath/{item.name}"

scan_existing_frameworks()

def copy_framework(src_bin: Path) -> str:
    # Always resolve symlinks and '..' segments before locating the .framework root.
    real_bin = src_bin.resolve()
    parts = real_bin.parts
    fw_idx = None
    for idx, part in enumerate(parts):
        if part.endswith(".framework"):
            fw_idx = idx
    if fw_idx is None:
        raise RuntimeError(f"Not a framework path: {real_bin}")
    fw_src = Path(*parts[: fw_idx + 1])
    fw_name = fw_src.stem
    fw_dst = frameworks_dir / fw_src.name
    target_rel = f"{fw_name}.framework/Versions/A/{fw_name}"
    target_rpath = f"@rpath/{target_rel}"

    if not fw_dst.exists():
        # Copy framework directory preserving structure, excluding headers/dev clutter
        def ignore_dev(_dir, names):
            ignored = set()
            for n in names:
                if n in ("Headers", "PrivateHeaders", "Modules") or n.endswith((".prl", ".a", ".h", ".hpp")):
                    ignored.add(n)
            return ignored
        shutil.copytree(fw_src, fw_dst, symlinks=True, ignore=ignore_dev)
        # Ensure writable binaries
        for p in fw_dst.rglob("*"):
            if p.is_file() and not p.is_symlink():
                make_writable(p)
        print(f"    Bundled framework: {fw_src.name}")

    dst_bin = frameworks_dir / target_rel
    if not dst_bin.is_file():
        # Fallback if framework layout didn't have Versions/A
        alt = fw_dst / fw_name
        if alt.is_file():
            dst_bin = alt
            target_rpath = f"@rpath/{fw_name}.framework/{fw_name}"
        else:
            raise RuntimeError(f"Copied framework missing binary: {dst_bin}")
    installed_by_realpath[str(real_bin)] = target_rpath
    origin_dir_by_bundle_path[str(dst_bin.resolve())] = real_bin.parent
    return target_rpath

def copy_dylib(src_path: Path, preferred_name: str) -> str:
    real_src = src_path.resolve()
    key = str(real_src)
    if key in installed_by_realpath:
        return installed_by_realpath[key]

    dst = frameworks_dir / preferred_name
    if dst.exists() or dst.is_symlink():
        # Reuse if same basename already placed
        target_rpath = f"@rpath/{preferred_name}"
        installed_by_realpath[key] = target_rpath
        origin_dir_by_bundle_path.setdefault(str(dst.resolve()), src_path.parent)
        return target_rpath

    shutil.copy2(real_src, dst)
    make_writable(dst)
    target_rpath = f"@rpath/{preferred_name}"
    installed_by_realpath[key] = target_rpath
    origin_dir_by_bundle_path[str(dst.resolve())] = real_src.parent
    return target_rpath

def resolve_dep_source(macho: Path, dep: str, rpaths):
    if dep.startswith("/"):
        p = Path(dep)
        if p.exists():
            return p.resolve()
        return None
    if dep.startswith("@rpath/"):
        rel = dep[len("@rpath/"):]
        # Already inside Contents/Frameworks?
        cand = frameworks_dir / rel
        if cand.exists():
            return cand.resolve()
        # Try binary's own LC_RPATH entries
        for rp in rpaths:
            if rp.startswith("@loader_path"):
                base = origin_dir_by_bundle_path.get(str(macho.resolve()), macho.parent)
                suffix = rp[len("@loader_path"):].lstrip("/")
                cand = (base / suffix / rel) if suffix else (base / rel)
            elif rp.startswith("@executable_path"):
                suffix = rp[len("@executable_path"):].lstrip("/")
                cand = contents / "MacOS" / suffix / rel
            else:
                cand = Path(rp) / rel
            if cand.exists():
                return cand.resolve()
        # Try Homebrew search dirs
        for d in search_lib_dirs:
            cand = d / rel
            if cand.exists():
                return cand.resolve()
        return None
    if dep.startswith("@loader_path/"):
        rel = dep[len("@loader_path/"):]
        # First check relative to bundle location
        cand = (macho.parent / rel)
        if cand.exists():
            try:
                cand.resolve().relative_to(app)
                return cand.resolve()
            except ValueError:
                pass
        # Check relative to original Homebrew directory if this binary was copied from Homebrew
        orig_dir = origin_dir_by_bundle_path.get(str(macho.resolve()))
        if orig_dir:
            cand = orig_dir / rel
            if cand.exists():
                return cand.resolve()
        # Fallback to Homebrew search dirs by basename
        base = Path(rel).name
        for d in search_lib_dirs:
            cand = d / base
            if cand.exists():
                return cand.resolve()
        return None
    if dep.startswith("@executable_path/"):
        rel = dep[len("@executable_path/"):]
        cand = (contents / "MacOS" / rel)
        if cand.exists():
            return cand.resolve()
        return None
    return None

def list_all_machos():
    result = []
    for p in sorted(contents.rglob("*")):
        if is_macho(p):
            result.append(p)
    return result

# Fixed-point loop: keep discovering and rewriting until no new Mach-O files are added and no changes remain.
processed_pass = 0
while True:
    processed_pass += 1
    if processed_pass > 12:
        raise RuntimeError("Dependency bundling exceeded 12 passes; possible cycle.")
    machos = list_all_machos()
    any_added = False
    for macho in machos:
        own_id, deps = otool_deps(macho)
        rpaths = otool_rpaths(macho)
        changes = []

        # Normalize LC_ID_DYLIB for dylibs/frameworks inside Contents/Frameworks
        new_id = None
        try:
            rel_fw = macho.relative_to(frameworks_dir)
            expected_id = f"@rpath/{rel_fw.as_posix()}"
            if own_id and own_id != expected_id:
                new_id = expected_id
        except ValueError:
            pass

        for dep in deps:
            if is_system_lib(dep):
                continue

            # If it's already @rpath/<something> present in Contents/Frameworks, keep it
            if dep.startswith("@rpath/"):
                rel = dep[len("@rpath/"):]
                if (frameworks_dir / rel).exists():
                    continue
                src = resolve_dep_source(macho, dep, rpaths)
                if src is None:
                    raise RuntimeError(f"Unresolved @rpath dependency {dep} in {macho}")
                if ".framework/" in str(src):
                    new_dep = copy_framework(src)
                else:
                    new_dep = copy_dylib(src, Path(rel).name)
                any_added = True
                if new_dep != dep:
                    changes.append((dep, new_dep))
                continue

            if dep.startswith("@loader_path/"):
                rel = dep[len("@loader_path/"):]
                local_cand = (macho.parent / rel)
                in_bundle = False
                if local_cand.exists():
                    try:
                        local_cand.resolve().relative_to(app)
                        in_bundle = True
                    except ValueError:
                        in_bundle = False
                if in_bundle:
                    continue
                src = resolve_dep_source(macho, dep, rpaths)
                if src is None:
                    raise RuntimeError(f"Unresolved @loader_path dependency {dep} in {macho}")
                if ".framework/" in str(src):
                    new_dep = copy_framework(src)
                else:
                    new_dep = copy_dylib(src, Path(rel).name)
                any_added = True
                changes.append((dep, new_dep))
                continue

            if dep.startswith("@executable_path/"):
                rel = dep[len("@executable_path/"):]
                local_cand = (contents / "MacOS" / rel)
                if local_cand.exists():
                    try:
                        local_cand.resolve().relative_to(app)
                        continue
                    except ValueError:
                        pass
                raise RuntimeError(f"Unresolved @executable_path dependency {dep} in {macho}")

            if is_external_abs(dep):
                src = Path(dep)
                if not src.exists():
                    raise RuntimeError(f"Missing external library {dep} referenced by {macho}")
                if ".framework/" in dep:
                    new_dep = copy_framework(src)
                else:
                    new_dep = copy_dylib(src, src.name)
                any_added = True
                changes.append((dep, new_dep))
                continue

        if new_id or changes:
            make_writable(macho)
            cmd = ["install_name_tool"]
            if new_id:
                cmd += ["-id", new_id]
            for old, new in changes:
                cmd += ["-change", old, new]
            cmd.append(str(macho))
            run_cmd(cmd)

    if not any_added:
        break

print(f"    Bundled dependency closure converged in {processed_pass} pass(es).")

# Normalize LC_RPATH on every Mach-O inside Contents/ so:
# - No /opt/homebrew, /usr/local, /opt/local, /Users, or /tmp rpaths remain.
# - Every Mach-O has a relative @loader_path rpath to Contents/Frameworks.
for macho in list_all_machos():
    rpaths = otool_rpaths(macho)
    rel = os.path.relpath(frameworks_dir, macho.parent)
    desired_rpath = "@loader_path" if rel == "." else f"@loader_path/{rel}"
    desired_extra = ["@executable_path/../Frameworks"] if macho.parent == (contents / "MacOS") else []

    to_delete = []
    for rp in rpaths:
        if rp.startswith(("/opt/homebrew", "/usr/local", "/opt/local", "/Users/", "/tmp/", "/private/tmp/", "/var/")):
            to_delete.append(rp)
    to_add = []
    for rp in [desired_rpath] + desired_extra:
        if rp not in rpaths and rp not in to_add:
            to_add.append(rp)

    if to_delete or to_add:
        make_writable(macho)
        cmd = ["install_name_tool"]
        for rp in to_delete:
            cmd += ["-delete_rpath", rp]
        for rp in to_add:
            cmd += ["-add_rpath", rp]
        cmd.append(str(macho))
        run_cmd(cmd)

print(f"    Normalized LC_RPATH on {len(list_all_machos())} Mach-O binaries.")
PYEOF

echo "==> Code signing inside-out (identity: ${SIGN_IDENTITY})"
chmod -R u+w "$APP"
xattr -cr "$APP" 2>/dev/null || true

CODESIGN_BASE=(codesign --force --sign "$SIGN_IDENTITY")
if [[ -n "$KEYCHAIN" ]]; then
    CODESIGN_BASE+=(--keychain "$KEYCHAIN")
fi
if [[ "$SIGN_IDENTITY" != "-" ]]; then
    CODESIGN_BASE+=(--options runtime --timestamp)
fi

sign_item() {
    local target="$1"
    shift || true
    "${CODESIGN_BASE[@]}" "$@" "$target"
}

# 1. Sign all Mach-O plugins/dylibs under Contents/PlugIns and Contents/Resources
while IFS= read -r -d '' bin; do
    sign_item "$bin"
done < <(find "${APP}/Contents/PlugIns" "${APP}/Contents/Resources" -type f \( -name "*.dylib" -o -name "*.so" \) -print0)

# 2. Sign plain dylibs in Contents/Frameworks (non-symlink files)
while IFS= read -r -d '' dylib; do
    sign_item "$dylib"
done < <(find "${APP}/Contents/Frameworks" -maxdepth 1 -type f -name "*.dylib" -print0)

# 3. Sign framework bundles in Contents/Frameworks
while IFS= read -r -d '' fw; do
    sign_item "$fw"
done < <(find "${APP}/Contents/Frameworks" -maxdepth 1 -type d -name "*.framework" -print0)

# 4. Sign helper executables beside the main binary
if [[ -f "${APP}/Contents/MacOS/choruscut-mcp" ]]; then
    sign_item "${APP}/Contents/MacOS/choruscut-mcp" --identifier "${BUNDLE_ID}.mcp"
fi

# 5. Sign the main app bundle
sign_item "$APP" --identifier "$BUNDLE_ID"

echo "==> Running hard self-containment and code-signature verification"
python3 - "$APP" <<'PYEOF'
import os
import re
import subprocess
import sys
from pathlib import Path

app = Path(sys.argv[1]).resolve()
contents = app / "Contents"
frameworks = contents / "Frameworks"

MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
}

def is_macho(p: Path) -> bool:
    if p.is_symlink() or not p.is_file():
        return False
    with p.open("rb") as f:
        return f.read(4) in MACHO_MAGICS

errors = []
checked = 0
for p in sorted(contents.rglob("*")):
    if not is_macho(p):
        continue
    checked += 1
    out_L = subprocess.check_output(["otool", "-L", str(p)], text=True)
    out_D = subprocess.check_output(["otool", "-D", str(p)], text=True).splitlines()
    own_id = out_D[1].strip() if len(out_D) >= 2 else None
    seen_own_id = 0
    for line in out_L.splitlines()[1:]:
        dep = line.strip().split(" (", 1)[0]
        if not dep:
            continue
        if own_id and dep == own_id:
            seen_own_id += 1
            if seen_own_id > 1:
                errors.append(f"[SELF DEP] {p.relative_to(app)} links its own LC_ID_DYLIB {own_id}")
        if any(bad in dep for bad in ("/opt/homebrew", "/usr/local", "/opt/local")):
            errors.append(f"[EXTERNAL REF] {p.relative_to(app)} -> {dep}")
        elif dep.startswith("/") and not dep.startswith(("/System/Library/", "/usr/lib/")):
            errors.append(f"[NON-SYSTEM ABS REF] {p.relative_to(app)} -> {dep}")
        elif dep.startswith("@rpath/"):
            rel = dep[len("@rpath/"):]
            if not (frameworks / rel).exists():
                errors.append(f"[MISSING @rpath] {p.relative_to(app)} -> {dep}")

    out_l = subprocess.check_output(["otool", "-l", str(p)], text=True)
    lines = out_l.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" in line:
            for j in range(i + 1, min(i + 6, len(lines))):
                m = re.match(r"\s*path\s+(.*?)\s+\(offset\s+\d+\)", lines[j])
                if m:
                    rp = m.group(1)
                    if any(bad in rp for bad in ("/opt/homebrew", "/usr/local", "/opt/local", "/Users/", "/tmp/")):
                        errors.append(f"[FORBIDDEN RPATH] {p.relative_to(app)} -> {rp}")
                    break

required_paths = [
    "Contents/MacOS/ChorusCut",
    "Contents/MacOS/choruscut-mcp",
    "Contents/Resources/LICENSE",
    "Contents/Resources/THIRD_PARTY_LICENSES.txt",
    "Contents/Resources/qt.conf",
    "Contents/Resources/align.py",
    "Contents/Resources/mlt-7/profiles/atsc_720p_24",
    "Contents/PlugIns/mlt-7/libmltcore.so",
    "Contents/PlugIns/mlt-7/libmltavformat.so",
    "Contents/PlugIns/mlt-7/libmltsdl2.so",
    "Contents/PlugIns/mlt-7/libmltnormalize.so",
    "Contents/PlugIns/mlt-7/libmltqt6.so",
    "Contents/PlugIns/mlt-7/libmltresample.so",
    "Contents/PlugIns/platforms/libqcocoa.dylib",
    "Contents/PlugIns/platforms/libqoffscreen.dylib",
    "Contents/PlugIns/sqldrivers/libqsqlite.dylib",
    "Contents/PlugIns/multimedia/libdarwinmediaplugin.dylib",
]
for rel in required_paths:
    if not (app / rel).exists():
        errors.append(f"[MISSING REQUIRED FILE] {rel}")

# Verify libSDL3.dylib if libSDL2 is sdl2-compat
sdl2_dylib = frameworks / "libSDL2-2.0.0.dylib"
if sdl2_dylib.exists():
    data = sdl2_dylib.read_bytes()
    if b"libSDL3.dylib" in data and not (frameworks / "libSDL3.dylib").exists():
        errors.append("[MISSING SDL3] sdl2-compat requires Contents/Frameworks/libSDL3.dylib")

if errors:
    print("Self-containment verification FAILED:", file=sys.stderr)
    for e in errors:
        print("  " + e, file=sys.stderr)
    sys.exit(1)

print(f"    Verified {checked} Mach-O binaries: ZERO /opt/homebrew or /usr/local references, all @rpath targets present.")
PYEOF

codesign --verify --deep --strict --verbose=2 "$APP"

if [[ "$KEEP_APP" -eq 1 ]]; then
    echo "==> Copying packaged app to ${OUT_DIR}/ChorusCut.app"
    rm -rf "${OUT_DIR}/ChorusCut.app"
    ditto "$APP" "${OUT_DIR}/ChorusCut.app"
fi

if [[ "$SKIP_DMG" -eq 0 ]]; then
    DMG_NAME="ChorusCut-${VERSION}-macos-${ARCH}.dmg"
    DMG_PATH="${OUT_DIR}/${DMG_NAME}"
    DMG_ROOT="${STAGE}/dmg-root"
    mkdir -p "$DMG_ROOT"
    ditto "$APP" "${DMG_ROOT}/ChorusCut.app"
    ln -s /Applications "${DMG_ROOT}/Applications"

    echo "==> Building compressed disk image: ${DMG_PATH}"
    rm -f "$DMG_PATH"
    for attempt in 1 2 3; do
        if hdiutil create \
            -volname "ChorusCut ${VERSION}" \
            -srcfolder "$DMG_ROOT" \
            -ov \
            -format UDZO \
            -imagekey zlib-level=9 \
            "$DMG_PATH"; then
            break
        fi
        if [[ "$attempt" -eq 3 ]]; then
            echo "Error: hdiutil create failed after 3 attempts." >&2
            exit 1
        fi
        echo "    hdiutil create retry ${attempt}/3..."
        sleep 2
    done

    if [[ "$SIGN_IDENTITY" != "-" ]]; then
        echo "==> Signing disk image: ${DMG_PATH}"
        DMG_SIGN_CMD=(codesign --force --sign "$SIGN_IDENTITY" --timestamp)
        if [[ -n "$KEYCHAIN" ]]; then
            DMG_SIGN_CMD+=(--keychain "$KEYCHAIN")
        fi
        "${DMG_SIGN_CMD[@]}" "$DMG_PATH"
    fi
fi

if [[ "$DMG_ONLY" -eq 0 ]]; then
    NODE_SRC="${REPO_ROOT}/comfyui/ComfyUI-ChorusCut"
    if [[ ! -d "$NODE_SRC" ]]; then
        echo "Error: ComfyUI node pack directory not found at ${NODE_SRC}" >&2
        exit 1
    fi
    ZIP_NAME="ComfyUI-ChorusCut-${VERSION}.zip"
    ZIP_PATH="${OUT_DIR}/${ZIP_NAME}"
    echo "==> Packaging ComfyUI node pack: ${ZIP_PATH}"
    rm -f "$ZIP_PATH"
    (
        cd "${REPO_ROOT}/comfyui"
        zip -r -y "$ZIP_PATH" "ComfyUI-ChorusCut" \
            -x "*.DS_Store" \
            -x "*/.DS_Store" \
            -x "*/__pycache__/*" \
            -x "*/__pycache__" \
            -x "*.pyc" \
            -x "*.pyo" \
            -x "*/._*" \
            -x "._*" \
            -x "*/.git/*"
    )
fi

write_checksums "$OUT_DIR"

echo "==> Done! Release artifacts in ${OUT_DIR}:"
ls -lh "$OUT_DIR"
