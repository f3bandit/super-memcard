#!/bin/sh
# Fetch the sd2psXtd firmware the virtual card runs, at the commit the docs were written against.
# Its files are used as they are: nothing in firmware/ is changed.
set -e
cd "$(dirname "$0")"
REV=${SD2PSXTD_REV:-8a20bd991b43ac731882f5f85030b19e94c23a3b}
if [ ! -d firmware/.git ]; then
    git clone --quiet https://github.com/sd2psXtd/firmware.git firmware
fi
git -C firmware fetch --quiet origin "$REV" 2>/dev/null || true
git -C firmware checkout --quiet "$REV"
echo "sd2psXtd firmware at $(git -C firmware rev-parse --short HEAD)"
