#!/bin/sh
# Remove an Oh My 3DO Library installed by the Linux installer.
#
# Usage: sh uninstall.sh [--yes]
#
# Removes the app's own files and its menu shortcut, never the user's games,
# saves or settings. The install folder itself is removed only if nothing
# else was put in it.
set -eu

fail() {
  printf '%s\n' "$1" >&2
  exit 1
}

case "${1:-}" in
  --yes|'') ;;
  *) printf '%s\n' 'Usage: sh uninstall.sh [--yes]' >&2; exit 2 ;;
esac
install_dir=$(cd "$(dirname "$0")" && pwd -P)

# Only act on a folder the installer made, with the app closed.
if [ "$(cat "$install_dir/installer-owned.txt" 2>/dev/null)" != OhMy3DO-User-Install-1 ]; then
  fail 'This is not an installed copy of Oh My 3DO Library.'
fi
for process in /proc/[0-9]*/exe; do
  running=$(readlink "$process" 2>/dev/null || :)
  if [ "$running" = "$install_dir/OhMy3DOLibrary" ]; then
    fail 'Close Oh My 3DO Library before uninstalling.'
  fi
done

if [ "${1:-}" != --yes ]; then
  printf '%s\n' 'Remove Oh My 3DO Library and its menu shortcut? Games, saves, preferences and metadata backups will be kept.'
  printf 'Continue [y/N]? '
  IFS= read -r answer || exit 1
  case "$answer" in
    y|Y|yes|YES) ;;
    *) printf '%s\n' 'Uninstallation canceled.'; exit 0 ;;
  esac
fi

# The shortcut, only if it is where the installer put it and is marked as ours.
desktop_file=$(cat "$install_dir/desktop-path.txt")
case "$desktop_file" in
  /*/applications/com.ohmy3do.library.desktop)
    if [ -f "$desktop_file" ] && grep -Fqx "X-OhMy3DO-Install=$install_dir" "$desktop_file"; then
      rm -- "$desktop_file"
    fi ;;
esac

# Remove application files only. Never recursively remove the install directory.
for name in OhMy3DOLibrary OhMy3DOLibrary.png LICENSE README.md THIRD_PARTY.md BUILD.json; do
  rm -f -- "$install_dir/$name"
done
# Leftovers of an interrupted self-update.
rm -f -- "$install_dir"/.ohmy3do-previous-* "$install_dir"/.ohmy3do-update-*
# All dependency notices are owned by the application, including self-updates.
rm -rf -- "$install_dir/licenses"
rm -f -- "$install_dir/installer-owned.txt" "$install_dir/desktop-path.txt" "$install_dir/uninstall.sh"
rmdir -- "$install_dir" 2>/dev/null || printf '%s\n' 'Additional files in the application folder were kept.'
printf '%s\n' 'Oh My 3DO Library uninstalled. Your games, saves and preferences were kept.'
