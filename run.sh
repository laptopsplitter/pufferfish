#!/bin/sh
# Starts tonic from this folder (without installing it; data/ is kept here):
#   ./run.sh               the Roblox it downloaded
#   ./run.sh --link URL    a Roblox link: tonic starts, or the running one joins
cd "$(dirname "$0")" || exit 1
lib=auto
while [ $# -gt 0 ]; do
  case "$1" in
    # In the environment, not the command line: a website Play link carries a
    # sign-in ticket, and other programs can read command lines.
    --link) shift; if [ $# -gt 0 ]; then TONIC_LINK=$1; export TONIC_LINK; shift; fi ;;
    *) lib=$1; shift ;;
  esac
done
exec ./tonic "$lib" --window --script scripts/startup.script --values scripts/roblox.values
