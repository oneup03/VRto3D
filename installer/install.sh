#!/usr/bin/env bash
# VRto3D Linux installer: copies the driver into SteamVR's drivers/ folder
# (auto-discovered, same layout as the Windows install) and registers it in
# steamvr.vrsettings. Ships at the root of vrto3d-linux64.tar.gz next to the
# drivers/ payload. No dependencies beyond coreutils + awk.
#
# Usage: ./install.sh [--uninstall] [--steam <steam-root>]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNINSTALL=0
STEAM_ROOT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --uninstall) UNINSTALL=1 ;;
        --steam) STEAM_ROOT="${2:?--steam needs a path}"; shift ;;
        -h|--help)
            echo "Usage: $0 [--uninstall] [--steam <steam-root>]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

die() { echo "error: $*" >&2; exit 1; }

# --- locate the Steam root ---------------------------------------------------
if [ -z "$STEAM_ROOT" ]; then
    for cand in \
        "$HOME/.local/share/Steam" \
        "$HOME/.steam/steam" \
        "$HOME/.steam/root" \
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        if [ -d "$cand/steamapps" ]; then
            STEAM_ROOT="$(readlink -f "$cand")"
            break
        fi
    done
fi
[ -n "$STEAM_ROOT" ] && [ -d "$STEAM_ROOT/steamapps" ] \
    || die "Steam not found - pass its root with --steam <path>"

# --- locate SteamVR (may live in any Steam library folder) -------------------
STEAMVR=""
LIBS="$STEAM_ROOT"
VDF="$STEAM_ROOT/steamapps/libraryfolders.vdf"
if [ -f "$VDF" ]; then
    LIBS="$LIBS
$(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$VDF")"
fi
while IFS= read -r lib; do
    if [ -n "$lib" ] && [ -d "$lib/steamapps/common/SteamVR" ]; then
        STEAMVR="$lib/steamapps/common/SteamVR"
        break
    fi
done <<EOF
$LIBS
EOF
[ -n "$STEAMVR" ] || die "SteamVR not found in any Steam library - install it from Steam first"

DRIVER_DST="$STEAMVR/drivers/vrto3d"
VRSETTINGS="$STEAM_ROOT/config/steamvr.vrsettings"

# --- steamvr.vrsettings editing (awk, no python/jq dependency) ----------------
# The file is machine-written by SteamVR (one "key" : value per line), which a
# line-based edit handles fine; any unexpected shape falls back to writing a
# fresh minimal file (the Windows installer's reset behavior) after a backup.

write_fresh_vrsettings() {
    mkdir -p "$(dirname "$VRSETTINGS")"
    cat > "$VRSETTINGS" <<'EOF'
{
   "steamvr" : {
      "requireHmd" : true,
      "forcedDriver" : "vrto3d",
      "activateMultipleDrivers" : true
   }
}
EOF
}

# Replace/insert the three managed keys, preserving everything else.
# Exits non-zero when the file shape is unexpected; caller falls back.
patch_vrsettings() {
    awk '
    BEGIN { injected = 0; bad = 0; n = 0 }
    # drop existing occurrences of the keys we manage
    /^[[:space:]]*"(requireHmd|forcedDriver|activateMultipleDrivers)"[[:space:]]*:/ { next }
    {
        if (!injected && index($0, "\"steamvr\"") > 0) {
            if ($0 ~ /"steamvr"[[:space:]]*:[[:space:]]*[{][[:space:]]*[}],?[[:space:]]*$/) {
                # empty inline section: expand it
                match($0, /^[[:space:]]*/); ind = substr($0, 1, RLENGTH)
                tail = ""; if ($0 ~ /,[[:space:]]*$/) tail = ","
                out[n++] = ind "\"steamvr\" : {"
                out[n++] = ind "   \"requireHmd\" : true,"
                out[n++] = ind "   \"forcedDriver\" : \"vrto3d\","
                out[n++] = ind "   \"activateMultipleDrivers\" : true"
                out[n++] = ind "}" tail
                injected = 1
                next
            }
            if ($0 ~ /"steamvr"[[:space:]]*:[[:space:]]*[{][[:space:]]*$/) {
                # multiline section: inject right after the opening brace
                match($0, /^[[:space:]]*/); ind = substr($0, 1, RLENGTH)
                out[n++] = $0
                out[n++] = ind "   \"requireHmd\" : true,"
                out[n++] = ind "   \"forcedDriver\" : \"vrto3d\","
                out[n++] = ind "   \"activateMultipleDrivers\" : true,"
                injected = 1
                next
            }
            bad = 1; exit   # "steamvr" in a shape we do not understand
        }
        out[n++] = $0
    }
    END {
        if (bad) exit 2
        if (!injected) {
            # no steamvr section: add one right after the top-level brace
            if (n == 0 || out[0] !~ /^[[:space:]]*[{][[:space:]]*$/) exit 2
            m = 0; new[m++] = out[0]
            new[m++] = "   \"steamvr\" : {"
            new[m++] = "      \"requireHmd\" : true,"
            new[m++] = "      \"forcedDriver\" : \"vrto3d\","
            new[m++] = "      \"activateMultipleDrivers\" : true"
            new[m++] = "   },"
            for (i = 1; i < n; i++) new[m++] = out[i]
            n = m
            for (i = 0; i < n; i++) out[i] = new[i]
        }
        # drop commas left dangling before a closing brace/bracket
        for (i = 0; i < n; i++) {
            if (i + 1 < n && out[i] ~ /,[[:space:]]*$/ && out[i+1] ~ /^[[:space:]]*[]}]/)
                sub(/,[[:space:]]*$/, "", out[i])
            print out[i]
        }
    }' "$VRSETTINGS" > "$VRSETTINGS.tmp" || { rm -f "$VRSETTINGS.tmp"; return 1; }
    mv "$VRSETTINGS.tmp" "$VRSETTINGS"
}

# Remove the keys this installer set (only when VRto3D is the forced driver).
strip_vrsettings() {
    grep -q '"forcedDriver"[[:space:]]*:[[:space:]]*"vrto3d"' "$VRSETTINGS" || return 0
    awk '
    /^[[:space:]]*"(requireHmd|forcedDriver)"[[:space:]]*:/ { next }
    { out[n++] = $0 }
    END {
        for (i = 0; i < n; i++) {
            if (i + 1 < n && out[i] ~ /,[[:space:]]*$/ && out[i+1] ~ /^[[:space:]]*[]}]/)
                sub(/,[[:space:]]*$/, "", out[i])
            print out[i]
        }
    }' "$VRSETTINGS" > "$VRSETTINGS.tmp" || { rm -f "$VRSETTINGS.tmp"; return 1; }
    mv "$VRSETTINGS.tmp" "$VRSETTINGS"
}

