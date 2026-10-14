# Third-Party Notices / 第三方说明

This repository aggregates three MIT-licensed open-source projects. Their
license texts and copyright notices are preserved verbatim in each
subdirectory. This file records the upstream sources and what was changed.

本仓库聚合了三个 MIT 许可的开源项目。各目录内保留了原始许可文本与版权声明，
本文件记录上游来源与改动范围。

---

## 1. `firmware/` — Trio firmware (三合一固件)

- Upstream: [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)
  (Gitee mirror: <https://gitee.com/FoloToy/ai-passport>)
- License: MIT, Copyright (c) 2026 FoloToy — see `firmware/LICENSE`
- Changes made in this repository (branch `feature/trio` work):
  - Restart-isolated app launcher (主控页): TODO / PODCAST / AI / SETUP entries,
    per-app boot mode persisted in the `netcfg` NVS partition (`main/launcher.c`,
    `main/app_mode.*`); the AI entry reboots into the `ota_0` slot.
  - TODO application (`main/app_todo.c`, `main/task_*.c`, `main/timer_model.c`,
    `main/trio_app.c`, `main/trio_sounds.c`, `main/rtttl_player.c`): daily task
    list, 45+5 pomodoro with manual round confirmation, standalone timer,
    RTTTL alerts, daily JSONL records on the littlefs `fs` partition.
  - SoftAP provisioning (`main/trio_setup.c`): fixed per-device hotspot
    `Trio-XXXX`, built-in web page with Wi-Fi scan, optional backend address and
    6-digit pairing code, 10-minute idle timeout.
  - Server sync (`main/todo_sync.c`, `main/todo_cfg.c`, `main/wifi_link.c`,
    `main/clock_sync.c`): task template merge, incremental record upload,
    SNTP time sync.
  - Partition table v2 (`partitions.csv`): 3 MB `factory` + 3 MB `ota_0` dual
    firmware layout.

## 2. `podcast/` — Pocket Podcast (随身听)

- Upstream: [xiixiixixi/ai-passport-podcast](https://github.com/xiixiixixi/ai-passport-podcast)
  (official play page: <https://ai-passport.folotoy.cn/plays/1024/>)
- License: MIT, Copyright (c) 2026 FoloToy — see `podcast/LICENSE` and
  `podcast/firmware/LICENSE`; third-party licenses of the upstream are kept in
  `podcast/docs/licenses/` and `podcast/firmware/docs/...`
- Changes made in this repository (server only, firmware tree is upstream as
  published):
  - `server/todo_store.py` (new): JSON task table + JSONL record log storage.
  - `server/server.py`: added the `/todo` management page endpoints
    (`/api/todo/*`) reusing the upstream Bearer-token authentication.
  - `server/static/todo.html` (new): task management and record export page.
  - Runtime user data (`server/data/`, `server/media/`), build outputs and
    `__pycache__` are excluded from this repository, following the upstream
    publish rules in `podcast/docs/publish.zh_CN.md`.

## 3. `xiaozhi/` — XiaoZhi voice firmware (小智语音)

- Upstream core: [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32),
  snapshot v2.4.2 (commit `bb9122ab08c3083eeb4f67b3974b7afe771723b8`)
- License: MIT, Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.;
  Copyright (c) 2025 Project Contributors — see `xiaozhi/LICENSE` and
  `xiaozhi/NOTICE`
- Board port: "XiaoZhi 2.5 for FoloToy AI Passport" distribution, following the
  official [FoloToy AI Passport BSP](https://github.com/FoloToy/ai-passport)
  (ESP32-C3 pins, ST7789P3 panel, ES8311 audio, GPIO0 ADC buttons).
- Additional changes in this repository (`main/boards/folotoy/ai-passport/`):
  - Reads the shared `netcfg` NVS partition written by the Trio SoftAP setup,
    so no second provisioning step is needed; with no stored credentials the
    device reboots back into the Trio setup flow.
  - Double-clicking OK exits XiaoZhi and reboots into the Trio home screen.

---

ESP-IDF, LVGL and other build dependencies keep their own licenses, preserved
by the upstream projects (`podcast/docs/licenses/`, `xiaozhi/NOTICE`,
component registries). Both upstream firmware SDKs are Espressif ESP-IDF
(Apache-2.0), used per their terms.
