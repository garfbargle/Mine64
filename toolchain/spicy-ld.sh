#!/usr/bin/env bash
# The historical makerom interface passes an SGI-only -nostartfiles flag.
# Modern GNU ld does not need it, so strip just that compatibility flag.
set -euo pipefail

args=()
for arg in "$@"; do
  if [[ "$arg" != "-nostartfiles" && "$arg" != "-nodefaultlibs" && "$arg" != "-nostdinc" ]]; then
    args+=("$arg")
  fi
done

libgcc=$(mips-n64-gcc -print-libgcc-file-name)
temp_dir=$(mktemp -d)
(cd "$temp_dir" && mips-n64-ar x "$libgcc" _udivdi3.o _umoddi3.o _divdi3.o _floatundisf.o _clz.o)

for ((i = 0; i < ${#args[@]}; i++)); do
  if [[ "${args[$i]}" == "-dT" ]]; then
    source_script=${args[$((i + 1))]}
    patched_script="$temp_dir/linker-script"
    while IFS= read -r line; do
      printf '%s\n' "$line" >> "$patched_script"
      if [[ "$line" == *'codesegment.o (.text .text.*)'* ]]; then
        for object in _udivdi3.o _umoddi3.o _divdi3.o _floatundisf.o; do
          printf '      %s/%s (.text .text.*)\n' "$temp_dir" "$object" >> "$patched_script"
        done
      elif [[ "$line" == *'codesegment.o (.rodata .rodata.*)'* ]]; then
        printf '      %s/_clz.o (.rodata .rodata.*)\n' "$temp_dir" >> "$patched_script"
        printf '      %s/_floatundisf.o (.rodata .rodata.*)\n' "$temp_dir" >> "$patched_script"
      fi
    done < "$source_script"
    args[$((i + 1))]=$patched_script
    break
  fi
done

mips-n64-ld "${args[@]}"
status=$?
rm -rf "$temp_dir"
exit "$status"
