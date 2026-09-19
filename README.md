# SwitchFnTv

Nintendo Switch OLED Atmosphere `.nro` client for 飞牛影视. First release: LAN username/password login, movie/series browsing, MPV playback, and already-linked external subtitles.

This is not a drop-in Jellyfin URL swap. The UI comes from Switchfin 0.9.4; the data layer talks to Feiniu APIs. Capabilities are **not** marked verified until they are tested on the user's Switch.

## Default server

`http://192.168.31.137:5666`

API prefix is `/v/api/v1`. Do not produce `/v/v/api/...`.

Passwords are not saved. Session tokens are stored locally (SD card on Switch) and are not claimed secure.

## Windows checks

Anonymous connectivity (no credentials):

```powershell
powershell.exe -NoProfile -File .\scripts\Test-FnTv.ps1
```

This writes `FnTv-check.json` with HTTP status, result class, config field names, and whether `nas_oauth` exists. It does not print bodies, cookies, tokens, or OAuth values.

Interactive API smoke (credentials stay in memory):

```powershell
python .\scripts\fn_api_smoke.py
```

## Build

This machine does not ship a Switch toolchain. Build the `.nro` with GitHub Actions:

1. Create a GitHub repository and push this tree with submodules.
2. Enable Actions.
3. Run the `build` workflow (`workflow_dispatch`).
4. Download `SwitchFnTv.nro`.

Do not invent a download link if Actions has not produced an artifact.

## Install

1. Confirm Atmosphere / HB Menu can launch the official [Switchfin 0.9.4 `.nro`](https://github.com/dragonflylee/switchfin/releases/tag/0.9.4).
2. Copy `SwitchFnTv.nro` to `/switch/SwitchFnTv/`.
3. Chinese UI/subtitle fonts: the nro keeps `chinese_fallback.ttf`; Switch system fonts are also used by MPV.

## First-release scope

Included: server address, username/password software keyboard, movie/series browse, episode pick, play/pause/seek, audio tracks, embedded subs, Feiniu external SRT/ASS/SSA, readable errors.

Not included: NAS OAuth/QR, live TV, danmaku, online subtitle search, full-file download, NAS management.

## 0.1.1 compatibility and resume

- Reads both numeric and string playback positions from Feiniu `play/info`, and accepts nullable metadata, boolean flags, decimal ratings, backdrops and cast profile images used by 0.9.7-4.
- Saves progress locally every 10 seconds and on pause, episode changes, normal exit and completion. Records are isolated by server and user under the existing configuration directory's `playback/` folder, with a backup file for interrupted writes. Stream URLs and credentials are excluded.
- Sends progress to the native `play/record` endpoint. Failed uploads keep the local record available for the next playback; reconnecting and playing retries synchronization. Abrupt power loss may lose the last checkpoint interval.
- The home page shows continue-watching entries. A show's play button resumes the last episode or advances after completion. Movie details offer separate continue and restart actions. Press Y to refresh details; select the synopsis to open it.
- Favorites now use Feiniu's PUT/DELETE API. Unsupported download actions remain hidden.

Regression checks compile the production API/history implementation with a fake HTTP transport; GitHub Actions also compiles the complete Switch app. Real NAS login, playback seeking, controller layout and progress synchronization still require verification on the user's Switch; no authenticated NAS responses are part of the tests.

Playback target: handheld 720p, dock max 1080p, SDR output. HDR sources use tone mapping. Direct play uses `/v/api/v1/media/range/{mediaGuid}` with the real URL/headers; existing Feiniu HLS/transcode is fallback only. No extra NAS containers.
