#!/bin/sh
# Builds and runs the host tests for yo-player's sign-in code (no SDK needed).
#   tests/run.sh                  protocol + feed reader (fast), then the account state machine (~15 s)
#   tests/run.sh feed a.json ...  also parse saved TVHTML5 browse responses
# tests/live-test.c is separate: it talks to Google and needs a refresh token (see its header).
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$here/..
libs=$app/../../libs
out=${TMPDIR:-/tmp}
flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined -pthread"
includes="-I$here/host-include -I$app/include -I$libs/simple-lib-core/include"

gcc $flags $includes "$here/host-test.c" "$app/src/account-oauth.c" "$app/src/extractors/youtube-tv.c" \
    "$libs/simple-lib-core/src/json.c" -o "$out/yo-host-test"
"$out/yo-host-test" "$@"

gcc $flags $includes "$here/account-test.c" "$app/src/account.c" "$app/src/account-oauth.c" \
    "$libs/simple-lib-core/src/json.c" -o "$out/yo-account-test"
"$out/yo-account-test"

# font.c (the stb_truetype text path): compiled for real against shims, rendering with the firmware fonts.
# YO_FONT_DIR must hold a copy of /dev_flash/data/font (RPCS3's dev_flash has one); skipped without it.
fontdir=${YO_FONT_DIR:?set YO_FONT_DIR to a copy of /dev_flash/data/font}
stage=$(mktemp -d)
cp $libs/simple-lib-app/include/font.h $libs/simple-lib-app/include/colors.h $here/font-include/*.h $stage/
fflags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined"
fsrc="$here/font-test.c $libs/simple-lib-app/src/font.c -lm"
gcc $fflags -DNO_SYSDIR -DFONT_SYSTEM_DIR='"/nonexistent/"' -I$stage $fsrc -o $out/yo-font-test-nodir
FONT_SHEET=$out/yo-font-sheet-nodir.png $out/yo-font-test-nodir
if [ -d "$fontdir" ]; then
   gcc $fflags -DFONT_SYSTEM_DIR="\"$fontdir/\"" -I$stage $fsrc -o $out/yo-font-test
   FONT_SHEET=$out/yo-font-sheet.png $out/yo-font-test
else
   echo "font test skipped: no firmware fonts at $fontdir (set YO_FONT_DIR)"
fi
rm -rf $stage
