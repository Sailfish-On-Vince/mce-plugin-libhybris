/** @file sysfs-led-vince.c
 *
 * mce-plugin-libhybris - Libhybris plugin for Mode Control Entity
 * <p>
 * Copyright (C) 2017 Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author BirdZhang <0312birdzhang@gmail.com>
 *
 * mce-plugin-libhybris is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License.
 *
 * mce-plugin-libhybris is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with mce-plugin-libhybris; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* ========================================================================= *
 * RGB led control: vince backend
 *
 * Three channels, all of which:
 * - must have 'brightness' control file
 * - must have 'max_brightness' control file
 * - must have 'blink' control file
 *
 * Assumptions built into code:
 * - writing to 'blink' affects 'brightness' control too and vice versa
 * ========================================================================= */

#include "sysfs-led-vince.h"
#include "sysfs-led-util.h"
#include "sysfs-val.h"
#include "plugin-config.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>

/* ========================================================================= *
 * PROTOTYPES
 * ========================================================================= */

typedef struct
{
    const char *max_brightness;    // R
    const char *brightness;    // W
} led_paths_vince_t;

typedef struct
{
    sysfsval_t *cached_max_brightness;
    sysfsval_t *cached_brightness;
} led_channel_vince_t;

/* ------------------------------------------------------------------------- *
 * ONE_CHANNEL
 * ------------------------------------------------------------------------- */

static void led_channel_vince_init      (led_channel_vince_t *self);
static void led_channel_vince_close     (led_channel_vince_t *self);
static bool led_channel_vince_probe     (led_channel_vince_t *self, const led_paths_vince_t *path);
static void led_channel_vince_set_value (const led_channel_vince_t *self, int value);

/* ------------------------------------------------------------------------- *
 * ALL_CHANNELS
 * ------------------------------------------------------------------------- */

static void led_control_vince_map_color (int r, int g, int b, int *white);
static void led_control_vince_value_cb  (void *data, int r, int g, int b);
static void led_control_vince_close_cb  (void *data);

bool        led_control_vince_probe     (led_control_t *self);

/* ========================================================================= *
 * ONE_CHANNEL
 * ========================================================================= */

static void
led_channel_vince_init(led_channel_vince_t *self)
{
    self->cached_max_brightness = sysfsval_create();
    self->cached_brightness     = sysfsval_create();
}

static void
led_channel_vince_close(led_channel_vince_t *self)
{
    sysfsval_delete(self->cached_max_brightness),
        self->cached_max_brightness = 0;

    sysfsval_delete(self->cached_brightness),
        self->cached_brightness = 0;
}

static bool
led_channel_vince_probe(led_channel_vince_t *self,
                        const led_paths_vince_t *path)
{
    bool res = false;

    if( !sysfsval_open_rw(self->cached_brightness, path->brightness) )
        goto cleanup;

    if( !sysfsval_open_ro(self->cached_max_brightness, path->max_brightness) )
        goto cleanup;

    sysfsval_refresh(self->cached_max_brightness);

    if( sysfsval_get(self->cached_max_brightness) <= 0 )
        goto cleanup;

    res = true;

cleanup:

    /* Always close the max_brightness file */
    sysfsval_close(self->cached_max_brightness);

    /* On failure close the other files too */
    if( !res )
    {
        sysfsval_close(self->cached_brightness);
    }

    return res;
}

static void
led_channel_vince_set_value(const led_channel_vince_t *self, int value)
{
    value = led_util_scale_value(value,
                                 sysfsval_get(self->cached_max_brightness));
    sysfsval_set(self->cached_brightness, value);
}

/* ========================================================================= *
 * ALL_CHANNELS
 * ========================================================================= */

#define VINCE_CHANNELS 1

static void
led_control_vince_map_color(int r, int g, int b, int *white)
{
    /* Use maximum value from requested RGB value */
    if( r < g ) r = g;
    if( r < b ) r = b;
    *white = r;
}

static void
led_control_vince_value_cb(void *data, int r, int g, int b)
{
    const led_channel_vince_t *channel = data;

    int white = 0;
    led_control_vince_map_color(r, g, b, &white);

    led_channel_vince_set_value(channel + 0, white);
}

static void
led_control_vince_close_cb(void *data)
{
    led_channel_vince_t *channel = data;
    led_channel_vince_close(channel + 0);
}
static bool
led_control_vince_static_probe(led_channel_vince_t *channel)
{
    /** Sysfs control paths for vince leds */
    static const led_paths_vince_t paths[][VINCE_CHANNELS] =
    {
        // "vince (Xiaomi Redmi note 5/5 plus)"
        {
            {
                .max_brightness = "/sys/class/leds/green/max_brightness",
                .brightness     = "/sys/class/leds/green/brightness",
            },
        },
    };

    bool ack = false;

    for( size_t i = 0; i < G_N_ELEMENTS(paths); ++i ) {
        if( (ack = led_channel_vince_probe(channel+0, &paths[i][0])) )
            break;
    }

    return ack;
}

static bool
led_control_vince_dynamic_probe(led_channel_vince_t *channel)
{
    static const objconf_t white_conf[] =
    {
        OBJCONF_FILE(led_paths_vince_t, brightness,     Brightness),
        OBJCONF_FILE(led_paths_vince_t, max_brightness, MaxBrightness),
        OBJCONF_STOP
    };

    static const char * const pfix[VINCE_CHANNELS] =
    {
        "Led",
    };

    bool ack = false;

    led_paths_vince_t paths[VINCE_CHANNELS];

    memset(paths, 0, sizeof paths);
    for( size_t i = 0; i < VINCE_CHANNELS; ++i )
        objconf_init(white_conf, &paths[i]);

    for( size_t i = 0; i < VINCE_CHANNELS; ++i ) {
        if( !objconf_parse(white_conf, &paths[i], pfix[i]) )
            goto cleanup;

        if( !led_channel_vince_probe(channel+i, &paths[i]) )
            goto cleanup;
    }

    ack = true;

cleanup:

    for( size_t i = 0; i < VINCE_CHANNELS; ++i )
        objconf_quit(white_conf, &paths[i]);

    return ack;
}

bool
led_control_vince_probe(led_control_t *self)
{

    static led_channel_vince_t channel[VINCE_CHANNELS];

    bool res = false;

    led_channel_vince_init(channel + 0);

    self->name   = "vince";
    self->data   = channel;
    self->enable = 0;
    self->value  = led_control_vince_value_cb;
    self->close  = led_control_vince_close_cb;

    /* We can use sw breathing logic */
    self->can_breathe = true;
    self->breath_type = LED_RAMP_HARD_STEP;

    if( self->use_config )
        res = led_control_vince_dynamic_probe(channel);

    if( !res )
        res = led_control_vince_static_probe(channel);

    if( !res )
        led_control_close(self);

    return res;
}