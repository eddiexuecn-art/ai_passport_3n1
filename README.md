# 社区3合1：任务番茄钟·播客·小智

把 AI Passport 变成随身的任务与陪伴终端。开机进入主控页，在 TODO、PODCAST、AI 三个应用间自由切换，双击确定键即退回主控页。

**English** | [简体中文（下方）](#简体中文)

## English

Turn your AI Passport into a pocket companion for focus, listening and chatting. Boot to a tidy home screen and switch between TODO, Podcast and AI voice chat; double-click OK to jump back anytime.

- **TODO** — Daily tasks and a pomodoro: 45 minutes of focus followed by a 5-minute break, and the next round waits for your confirmation. Long-press to suspend, drop or finish a task; progress and records save automatically and sync to the companion web dashboard when online. Long-press the task list to open a standalone timer with 5–60 minute presets.
- **PODCAST** — Browse shows and episodes and listen right away, with volume and seek at your fingertips. Episodes are prepared by your self-hosted web backend.
- **AI** — One click into the XiaoZhi voice assistant: just talk — ask questions, chat, or hear a story.

### Repository layout

```
firmware/   Trio firmware: TODO + PODCAST + AI entry + SoftAP provisioning + server task sync
podcast/    Upstream "Pocket Podcast" project (web backend in server/, standalone firmware in firmware/)
xiaozhi/    XiaoZhi voice firmware (FoloToy AI Passport board port, flashed to the ota_0 slot)
```

### Quick start

**Firmware** (ESP-IDF v5.5.3, ESP32-C3 / 8 MB flash):

```bash
cd firmware
idf.py set-target esp32c3
idf.py build
python -m esptool --chip esp32c3 merge_bin -o trio-full.bin --flash_mode dio --flash_size 8MB --flash_freq 80m \
    0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x20000 build/FoloToy-AI-Passport.bin
# flash trio-full.bin (or the full dual image below) at offset 0x0
```

The shipped partition table keeps a 3 MB `factory` slot for this firmware and a 3 MB `ota_0` slot for the XiaoZhi image; the home-screen AI entry reboots into that slot. Build XiaoZhi with ESP-IDF v6.1 (`python scripts/build.py --list-boards` in `xiaozhi/`) and merge it at `0x330000` for a single full image.

**Backend** (Python 3, powers the podcast library and the `/todo` task manager):

```bash
cd podcast/server
pip install -r requirements.txt
python server.py            # listens on :8899, admin web UI + /todo page
```

On first launch, open the setup page to create the admin password and device pairing.

### Usage

First boot goes straight into setup: on your phone, join the device hotspot `Trio-XXXX` (fixed per device; password shown on the device screen), open `http://192.168.4.1`, pick your 2.4 GHz Wi-Fi, optionally fill in the backend address and 6-digit pairing code, then tap **Connect & Save**. Reconfigure anytime from `SETUP` on the home screen.

- **Home screen** — Up/Down to choose an app, OK to open; double-click OK inside any app to return.
- **TODO** — Up/Down to pick a task, OK to start the pomodoro; OK pauses/resumes, long-press OK chooses suspend / drop / finish. Long-press OK on the task list opens the standalone timer (Up/Down sets 5–60 minutes, OK starts). Works offline; tasks and records sync when online.
- **PODCAST** — Up/Down to browse shows and episodes, OK to play/pause, Up/Down for volume, long-press Up/Down to skip ±15 s, long-press OK to go back. Requires your self-hosted backend.
- **AI** — enter and just talk. If Wi-Fi is not configured, it returns to the setup flow automatically.

### License & credits

Everything here is MIT-licensed open source; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the full upstream list.

- [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) — official AI Passport BSP & demo firmware (MIT © 2026 FoloToy)
- [xiixiixixi/ai-passport-podcast](https://github.com/xiixiixixi/ai-passport-podcast) · [official play page](https://ai-passport.folotoy.cn/plays/1024/) — the Pocket Podcast play this podcast/ tree derives from (MIT © 2026 FoloToy)
- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — XiaoZhi AI voice core (MIT © 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and contributors)

---

## 简体中文

把 AI Passport 变成随身的任务与陪伴终端。开机进入主控页，在 ToDo（含番茄钟）、PODCAST（播客）、AI（小智） 三个应用间自由切换，双击确定键即退回主控页。ToDo 内置每日任务与番茄钟：选中任务一键开始，45 分钟专注加 5 分钟休息循环推进，休息结束由你确认再开下一轮；长按呼出菜单可暂停挂起、放弃本轮或结束任务，完成进度和记录自动保存，联网后同步到服务端网页管理。任务列表长按还能进入独立计时器，5 到 60 分钟自由调档。PODCAST（社区玩法：天才 随身听） 浏览节目单直接收听，上下键调音量、长按快进快退。AI（社区玩法：FOLOTOY 小智） 模式一键进入语音对话，随时提问聊天。把专注、收听和陪伴装进口袋，一天的任务看得见、走得完。

### 仓库结构

```
firmware/   三合一固件：TODO + PODCAST + AI 入口 + SoftAP 配网 + 服务端任务同步
podcast/    「随身听」上游项目（server/ 网页后台 + firmware/ 单应用固件）
xiaozhi/    小智语音固件（FoloToy AI Passport 板级适配，烧入 ota_0 槽）
```

### 快速开始

**固件**（ESP-IDF v5.5.3，ESP32-C3 / 8MB Flash）：见上方 English 的命令；分区表含 3MB `factory`（本固件）与 3MB `ota_0`（小智）双应用槽，主控页选 AI 即重启进小智。小智用 ESP-IDF v6.1 构建（`xiaozhi/` 内 `python scripts/build.py`），合并镜像时写在 `0x330000`。

**后台**（Python 3，提供播客节目库与 `/todo` 任务管理页）：

```bash
cd podcast/server
pip install -r requirements.txt
python server.py            # 监听 8899 端口，管理网页 + /todo 页
```

首次启动打开设置页创建管理口令和设备配对。

### 使用方法

首次开机自动进入配网页：手机连接设备热点 `Trio-XXXX`（按设备固定，密码见设备屏幕），浏览器打开 `http://192.168.4.1`，点「扫描附近网络」选择家中 2.4GHz Wi-Fi 并输入密码，可选填服务端地址和六位配对码，点「连接并保存」，看到“保存成功”后设备自动联网进入主控页；之后可从主控页 SETUP 随时重新配网。

- **主控页**：上下键选择应用，OK 进入；任意应用内双击 OK 退出回主控页。
- **TODO**：上下键选任务，OK 开始番茄钟——45 分钟专注加 5 分钟休息循环，OK 暂停/继续，长按 OK 选择挂起、放弃或结束；任务列表页长按 OK 进入独立计时器，上下键调 5–60 分钟，OK 开始。离线也能用，联网后任务与记录自动同步服务端。
- **PODCAST**：上下键选节目和单集，OK 播放/暂停，上下键调音量，长按上下键快退/快进 15 秒，长按 OK 返回列表；需配好自建服务端才有节目。
- **AI**：进入后直接语音对话；若未配网，会自动退回配网流程。

### 许可与致谢

本项目全部基于 MIT 开源软件，上游清单见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)：

- [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) — AI Passport 官方 BSP 与示例固件（MIT © 2026 FoloToy）
- [xiixiixixi/ai-passport-podcast](https://github.com/xiixiixixi/ai-passport-podcast) · [官方玩法页](https://ai-passport.folotoy.cn/plays/1024/) — podcast/ 目录的上游（MIT © 2026 FoloToy）
- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — 小智 AI 语音核心（MIT © 2025 深圳心智未来科技有限公司及贡献者）
