# Yo! Player (PSL1GHT) — sign-in fork

Fork of [openlewa/yo-player-psl1ght](https://github.com/openlewa/yo-player-psl1ght), which ports
[mohasi/ps3-dev](https://github.com/mohasi/ps3-dev) Yo! Player to the open-source PSL1GHT SDK.
No Sony SDK needed.

## What's new

- **YouTube sign-in** from the console: press **Select**, enter the code at youtube.com/activate. No keyboard needed.
- **Your own feeds**: Recommended, Subscriptions, History and Watch Later, with ad tiles filtered out.
- **Playback fixed**: audio no longer cuts and video no longer freezes after ~60 s (the old stream client was capped by Google).
- **Boots reliably**: the firmware font library that kept the app from ever reaching main() is replaced by a built-in renderer. Latin text works without /dev_flash; CJK uses firmware fonts when present.
- **Safer sign-in**: playback stays logged-out; only browsing uses your account. Tokens refresh automatically.
- Smaller fixes: thumbnail/image loading, video grid, search and TLS robustness. Host tests cover sign-in and fonts (ASAN/UBSAN).

## Install

Copy  to  on a build of the app, or build it yourself
(see ). Installs beside the original Yo! Player.

## Known limits

- Tested on RPCS3 and a real console; Korean shows boxes (no Hangul firmware font).
- Sign-in uses the YouTube TV device flow. Use at your own risk; Google may change it.
