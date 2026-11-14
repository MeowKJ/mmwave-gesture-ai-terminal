#pragma once

#include <stdbool.h>

/* Register the CDC binary receive lane.  The caller must invoke this after
   bsp_usb_init() and before accepting host audio sessions. */
void meo_voice_link_init(void);

/* Handle the short ASCII control command that switches CDC into framed binary
   mode.  Returns true when the VOICE command belongs to this module. */
bool meo_voice_link_command(const char *line);
