// led.h - optional TrimUI Brick RGB LED control: a red "recording" tally light.
// Best-effort and self-contained: if the /sys/class/led_anim interface isn't
// present (e.g. another device), every call is a silent no-op.
#ifndef LED_H
#define LED_H

// Save the user's current LED look, then set a red breathing pulse on all zones.
void led_record_start(void);
// Restore exactly the look saved by led_record_start (no-op if nothing was saved).
void led_record_stop(void);

#endif
