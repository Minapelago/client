#!/usr/bin/env bash
# Fetch a Windows depot for Mina the Hollower so signature work targets the live shipping build. Re-run after a game update.
#
#   scripts/fetch-windows-depot.sh <steam-username> [outdir] [branch] [betapassword]
#
# branch defaults to experimental-modding (where the native modloader ships); pass "public" for the stable build. The first run logs in
# interactively (Steam Guard 2FA) and steamcmd then caches credentials. Only the ~20 MB Windows binaries depot is pulled.
#
# App/depot IDs (from `steamcmd +app_info_print 1875580`):
#   app 1875580; depot 1875582 = oslist "windows" (MinaTheHollower.exe, handler.exe, steam_api64.dll).
#
# A non-public branch has its own per-depot manifest and download_depot takes no branch name, so for those we resolve depot 1875582's manifest gid
# on that branch with app_info_print and download that manifest.
set -euo pipefail

APP=1875580       # Mina the Hollower
WIN_DEPOT=1875582 # the oslist=windows binaries depot
STEAMCMD=${STEAMCMD:-/usr/games/steamcmd}
STEAM_USER=${1:?usage: fetch-windows-depot.sh <steam-username> [outdir] [branch] [betapassword]}
OUTDIR=${2:-$HOME/minathehollower/windows-current}
BRANCH=${3:-experimental-modding}
BETAPW=${4:-}

# @sSteamCmdForcePlatformType windows must precede +login to select the Windows depot on Linux.
# stdin is closed (</dev/null): without a TTY an uncached +login would hang on steamcmd's password prompt. Closing it makes the login fail fast so
# assert_login_ok can report it. Cache creds via RELOGIN_HINT.
run_steamcmd() { "$STEAMCMD" +@sSteamCmdForcePlatformType windows +login "$STEAM_USER" "$@" +quit </dev/null; }

# steamcmd caches a login token after one interactive login; a Steam client self-update or unclean shutdown can wipe it, and a non-interactive
# +login then fails with the error buried in the piped log. Scan the log and print an actionable message; the interactive re-login re-caches the token.
RELOGIN_HINT="$STEAMCMD +@sSteamCmdForcePlatformType windows +login $STEAM_USER +quit"
assert_login_ok()
{
    local log=$1
    if grep -qiE 'Cached credentials not found|Invalid Password|Rate Limit|Two[ -]?Factor|Account Logon Denied|Invalid Login Auth Code|Login Failure|Logging in user .* (FAILED|ERROR)' "$log"; then
        echo "!! steamcmd could not log in as '$STEAM_USER' (cached credentials missing or expired)." >&2
        echo "!! Re-cache them ONCE in an interactive terminal (answer the password + Steam Guard prompts):" >&2
        echo "!!   $RELOGIN_HINT" >&2
        echo "!! then re-run this script. Full steamcmd output: $log" >&2
        exit 1
    fi
}

