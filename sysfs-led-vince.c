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
    const char *max_brightness;
    const char *brightness;
    const char *blink;
} led_paths_vince_t;

typedef struct
{
    sysfsval_t *cached_max_brightness;
    sysfsval_t *cached_brightness;
    sysfsval_t *cached_blink;

    int         control_value;
    bool        control_blink;

} led_channel_vince_t;

/* ------------------------------------------------------------------------- *
 * ONE_CHANNEL
 * ------------------------------------------------------------------------- */

static void        led_channel_vince_init        (led_channel_vince_t *self);
static void        led_channel_vince_close       (led_channel_vince_t *self);
static bool        led_channel_vince_probe       (led_channel_vince_t *self, const led_paths_vince_t *path);
static void        led_channel_vince_set_value   (led_channel_vince_t *self, int value);
static void        led_channel_vince_set_blink   (led_channel_vince_t *self, int on_ms, int off_ms);


/* ------------------------------------------------------------------------- *
 * ALL_CHANNELS
 * ------------------------------------------------------------------------- */

static void        led_control_vince_map_color   (int r, int g, int b, int *red, int *green);
static void        led_control_vince_blink_cb    (void *data, int on_ms, int off_ms);
static void        led_control_vince_value_cb    (void *data, int r, int g, int b);
static void        led_control_vince_close_cb    (void *data);

bool               led_control_vince_probe       (led_control_t *self);

/* ========================================================================= *
 * ONE_CHANNEL
 * ========================================================================= */

static void
led_channel_vince_init(led_channel_vince_t *self)
{
    self->cached_max_brightness = sysfsval_create();
    self->cached_brightness     = sysfsval_create();
    self->cached_blink          = sysfsval_create();

    self->control_blink = false;
    self->control_value = 0;
}

static void
led_channel_vince_close(led_channel_vince_t *self)
{
    sysfsval_delete(self->cached_max_brightness),
        self->cached_max_brightness = 0;

    sysfsval_delete(self->cached_brightness),
        self->cached_brightness = 0;

    sysfsval_delete(self->cached_blink),
        self->cached_blink = 0;
}

static bool
led_channel_vince_probe(led_channel_vince_t *self,
                           const led_paths_vince_t *path)
{
    bool ack = false;
     
    if( !sysfsval_open_rw(self->cached_blink, path->blink) )
        goto cleanup;

    if( !sysfsval_open_rw(self->cached_brightness, path->brightness) )
        goto cleanup;
#if 0 // TODO: make a QUIRK out of this
    sysfsval_set(self->cached_max_brightness, 255);
#endif
    sysfsval_refresh(self->cached_max_brightness);

    if( sysfsval_get(self->cached_max_brightness) <= 0 )
        goto cleanup;

    if( !sysfsval_open_rw(self->cached_brightness, path->brightness) )
        goto cleanup;

    ack = true;

cleanup:

    /* Always close the max_brightness file */
    sysfsval_close(self->cached_max_brightness);

    /* On failure close the other files too */
    if( !ack )
    {
        sysfsval_close(self->cached_brightness);
        sysfsval_close(self->cached_blink);
    }

    return ack;
}

static void
led_channel_vince_set_value(led_channel_vince_t *self, int value)
{
    value = led_util_scale_value(value,
                                 sysfsval_get(self->cached_max_brightness));
    /* Ignore blinking requests while brightness is zero. */
    // if( value <= 0 )
        // self->control_blink = false;
    if( self->control_blink ) {
        sysfsval_set(self->cached_brightness, 0);
        sysfsval_set(self->cached_blink, 1);
    }
    else {
        sysfsval_set(self->cached_blink, 0);
        sysfsval_set(self->cached_brightness, value);
    }

    return;
}

static void
led_channel_vince_set_blink(led_channel_vince_t *self,
                            int on_ms, int off_ms)
{
    /* The state machine at upper level adjusts blink setting first
     * followed by brighthess setting - in vince modifying one will
     * affect the other too and must thus be handled at the same
     * time -> just cache the requested state. */
    self->control_blink = (on_ms && off_ms);
}

/* ========================================================================= *
 * ALL_CHANNELS
 * ========================================================================= */

#define vince_CHANNELS 2

static void
led_control_vince_map_color(int r, int g, int b, int *red, int *green)
{
    /* If the pattern defines red and/or green intensities, those should
     * be used. Otherwise make sure that requesting for blue only colour
     * does not result in the led being turned off. */
    if( r || g )
    {
        *red   = r;
        *green = g;
    }
    else
    {
        *red   = b;
        *green = b;
    }
}

