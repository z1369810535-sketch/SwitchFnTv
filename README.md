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

## 0.1.2 交互与兼容性

- 卡片上按 X 的菜单改用飞牛接口：收藏 / 取消收藏走 `item/favorite`，标记已看 / 未看走 `item/watched`；NAS 版本不支持时只在本机标记并提示。不可用的「下载」已隐藏。
- 启动时检查登录改为短超时（6 秒 × 2 次），NAS 未开机时不再卡在加载页 45 秒以上，并提示原因。服务器列表里点已保存的账号会用飞牛接口校验，登录过期时直接跳到密码页（用户名已填好）。
- 「退出登录」一定会删除本机登录凭证，然后回到服务器列表（以前接口失败时账号仍然保持登录，并且会直接退出应用）。
- 去掉了 Jellyfin 的 websocket：它在飞牛上会无限重连，令牌出现在 URL 里，退出或切换账号时可能卡住界面。
- 错误提示按类型区分：网络（超时、无法解析、无法连接、5xx）、登录失效（401/403）、服务器返回的原因、非飞牛地址。密码错误、接口拒绝不再自动重试（避免触发 NAS 登录锁定）；签名错误提示检查 Switch 系统时间。
- 媒体库：按 Y 排序（添加日期 / 名称 / 上映日期 / 评分，升序或降序），NAS 拒绝的排序列会自动退回名称排序；顶部显示库名、总数和当前排序。媒体库默认最新添加在前，文件夹默认按名称。
- 首页：每个媒体库一行「最近添加」；「继续观看」为空时隐藏；媒体库列表加载失败后按 - 可以重试（以前要重启应用）。
- 剧集：剧集页的剧集列表只加载一次，播放键立即响应，连按不会打开两个播放器，播放器的选集列表直接复用；季页面显示剧名和集数，打开时定位到该继续看的那一集，播完返回自动刷新。
- 播放：直连地址打开失败时自动换成飞牛返回的备用地址再试一次（从原位置继续）；画质菜单对直连无效，已隐藏。集名显示为 `S1E2 · 名称` / `第 2 集 · 名称`，不再出现 `S0E0 -`。
- 设置：隐藏对飞牛无效的转码、下载画质、同步设置选项。
- 没有海报时显示占位图，不再用条目 guid 当图片地址请求。
- 新增测试：`tests/fntv_compat_test.cpp`（错误、重试、排序、标记已看等 9 组）和 `tests/ui_resources_test.py`（布局、绑定、翻译）。

需要在 Switch 上确认：`item/watched`、排序列 `release_date` / `vote_average`、`user/logout` 在 0.9.7-4 上是否可用，以及备用播放地址的实际表现。前三项不可用时都会自动降级，不影响浏览和播放。
