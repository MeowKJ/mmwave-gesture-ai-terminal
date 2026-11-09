#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 24
#define LV_DEF_REFR_PERIOD 15 /* match firmware sdkconfig.defaults */
#define LV_USE_OS LV_OS_NONE
#define LV_MEM_SIZE (8 * 1024 * 1024)
#define LV_USE_FLOAT 0 /* match product point/draw descriptor layout */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_FONT_MONTSERRAT_26 1
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_SYSMON 0
#endif
