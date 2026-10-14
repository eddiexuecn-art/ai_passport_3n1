// main/app_podcast.h —— PODCAST 应用：节目/剧集/播放三屏 + 后台 worker。
// 生命周期见 app_framework.h：enter 建 worker 与屏，exit 停播放、杀 worker、
// 删屏清内存；收听进度在服务端与本地 NVS，重进即恢复。
#pragma once

#include "app_framework.h"

extern const trio_app_t APP_PODCAST;
