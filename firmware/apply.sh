#!/bin/sh
# Add Super MemCard to an sd2psXtd checkout: copies firmware/ps1/* (and Monocypher, third_party/) into its src/ps1/ and applies
# patches/*.patch (an include and a case line in the PS1 dispatcher, a call in the main loop, the
# source file and a WITH_SUPERMEMCARD option in CMake).
#   firmware/apply.sh <sd2psXtd folder>
# Uses patch(1), not git apply: git apply silently skips files when run inside another repo.
set -e
here=$(cd "$(dirname "$0")" && pwd)
fw=${1:?usage: apply.sh <sd2psXtd folder>}
cp "$here"/ps1/* "$fw/src/ps1/"
cp "$here"/../third_party/monocypher/monocypher.c "$here"/../third_party/monocypher/monocypher.h "$fw/src/ps1/"   # the card identity
for p in "$here"/patches/*.patch; do patch -s -p1 -N -d "$fw" < "$p"; done
grep -q "ps1_supermemcard_bus.inc" "$fw/src/ps1/ps1_memory_card.c" && grep -q "ps1_supermemcard_task" "$fw/src/ps1.c" || {
    echo "apply.sh: the patch didn't apply to $fw" >&2; exit 1; }
echo "Super MemCard added to $fw"
