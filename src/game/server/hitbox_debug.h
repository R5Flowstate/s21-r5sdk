#ifndef HITBOX_DEBUG_H
#define HITBOX_DEBUG_H

// Emits player collision-hull outlines through the debug overlay interface once
// per server frame. The adders are S2C-hooked, so the client renders them.
void HitboxDebug_DrawFrame(void);

#endif // HITBOX_DEBUG_H
