#!/bin/sh
# install.sh [ssh host]: puts build/bbcoop.so in bbport's plugins folder, and
# build/mod in its mods folder as "Co-op - menu and text", on this machine or,
# given one, on that ssh host (a Steam Deck, say). Nothing else is changed.
# The data folder is the AppImage's, ~/.local/share/bbport; BB_DATA_DIR names another.
set -eu
cd "$(dirname "$0")"
[ -f build/bbcoop.so ] || ./build.sh
mod="Co-op - menu and text"  # build/mod, made by build/coop_assets (README)
case $# in
0) on="sh -c" where="this machine" ;;
1) on="ssh $1" where=$1 ;;
*)
    echo "usage: install.sh [ssh host]" >&2
    exit 2
    ;;
esac
data=${BB_DATA_DIR:-'$HOME/.local/share/bbport'}  # $HOME: the installing shell's
# a new file, renamed over the old one: a running game keeps the copy it loaded
$on "mkdir -p \"$data/plugins\" && cat > \"$data/plugins/.bbcoop.so.new\" && mv -f \"$data/plugins/.bbcoop.so.new\" \"$data/plugins/bbcoop.so\"" < build/bbcoop.so ||
    { echo "error: could not install to $where; nothing installed" >&2; exit 1; }
echo "installed bbcoop.so to $where"
if [ -d build/mod/dvdroot_ps4 ]; then
    tar -C build/mod -cf - dvdroot_ps4 | $on "rm -rf \"$data/mods/$mod\" && mkdir -p \"$data/mods/$mod\" && tar -C \"$data/mods/$mod\" -xf -"
    echo "installed the mod \"$mod\" to $where"
fi
cat <<END
In bbport.ini on $where (or the launcher's co-op group):

online=1
online.host=<the server's address>
plugins.bbcoop=1
bbcoop.role=host              # or guest
bbcoop.partner=<the other machine's address>
END
