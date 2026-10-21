#!/bin/sh
# Self-extracting, per-user installer. The payload and notices come from the portable package.
set -eu
assume_yes=false
case "${1:-}" in
  --help|-h)
    printf '%s\n' 'Oh My 3DO Library installer' 'Usage: sh INSTALLER.run [--yes]' 'Installs for the current user; no sudo. Requires GTK 3 and WebKitGTK 4.1.'
    exit 0 ;;
  --yes) assume_yes=true ;;
  '') ;;
  *) printf '%s\n' 'Unknown argument. Use --help.' >&2; exit 2 ;;
esac
[ "$(id -u)" -ne 0 ] || { printf '%s\n' 'Run this installer as your normal user, without sudo.' >&2; exit 1; }
[ "$(uname -m)" = x86_64 ] || { printf '%s\n' 'This installer requires x86-64 Linux.' >&2; exit 1; }
: "${HOME:?Set HOME before installing}"
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
case "$data_home" in /*) ;; *) printf '%s\n' 'XDG_DATA_HOME must be an absolute path.' >&2; exit 1 ;; esac
carriage=$(printf '\r')
case "$data_home" in *'
'*|*"$carriage"*) printf '%s\n' 'The install path cannot contain line breaks.' >&2; exit 1 ;; esac
data_home=$(readlink -m "$data_home")
install_dir="$data_home/oh-my-3do-library"
desktop_file="$data_home/applications/com.ohmy3do.library.desktop"
[ ! -L "$install_dir" ] || { printf '%s\n' 'The install folder must not be a symbolic link.' >&2; exit 1; }
if [ -e "$install_dir" ] && [ ! -f "$install_dir/installer-owned.txt" ]; then
  printf 'Folder already exists and is not an installed copy. Move it aside first:\n%s\n' "$install_dir" >&2
  exit 1
fi
if [ -d "$install_dir" ] && find "$install_dir" -type l -print -quit | grep -q .; then
  printf '%s\n' 'The app folder contains symbolic links. Move it aside before reinstalling.' >&2; exit 1
fi
for process in /proc/[0-9]*/exe; do
  running=$(readlink "$process" 2>/dev/null || :)
  if [ "$running" = "$install_dir/OhMy3DOLibrary" ]; then
    printf '%s\n' 'Close Oh My 3DO Library before installing.' >&2; exit 1
  fi
done
printf 'Install Oh My 3DO Library @@VERSION@@ for your account?\nLocation: %s\nGames, saves and settings will be kept.\n' "$install_dir"
if [ "$assume_yes" = false ]; then
  printf 'Continue [y/N]? '
  IFS= read -r answer || exit 1
  case "$answer" in y|Y|yes|YES) ;; *) printf '%s\n' 'Installation canceled.'; exit 0 ;; esac
fi
scratch=$(mktemp -d "${TMPDIR:-/tmp}/ohmy3do-install.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
trap 'exit 1' HUP INT TERM
archive_line=$(awk '/^__OHMY3DO_PAYLOAD__$/ {print NR+1; exit}' "$0")
[ -n "$archive_line" ] || { printf '%s\n' 'Installer payload missing.' >&2; exit 1; }
tail -n +"$archive_line" "$0" > "$scratch/payload.tar.gz"
printf '%s  %s\n' '@@PAYLOAD_SHA256@@' "$scratch/payload.tar.gz" | sha256sum -c -
mkdir "$scratch/payload"
tar -xzf "$scratch/payload.tar.gz" -C "$scratch/payload" --no-same-owner --no-same-permissions
if command -v ldd >/dev/null 2>&1; then
  ldd "$scratch/payload/OhMy3DOLibrary" > "$scratch/libraries.txt" 2>&1 || :
  if grep -q 'not found' "$scratch/libraries.txt"; then
    printf '%s\n' 'Install the missing system libraries before running this installer again:' >&2
    grep 'not found' "$scratch/libraries.txt" >&2
    printf '%s\n' 'On Debian/Ubuntu, install GTK 3 and WebKitGTK 4.1 (libgtk-3-0 / libwebkit2gtk-4.1-0, or their t64 equivalents).' >&2
    exit 1
  fi
fi
[ ! -L "$desktop_file" ] || { printf '%s\n' 'The menu shortcut must not be a symbolic link.' >&2; exit 1; }
if [ -f "$desktop_file" ] && ! grep -Fqx "X-OhMy3DO-Install=$install_dir" "$desktop_file"; then
  printf 'An unrelated menu shortcut already exists: %s\n' "$desktop_file" >&2; exit 1
fi
mkdir -p "$install_dir" "$data_home/applications"
printf '%s\n' 'OhMy3DO-User-Install-1' > "$install_dir/installer-owned.txt"
cp -R "$scratch/payload/." "$install_dir/"
chmod 755 "$install_dir/OhMy3DOLibrary" "$install_dir/uninstall.sh"
printf '%s\n' "$desktop_file" > "$install_dir/desktop-path.txt"
# Desktop Entry values and quoted Exec arguments have separate escaping rules.
desktop_value() { printf '%s' "$1" | sed 's/\\/\\\\/g'; }
desktop_exec() { printf '%s' "$1" | sed -e 's/\\/\\\\\\\\/g' -e 's/"/\\\\"/g' -e 's/\$/\\\\$/g' -e 's/`/\\\\`/g' -e 's/%/%%/g'; }
{
  printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=Oh My 3DO Library' 'Comment=Manage your 3DO library, media and save backups'
  printf 'Exec="%s"\n' "$(desktop_exec "$install_dir/OhMy3DOLibrary")"
  printf 'TryExec=%s\n' "$(desktop_value "$install_dir/OhMy3DOLibrary")"
  printf 'Icon=%s\n' "$(desktop_value "$install_dir/OhMy3DOLibrary.png")"
  printf '%s\n' 'Terminal=false' 'Categories=Game;Utility;' 'StartupNotify=true'
  printf 'X-OhMy3DO-Install=%s\n' "$install_dir"
} > "$desktop_file"
chmod 644 "$desktop_file"
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database "$data_home/applications" >/dev/null 2>&1 || :; fi
printf 'Installed. Start Oh My 3DO Library from your application menu.\nTo uninstall:\n  sh "%s/uninstall.sh"\n' "$install_dir"
exit 0
__OHMY3DO_PAYLOAD__
