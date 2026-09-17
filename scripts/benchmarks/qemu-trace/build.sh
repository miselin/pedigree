#!/bin/sh
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 /path/to/qemu/include /path/to/plugin.so [trace|profile]" >&2
    exit 2
fi
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
plugin_name=${3:-trace}
case $plugin_name in trace|profile) ;; *) exit 2 ;; esac
case $(uname -s) in
    Darwin) shared_flags='-dynamiclib -undefined dynamic_lookup' ;;
    *) shared_flags='-shared -fPIC' ;;
esac
# pkg-config and shared_flags intentionally expand to compiler argument lists.
${CC:-cc} -std=c11 -O2 -g -Wall -Wextra -Werror -fvisibility=hidden \
    $shared_flags -I"$1" $(pkg-config --cflags glib-2.0) \
    "$source_dir/$plugin_name.c" -o "$2" $(pkg-config --libs glib-2.0)
