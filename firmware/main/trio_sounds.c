#include "trio_sounds.h"

/* RTTTL 格式：name:d=o,o=o,b=bpm,notes（与 funbox demo 的 rtttl_player 兼容）。
 * 每段为乐句重复 3 遍（4p 为四分休止），总长约 4-5 秒，确保提醒能被注意到。 */

/* 下行三音：提醒结束专注、进入休息 */
const char TRIO_SOUND_REST[] =
    "Rest:d=4,o=6,b=180:8g,8e,8c,4p,8g,8e,8c,4p,8g,8e,8c";

/* 上行三音：休息结束，按键确认继续 */
const char TRIO_SOUND_RESUME[] =
    "Go:d=4,o=6,b=180:8c,8e,8g,4p,8c,8e,8g,4p,8c,8e,8g";

/* 双音上扬：独立计时器到点 */
const char TRIO_SOUND_TIMER[] =
    "Ding:d=4,o=6,b=200:c,g,c7,4p,c,g,c7,4p,c,g,c7";
