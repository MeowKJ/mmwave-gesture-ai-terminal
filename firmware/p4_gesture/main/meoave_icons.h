#pragma once
#include "lvgl.h"

/* Lucide 0.468.0 (ISC); native-size, antialiased alpha masks generated from
 * the checked-in SVGs. FACE/FLOWER remain the product's own launcher marks. */
typedef enum { ICON_LEFT=1, ICON_RIGHT, ICON_WIFI, ICON_SUN, ICON_EYE,
               ICON_MINUS, ICON_PLUS, ICON_GEAR, ICON_FACE, ICON_FLOWER,
               ICON_SPEAKER, ICON_MIC, ICON_EYE_OFF, ICON_DELETE } icon_kind_t;
const lv_image_dsc_t *meo_icon_asset(icon_kind_t kind, int size);
