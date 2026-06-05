// led.c - optional TrimUI Brick RGB LED control: a red breathing "recording"
// tally light that SAVES the user's current LED look and RESTORES it afterwards.
//
// The Brick exposes an in-kernel LED animation engine at /sys/class/led_anim/.
// You set, per zone, a target colour ("RRGGBB " with a trailing space), a cycle
// duration (ms), a loop count (-1 = endless) and an effect type; writing the
// effect_<zone> node is the TRIGGER that (re)starts the animation. A kernel thread
// ("ledc daemon") then drives the hardware from those nodes — nothing in userspace
// polls or overwrites them, so our writes simply stick until we change them back.
// (Probed on-device: effects 0 disable / 1 linear / 2 breath / 3 sniff / 4 static /
//  5..7 blink; zones lr [both sticks] / m [middle] / f1 / f2.)
//
// Everything here is best-effort: as root the rw-rw-r-- nodes are writable; if the
// interface is missing or a write fails we just skip it (no LED, no harm).
#include "led.h"
#include <stdio.h>
#include <string.h>

#define LED_BASE "/sys/class/led_anim"

// Zones we animate — the same set the user's NextUI LED setting drives on the Brick.
static const char *const LED_ZONES[] = { "lr", "m", "f1", "f2" };
#define LED_NZONES ((int)(sizeof(LED_ZONES) / sizeof(LED_ZONES[0])))

// Per-zone snapshot for restore (values read back verbatim, written back verbatim).
static struct { char rgb[40], eff[16], cyc[16], dur[16]; } g_saved[LED_NZONES];
static int g_have_saved = 0;

static int led_present(void) {
    FILE *f = fopen(LED_BASE "/effect_names", "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void led_write(const char *node, const char *val) {
    if (!val || !val[0]) return;
    char p[96];
    snprintf(p, sizeof(p), LED_BASE "/%s", node);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fputs(val, f);
    fclose(f);
}

// Read a node into out[], stripping only the trailing newline/CR (so the format's
// significant trailing space on the colour nodes is preserved for a verbatim write).
static void led_read(const char *node, char *out, int cap) {
    out[0] = '\0';
    char p[96];
    snprintf(p, sizeof(p), LED_BASE "/%s", node);
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(out, cap, f)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    }
    fclose(f);
}

static void zone_node(char *buf, int cap, const char *prefix, const char *zone) {
    snprintf(buf, cap, "%s%s", prefix, zone);
}

void led_record_start(void) {
    if (!led_present()) return;

    // Snapshot the current per-zone look so we can put it back on stop.
    for (int i = 0; i < LED_NZONES; i++) {
        char node[40];
        zone_node(node, sizeof(node), "effect_rgb_hex_", LED_ZONES[i]); led_read(node, g_saved[i].rgb, sizeof(g_saved[i].rgb));
        zone_node(node, sizeof(node), "effect_cycles_",  LED_ZONES[i]); led_read(node, g_saved[i].cyc, sizeof(g_saved[i].cyc));
        zone_node(node, sizeof(node), "effect_duration_", LED_ZONES[i]); led_read(node, g_saved[i].dur, sizeof(g_saved[i].dur));
        zone_node(node, sizeof(node), "effect_",         LED_ZONES[i]); led_read(node, g_saved[i].eff, sizeof(g_saved[i].eff));
    }
    g_have_saved = 1;

    // Red breathing pulse on every zone (params first, effect-trigger last).
    for (int i = 0; i < LED_NZONES; i++) {
        char node[40];
        zone_node(node, sizeof(node), "effect_rgb_hex_", LED_ZONES[i]); led_write(node, "FF0000 ");
        zone_node(node, sizeof(node), "effect_duration_", LED_ZONES[i]); led_write(node, "1200");
        zone_node(node, sizeof(node), "effect_cycles_",  LED_ZONES[i]); led_write(node, "-1");
        zone_node(node, sizeof(node), "effect_",         LED_ZONES[i]); led_write(node, "2");  // 2 = breath
    }
}

void led_record_stop(void) {
    if (!g_have_saved) return;
    for (int i = 0; i < LED_NZONES; i++) {
        char node[40];
        zone_node(node, sizeof(node), "effect_rgb_hex_", LED_ZONES[i]); led_write(node, g_saved[i].rgb);
        zone_node(node, sizeof(node), "effect_duration_", LED_ZONES[i]); led_write(node, g_saved[i].dur);
        zone_node(node, sizeof(node), "effect_cycles_",  LED_ZONES[i]); led_write(node, g_saved[i].cyc);
        zone_node(node, sizeof(node), "effect_",         LED_ZONES[i]); led_write(node, g_saved[i].eff);
    }
    g_have_saved = 0;
}
