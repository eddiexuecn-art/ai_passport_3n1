// main/trio_sounds.h —— 三合一提示音（RTTTL 片段，经 rtttl_player 异步播放）。
#pragma once

/* 专注到点 → 该休息了（下行短句） */
extern const char TRIO_SOUND_REST[];
/* 休息到点 → 等待按键确认继续（上行短句） */
extern const char TRIO_SOUND_RESUME[];
/* 独立计时器到点 */
extern const char TRIO_SOUND_TIMER[];
