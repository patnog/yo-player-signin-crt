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