MANIFEST=""
if [ "$BRANCH" != "public" ]; then
    echo ">> steamcmd: resolving depot $WIN_DEPOT manifest on branch '$BRANCH' (login: $STEAM_USER)"
    # A password-protected beta must be unlocked once via app_update -beta so app_info shows its manifest.
    UPDATE_ARGS=()
    [ -n "$BETAPW" ] && UPDATE_ARGS=(+app_update "$APP" -beta "$BRANCH" -betapassword "$BETAPW")
    run_steamcmd "${UPDATE_ARGS[@]}" +app_info_update 1 +app_info_print "$APP" >/tmp/steamcmd_appinfo.log 2>&1 || true
    assert_login_ok /tmp/steamcmd_appinfo.log

    # The log path is an argv: `python3 -` already consumes stdin for the heredoc.
    MANIFEST=$(python3 - "$WIN_DEPOT" "$BRANCH" /tmp/steamcmd_appinfo.log <<'PY'
import re, sys
depot, branch, logpath = sys.argv[1], sys.argv[2], sys.argv[3]
# Strip ANSI, then walk the app_info VDF as a token stream of quoted strings and braces.
text = re.sub(r"\x1b\[[0-9;]*m", "", open(logpath, encoding="utf-8", errors="replace").read())
toks = re.findall(r'"((?:[^"\\]|\\.)*)"|(\{)|(\})', text)
stack, out, pending_key = [], None, None
# Path tracker: a key pushes scope on '{', otherwise it takes the next scalar as its value.
for s, ob, cb in toks:
    if ob:
        stack.append(pending_key if pending_key is not None else "<anon>"); pending_key = None
    elif cb:
        if stack: stack.pop()
    else:
        if pending_key is None:
            pending_key = s
        else:
            path = stack + [pending_key]  # full path of this key->scalar
            if (len(path) >= 5 and path[-5] == "depots" and path[-4] == depot
                    and path[-3] == "manifests" and path[-2] == branch and path[-1] == "gid"):
                out = s
            pending_key = None
print(out or "")
PY
)
    [ -n "$MANIFEST" ] || { echo "!! could not resolve manifest for depot $WIN_DEPOT on branch '$BRANCH' (no access to the beta, or wrong branch name?)"; exit 1; }
    echo ">> resolved manifest: $MANIFEST"
fi

echo ">> steamcmd: downloading app $APP windows depot $WIN_DEPOT${MANIFEST:+ manifest $MANIFEST} (branch: $BRANCH)"
run_steamcmd +download_depot "$APP" "$WIN_DEPOT" $MANIFEST 2>&1 | tee /tmp/steamcmd_fetch.log || true
assert_login_ok /tmp/steamcmd_fetch.log

# download_depot stages under <steam home>/steamapps/content/app_<APP>/depot_<DEPOT>/ and prints the path. In windows-platform mode it prints
# `Depot download complete : "<path-with-backslashes>" (manifest <gid>)`, so extract the quoted path and normalize backslashes.
SRC=$(sed -n 's/^Depot download complete : "\([^"]*\)".*/\1/p' /tmp/steamcmd_fetch.log | tr -d '\r' | tr '\\' '/' | tail -1)
if [ -z "${SRC:-}" ] || [ ! -d "$SRC" ]; then
    # -print -quit stops at the first match; `| head` would leave find writing into a closed pipe (SIGPIPE), which kills the script under
    # pipefail + `set -e`. `|| true` lets the guard below report a no-match.
    SRC=$(find "$HOME/.local/share/Steam/steamcmd" "$HOME/.steam" "$HOME/Steam" \
        -type d -path "*content/app_${APP}/depot_${WIN_DEPOT}" -print -quit 2>/dev/null || true)
fi
[ -n "${SRC:-}" ] && [ -d "$SRC" ] || { echo "!! could not locate downloaded depot dir"; exit 1; }

EXE=$(find "$SRC" -iname MinaTheHollower.exe -print -quit 2>/dev/null || true)
[ -n "$EXE" ] || { echo "!! MinaTheHollower.exe not found under $SRC"; exit 1; }

mkdir -p "$OUTDIR"
cp -f "$EXE" "$OUTDIR/MinaTheHollower.exe"
for f in handler.exe steam_api64.dll; do
    g=$(find "$SRC" -iname "$f" | head -1) || true
    [ -n "${g:-}" ] && cp -f "$g" "$OUTDIR/$f" || true
done

# Build id = TimeDateStamp:SizeOfImage (hex), the value the mod logs as build_id=.
BID=$(python3 - "$OUTDIR/MinaTheHollower.exe" <<'PY'
import sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
print(f"{pe.FILE_HEADER.TimeDateStamp:x}:{pe.OPTIONAL_HEADER.SizeOfImage:08x}")
PY
)
echo ">> staged $OUTDIR/MinaTheHollower.exe   build_id=$BID   (branch=$BRANCH)"
