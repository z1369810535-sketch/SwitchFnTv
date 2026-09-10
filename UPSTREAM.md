# Upstream

SwitchFnTv vendors [Switchfin 0.9.4](https://github.com/dragonflylee/switchfin) (`b032c04d62256576d04762ef54bb22600838d775`) under the Apache License 2.0. The original `LICENSE` file is retained.

Feiniu request signing follows [fntv-electron](https://github.com/QiaoKes/fntv-electron) commit `bb30d12820b1d2500d39e9feeb6548432e485783`. Protocol constants in that client are public client constants, not user passwords.

This tree replaces the Jellyfin data layer with a Feiniu API module. Music, live TV, danmaku, downloads, and NAS management are out of the first release.

Nintendo Switch builds use the upstream `devkitpro/devkita64:20260219` image and Switchfin `switch-portlibs` packages.