static void
led_control_vince_blink_cb(void *data, int on_ms, int off_ms)
{
    led_channel_vince_t *channel = data;
    led_channel_vince_set_blink(channel + 0, on_ms, off_ms);
    led_channel_vince_set_blink(channel + 1, on_ms, off_ms);
}

static void
led_control_vince_value_cb(void *data, int r, int g, int b)
{
    led_channel_vince_t *channel = data;
    int red   = 0;
    int green = 0;
    led_control_vince_map_color(r, g, b, &red, &green);
    led_channel_vince_set_value(channel + 0, r);
    led_channel_vince_set_value(channel + 1, g);
    
}

static void
led_control_vince_close_cb(void *data)
{
    led_channel_vince_t *channel = data;
    led_channel_vince_close(channel + 0);
    led_channel_vince_close(channel + 1);
}

static bool
led_control_vince_static_probe(led_channel_vince_t *channel)
{
    /** Sysfs control paths for RGB leds */
    static const led_paths_vince_t vince_paths[][vince_CHANNELS] =
    {
        // vince (Xiaomi Redmi note 5/5 plus)
        {
            {
                .max_brightness = "/sys/class/leds/red/max_brightness",
                .brightness     = "/sys/class/leds/red/brightness",
                .blink          = "/sys/class/leds/red/blink",
            },
            {
                .max_brightness = "/sys/class/leds/green/max_brightness",
                .brightness     = "/sys/class/leds/green/brightness",
                .blink          = "/sys/class/leds/green/blink",
            },
            // {
            //     .max_brightness = "/sys/class/leds/blue/max_brightness",
            //     .brightness     = "/sys/class/leds/blue/brightness",
            //     .blink          = "/sys/class/leds/blue/blink",
            // },
        },
    };

    bool ack = false;

    for( size_t i = 0; i < G_N_ELEMENTS(vince_paths); ++i ) {
        if( led_channel_vince_probe(&channel[0], &vince_paths[i][0]) &&
            led_channel_vince_probe(&channel[1], &vince_paths[i][1]) ) {
            ack = true;
            break;
        }
    }

    return ack;
}

static bool
led_control_vince_dynamic_probe(led_channel_vince_t *channel)
{
  /* See inifiles/60-vince.ini for example */
  static const objconf_t vince_conf[] =
  {
    OBJCONF_FILE(led_paths_vince_t, brightness,      Brightness),
    OBJCONF_FILE(led_paths_vince_t, max_brightness,  MaxBrightness),
    OBJCONF_FILE(led_paths_vince_t, blink,           Blink),
    OBJCONF_STOP
  };

  static const char * const pfix[vince_CHANNELS] =
  {
    "Red", "Green"
  };

  bool ack = false;

  led_paths_vince_t paths[vince_CHANNELS];

  memset(paths, 0, sizeof paths);
  for( size_t i = 0; i < vince_CHANNELS; ++i )
    objconf_init(vince_conf, &paths[i]);

  for( size_t i = 0; i < vince_CHANNELS; ++i )
  {
    if( !objconf_parse(vince_conf, &paths[i], pfix[i]) )
      goto cleanup;

    if( !led_channel_vince_probe(channel+i, &paths[i]) )
      goto cleanup;
  }

  ack = true;

cleanup:

  for( size_t i = 0; i < vince_CHANNELS; ++i )
    objconf_quit(vince_conf, &paths[i]);

  return ack;
}

bool
led_control_vince_probe(led_control_t *self)
{
    static led_channel_vince_t channel[vince_CHANNELS];

    bool ack = false;

    led_channel_vince_init(channel+0);
    led_channel_vince_init(channel+1);
    // led_channel_vince_init(channel+2);

    self->name   = "vince";
    self->data   = channel;
    self->enable = 0;
    self->blink  = led_control_vince_blink_cb;
    self->value  = led_control_vince_value_cb;
    self->close  = led_control_vince_close_cb;

    /* Prefer to use the built-in soft-blinking */
    self->can_breathe = false;

    if( self->use_config )
        ack = led_control_vince_dynamic_probe(channel);

    if( !ack )
        ack = led_control_vince_static_probe(channel);

    if( !ack )
        led_control_close(self);

    return ack;
}
