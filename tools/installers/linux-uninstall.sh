#!/bin/sh
set -eu
case "${1:-}" in --yes|'') ;; *) printf '%s\n' 'Usage: sh uninstall.sh [--yes]' >&2; exit 2 ;; esac
install_dir=$(cd "$(dirname "$0")" && pwd -P)
[ "$(cat "$install_dir/installer-owned.txt" 2>/dev/null)" = OhMy3DO-User-Install-1 ] || { printf '%s\n' 'This is not an installed copy of Oh My 3DO Library.' >&2; exit 1; }
for process in /proc/[0-9]*/exe; do
  running=$(readlink "$process" 2>/dev/null || :)
  if [ "$running" = "$install_dir/OhMy3DOLibrary" ]; then printf '%s\n' 'Close Oh My 3DO Library before uninstalling.' >&2; exit 1; fi
done
if [ "${1:-}" != --yes ]; then
  printf '%s\n' 'Remove Oh My 3DO Library and its menu shortcut? Games, saves, preferences and metadata backups will be kept.'
  printf 'Continue [y/N]? '
  IFS= read -r answer || exit 1
  case "$answer" in y|Y|yes|YES) ;; *) printf '%s\n' 'Uninstallation canceled.'; exit 0 ;; esac
fi
desktop_file=$(cat "$install_dir/desktop-path.txt")
case "$desktop_file" in
  /*/applications/com.ohmy3do.library.desktop)
    if [ -f "$desktop_file" ] && grep -Fqx "X-OhMy3DO-Install=$install_dir" "$desktop_file"; then rm -- "$desktop_file"; fi ;;
esac
# Remove application files only. Never recursively remove the install directory.
for name in OhMy3DOLibrary OhMy3DOLibrary.png LICENSE README.md THIRD_PARTY.md BUILD.json; do
  rm -f -- "$install_dir/$name"
done
rm -f -- "$install_dir"/.ohmy3do-previous-* "$install_dir"/.ohmy3do-update-*
# All dependency notices are owned by the application, including self-updates.
rm -rf -- "$install_dir/licenses"
rm -f -- "$install_dir/installer-owned.txt" "$install_dir/desktop-path.txt" "$install_dir/uninstall.sh"
rmdir -- "$install_dir" 2>/dev/null || printf '%s\n' 'Additional files in the application folder were kept.'
printf '%s\n' 'Oh My 3DO Library uninstalled. Your games, saves and preferences were kept.'
