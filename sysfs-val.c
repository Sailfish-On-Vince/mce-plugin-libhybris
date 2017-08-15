/** @file sysfs-val.c
 *
 * mce-plugin-libhybris - Libhybris plugin for Mode Control Entity
 * <p>
 * Copyright (C) 2017 Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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

#include "sysfs-val.h"

#include "plugin-logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

/* ========================================================================= *
 * TYPES
 * ========================================================================= */

struct sysfsval_t
{
    char *sv_path;
    int   sv_file;
    int   sv_curr;
};

/* ========================================================================= *
 * PROTOS
 * ========================================================================= */

static void        sysfsval_ctor      (sysfsval_t *self);
static void        sysfsval_dtor      (sysfsval_t *self);

sysfsval_t        *sysfsval_create    (void);
void               sysfsval_delete    (sysfsval_t *self);
bool               sysfsval_open      (sysfsval_t *self, const char *path);
void               sysfsval_close     (sysfsval_t *self);
const char        *sysfsval_path      (const sysfsval_t *self);
int                sysfsval_get       (const sysfsval_t *self);
bool               sysfsval_set       (sysfsval_t *self, int value);
void               sysfsval_invalidate(sysfsval_t *self);
bool               sysfsval_refresh   (sysfsval_t *self);

/* ========================================================================= *
 * CODE
 * ========================================================================= */

/** Initialize sysfsval_t object to a sane state
 *
 * @param self sysfsval_t object pointer
 */
static void
sysfsval_ctor(sysfsval_t *self)
{
    self->sv_path = 0;
    self->sv_file = -1;
    self->sv_curr = -1;
}

/** Release all dynamically allocated resources used by sysfsval_t object
 *
 * @param self sysfsval_t object pointer
 */
static void
sysfsval_dtor(sysfsval_t *self)
{
    sysfsval_close(self);
}

/** Allocate and initialize an sysfsval_t object
 *
 * @return sysfsval_t object pointer
 */
sysfsval_t *
sysfsval_create(void)
{
    sysfsval_t *self = calloc(1, sizeof *self);
    sysfsval_ctor(self);
    return self;
}

/** De-initialize and release sysfsval_t object
 *
 * @param self sysfsval_t object pointer, or NULL
 */
void
sysfsval_delete(sysfsval_t *self)
{
    if( self )  {
        sysfsval_dtor(self);
        free(self);
    }
}

/** Assign path to sysfsval_t object and attempt to open the file
 *
 * @param self sysfsval_t object pointer
 *
 * @return true if file was opened succesfully, false otherwise
 */
bool
sysfsval_open(sysfsval_t *self, const char *path)
{
    bool ack = false;

    sysfsval_close(self);

    if( !path )
        goto EXIT;

    if( (self->sv_path = strdup(path)) == 0 )
        goto EXIT;

    if( (self->sv_file = open(path, O_RDWR)) == -1 )
        goto EXIT;

    mce_log(LOG_DEBUG, "%s: opened", sysfsval_path(self));

    /* Note: Current value is not fetched by default */

    ack = true;

EXIT:
    if( !ack )
        sysfsval_close(self);

    return ack;
}

/** Close file associated with sysfsval_t object and forget file path
 *
 * @param self sysfsval_t object pointer
 */
void
sysfsval_close(sysfsval_t *self)
{
    if( self->sv_file != -1 ) {
        mce_log(LOG_DEBUG, "%s: closed", sysfsval_path(self));
        close(self->sv_file), self->sv_file = -1;
    }

    free(self->sv_path), self->sv_path = 0;
}

/** Get file path associated with sysfsval_t object
 *
 * Note: This function is meant to be used only for diagnostic
 * logging and it can be assumed to always return a valid c string.
 *
 * @param self sysfsval_t object pointer
 *
 * @return file path, or "unset"
 */
const char *
sysfsval_path(const sysfsval_t *self)
{
    return self->sv_path ?: "unset";
}

/** Get integer value associated with sysfsval_t object
 *
 * @param self sysfsval_t object pointer
 *
 * @return cached content of sysfs file as integer, or -1
 */
int
sysfsval_get(const sysfsval_t *self)
{
    return self->sv_curr;
}

/** Update sysfs content associated with sysfsval_t object
 *
 * @param self sysfsval_t object pointer
 * @param value number to write to sysfs file
 *
 * @return false if updating sysfs content failed, true otherwise
 */
bool
sysfsval_set(sysfsval_t *self, int value)
{
    bool ack = true;

    if( self->sv_curr == value )
        goto EXIT;

    mce_log(LOG_DEBUG, "%s: write: %d -> %d", sysfsval_path(self),
            self->sv_curr, value);

    self->sv_curr = value;

    char data[256];

    int todo = snprintf(data, sizeof data, "%d\n", value);
    int done = write(self->sv_file, data, todo);

    if( done == todo )
        goto EXIT;

    ack = false;

    if( done == -1 )
        mce_log(LOG_ERR, "%s: write: %m", sysfsval_path(self));
    else
        mce_log(LOG_ERR, "%s: write: partial", sysfsval_path(self));

EXIT:
    return ack;
}

/** Update cached value associated with sysfsval_t object
 *
 * Meant to be used in cases where it is known that writing to
 * one sysfs file changes also the value of some other sysfs
 * control file and we want to be able to avoid unnecessary
 * write when the next sysfsval_set() call is made.
 *
 * @param self sysfsval_t object pointer
 * @param value number to write to sysfs file
 */
void
sysfsval_assume(sysfsval_t *self, int value)
{
    if( self->sv_curr != value ) {
        mce_log(LOG_DEBUG, "%s: assume: %d -> %d", sysfsval_path(self),
                self->sv_curr, value);
        self->sv_curr = value;
    }
}

/** Invalidate cached value associated with sysfsval_t object
 *
 * Meant to be used in cases where it is known that the
 * sysfs file needs to be written the next time sysfsval_set()
 * gets called.
 *
 * @param self sysfsval_t object pointer
 * @param value number to write to sysfs file
 */
void
sysfsval_invalidate(sysfsval_t *self)
{
    if( self->sv_curr != -1 ) {
        self->sv_curr = -1;
        mce_log(LOG_DEBUG, "%s: invalidated", sysfsval_path(self));
    }
}

/** Read value from sysfs file associated with sysfsval_t object
 *
 * Meant to be used for obtainining initial value / in cases
 * it is known that the cached value is most likely not reflecting
 * the actual data available via sysfs.
 *
 * @param self sysfsval_t object pointer
 *
 * @return true if reading succeeded, false otherwise
 */
bool
sysfsval_refresh(sysfsval_t *self)
{
    bool ack = false;
    int value = -1;

    char data[256];

    if( self->sv_file == -1 )
        goto EXIT;

    if( lseek(self->sv_file, 0, SEEK_SET) == -1 ) {
        mce_log(LOG_ERR, "%s: seek: %m", sysfsval_path(self));
        goto EXIT;
    }

    int done = read(self->sv_file, data, sizeof data - 1);

    if( done == -1 ) {
        mce_log(LOG_ERR, "%s: read: %m", sysfsval_path(self));
        goto EXIT;
    }

    if( done == 0 ) {
        mce_log(LOG_ERR, "%s: read: EOF", sysfsval_path(self));
        goto EXIT;
    }

    data[done] = 0;
    value = strtol(data, 0, 0);

    mce_log(LOG_DEBUG, "%s: read: %d -> %d", sysfsval_path(self),
            self->sv_curr, value);
    self->sv_curr = value;

    ack = true;

EXIT:

    if( !ack )
        sysfsval_invalidate(self);

    return ack;
}