# --- uninstall ----------------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
    rm -rf "$DRIVER_DST"
    [ -f "$VRSETTINGS" ] && strip_vrsettings
    echo "VRto3D removed from $STEAMVR"
    echo "(config/profiles left in $STEAM_ROOT/config/vrto3d - delete manually if unwanted)"
    exit 0
fi

# --- install --------------------------------------------------------------------
[ -d "$SCRIPT_DIR/drivers/vrto3d" ] || die "drivers/vrto3d not found next to install.sh"

rm -rf "$DRIVER_DST"
mkdir -p "$STEAMVR/drivers"
cp -a "$SCRIPT_DIR/drivers/vrto3d" "$DRIVER_DST"

# Register as the forced HMD driver (the settings the README documents).
if [ -f "$VRSETTINGS" ]; then
    cp "$VRSETTINGS" "$VRSETTINGS.bak"
    if ! patch_vrsettings; then
        echo "note: could not merge into steamvr.vrsettings - writing a fresh one" >&2
        echo "      (previous file kept at $VRSETTINGS.bak)" >&2
        write_fresh_vrsettings
    fi
else
    write_fresh_vrsettings
fi

echo "VRto3D installed to $DRIVER_DST"

# Hotkeys/gamepad read /dev/input directly, which needs the input group.
if ! id -nG | grep -qw input; then
    echo
    echo "NOTE: your user is not in the 'input' group - hotkeys and gamepad"
    echo "input will not work until you run:"
    echo "    sudo usermod -aG input ${USER:-$(id -un)}"
    echo "and log out and back in."
fi

echo
echo "Done. Start SteamVR - the 3D output window should appear on your display."
echo "Open the OSD with Ctrl + Home to pick display, output mode, and resolution."
