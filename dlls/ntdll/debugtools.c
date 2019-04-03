/*
 * Debugging functions
 *
 * Copyright 2000 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <ctype.h>

#include "wine/debug.h"
#include "wine/exception.h"
#include "wine/library.h"
#include "wine/unicode.h"
#include "winnt.h"
#include "winternl.h"
#include "ntdll_misc.h"

WINE_DECLARE_DEBUG_CHANNEL(pid);
WINE_DECLARE_DEBUG_CHANNEL(timestamp);

static BOOL init_done;
static struct debug_info initial_info;  /* debug info for initial thread */
static unsigned char default_flags = (1 << __WINE_DBCL_ERR) | (1 << __WINE_DBCL_FIXME);
static int nb_debug_options = -1;
static int options_size;
static struct __wine_debug_channel *debug_options;

static const char * const debug_classes[] = { "fixme", "err", "warn", "trace" };

/* get the debug info pointer for the current thread */
static inline struct debug_info *get_info(void)
{
    if (!init_done)
    {
        if (!initial_info.str_pos) initial_info.str_pos = initial_info.strings;
        if (!initial_info.out_pos) initial_info.out_pos = initial_info.output;
        return &initial_info;
    }
    return ntdll_get_thread_data()->debug_info;
}

/* allocate some tmp space for a string */
static char *get_temp_buffer( size_t n )
{
    struct debug_info *info = get_info();
    char *res = info->str_pos;

    if (res + n >= &info->strings[sizeof(info->strings)]) res = info->strings;
    info->str_pos = res + n;
    return res;
}

/* release extra space that we requested in get_temp_buffer() */
static void release_temp_buffer( char *ptr, size_t size )
{
    struct debug_info *info = get_info();
    info->str_pos = ptr + size;
}

/* add a string to the output buffer */
static int append_output( struct debug_info *info, const char *str, size_t len )
{
    if (len >= sizeof(info->output) - (info->out_pos - info->output))
    {
       fprintf( stderr, "wine_dbg_output: debugstr buffer overflow (contents: '%s')\n", info->output );
       info->out_pos = info->output;
       abort();
    }
    memcpy( info->out_pos, str, len );
    info->out_pos += len;
    return len;
}

/* add a new debug option at the end of the option list */
static void add_option( const char *name, unsigned char set, unsigned char clear )
{
    int min = 0, max = nb_debug_options - 1, pos, res;

    if (!name[0])  /* "all" option */
    {
        default_flags = (default_flags & ~clear) | set;
        return;
    }
    if (strlen(name) >= sizeof(debug_options[0].name)) return;

    while (min <= max)
    {
        pos = (min + max) / 2;
        res = strcmp( name, debug_options[pos].name );
        if (!res)
        {
            debug_options[pos].flags = (debug_options[pos].flags & ~clear) | set;
            return;
        }
        if (res < 0) max = pos - 1;
        else min = pos + 1;
    }
    if (nb_debug_options >= options_size)
    {
        options_size = max( options_size * 2, 16 );
        debug_options = realloc( debug_options, options_size * sizeof(debug_options[0]) );
    }

    pos = min;
    if (pos < nb_debug_options) memmove( &debug_options[pos + 1], &debug_options[pos],
                                         (nb_debug_options - pos) * sizeof(debug_options[0]) );
    strcpy( debug_options[pos].name, name );
    debug_options[pos].flags = (default_flags & ~clear) | set;
    nb_debug_options++;
}

/* parse a set of debugging option specifications and add them to the option list */
static void parse_options( const char *str )
{
    char *opt, *next, *options;
    unsigned int i;

    if (!(options = strdup(str))) return;
    for (opt = options; opt; opt = next)
    {
        const char *p;
        unsigned char set = 0, clear = 0;

        if ((next = strchr( opt, ',' ))) *next++ = 0;

        p = opt + strcspn( opt, "+-" );
        if (!p[0]) p = opt;  /* assume it's a debug channel name */

        if (p > opt)
        {
            for (i = 0; i < ARRAY_SIZE(debug_classes); i++)
            {
                int len = strlen(debug_classes[i]);
                if (len != (p - opt)) continue;
                if (!memcmp( opt, debug_classes[i], len ))  /* found it */
                {
                    if (*p == '+') set |= 1 << i;
                    else clear |= 1 << i;
                    break;
                }
            }
            if (i == ARRAY_SIZE(debug_classes)) /* bad class name, skip it */
                continue;
        }
        else
        {
            if (*p == '-') clear = ~0;
            else set = ~0;
        }
        if (*p == '+' || *p == '-') p++;
        if (!p[0]) continue;

        if (!strcmp( p, "all" ))
            default_flags = (default_flags & ~clear) | set;
        else
            add_option( p, set, clear );
    }
    free( options );
}

/* print the usage message */
static void debug_usage(void)
{
    static const char usage[] =
        "Syntax of the WINEDEBUG variable:\n"
        "  WINEDEBUG=[class]+xxx,[class]-yyy,...\n\n"
        "Example: WINEDEBUG=+relay,warn-heap\n"
        "    turns on relay traces, disable heap warnings\n"
        "Available message classes: err, warn, fixme, trace\n";
    write( 2, usage, sizeof(usage) - 1 );
    exit(1);
}

/* initialize all options at startup */
static void init_options(void)
{
    char *wine_debug = getenv("WINEDEBUG");
    struct stat st1, st2;

    nb_debug_options = 0;

    /* check for stderr pointing to /dev/null */
    if (!fstat( 2, &st1 ) && S_ISCHR(st1.st_mode) &&
        !stat( "/dev/null", &st2 ) && S_ISCHR(st2.st_mode) &&
        st1.st_rdev == st2.st_rdev)
    {
        default_flags = 0;
        return;
    }
    if (!wine_debug) return;
    if (!strcmp( wine_debug, "help" )) debug_usage();
    parse_options( wine_debug );
}

/***********************************************************************
 *		__wine_dbg_get_channel_flags  (NTDLL.@)
 *
 * Get the flags to use for a given channel, possibly setting them too in case of lazy init
 */
unsigned char __cdecl __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel )
{
    int min, max, pos, res;

    if (nb_debug_options == -1) init_options();

    min = 0;
    max = nb_debug_options - 1;
    while (min <= max)
    {
        pos = (min + max) / 2;
        res = strcmp( channel->name, debug_options[pos].name );
        if (!res) return debug_options[pos].flags;
        if (res < 0) max = pos - 1;
        else min = pos + 1;
    }
    /* no option for this channel */
    if (channel->flags & (1 << __WINE_DBCL_INIT)) channel->flags = default_flags;
    return default_flags;
}

/***********************************************************************
 *		__wine_dbg_strdup  (NTDLL.@)
 */
const char * __cdecl __wine_dbg_strdup( const char *str )
{
    struct debug_info *info = get_info();
    char *res = info->str_pos;
    size_t n = strlen( str ) + 1;

    if (res + n > &info->strings[sizeof(info->strings)]) res = info->strings;
    info->str_pos = res + n;
    return strcpy( res, str );
}

/***********************************************************************
 *		__wine_dbg_output  (NTDLL.@)
 */
int __cdecl __wine_dbg_output( const char *str )
{
    struct debug_info *info = get_info();
    const char *end = strrchr( str, '\n' );
    int ret = 0;

    if (end)
    {
        ret += append_output( info, str, end + 1 - str );
        write( 2, info->output, info->out_pos - info->output );
        info->out_pos = info->output;
        str = end + 1;
    }
    if (*str) ret += append_output( info, str, strlen( str ));
    return ret;
}

/***********************************************************************
 *		NTDLL_dbg_vprintf
 */
static int NTDLL_dbg_vprintf( const char *format, va_list args )
{
    struct debug_info *info = get_info();
    int end;

    int ret = vsnprintf( info->out_pos, sizeof(info->output) - (info->out_pos - info->output),
                         format, args );

    /* make sure we didn't exceed the buffer length
     * the two checks are due to glibc changes in vsnprintfs return value
     * the buffer size can be exceeded in case of a missing \n in
     * debug output */
    if ((ret == -1) || (ret >= sizeof(info->output) - (info->out_pos - info->output)))
    {
       fprintf( stderr, "wine_dbg_vprintf: debugstr buffer overflow (contents: '%s')\n",
                info->output);
       info->out_pos = info->output;
       abort();
    }

    for (end = ret; end > 0; end--) if (info->out_pos[end - 1] == '\n') break;

    if (!end) info->out_pos += ret;
    else
    {
        char *pos = info->output;
        write( 2, pos, info->out_pos + end - pos );
        /* move beginning of next line to start of buffer */
        memmove( pos, info->out_pos + end, ret - end );
        info->out_pos = pos + ret - end;
    }
    return ret;
}

/***********************************************************************
 *		NTDLL_dbg_vlog
 */
static int NTDLL_dbg_vlog( enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                           const char *function, const char *format, va_list args )
{
    static const char * const classes[] = { "fixme", "err", "warn", "trace" };
    struct debug_info *info = get_info();
    int ret = 0;

    /* only print header if we are at the beginning of the line */
    if (info->out_pos == info->output || info->out_pos[-1] == '\n')
    {
        if (TRACE_ON(timestamp))
        {
            ULONG ticks = NtGetTickCount();
            ret = wine_dbg_printf( "%3u.%03u:", ticks / 1000, ticks % 1000 );
        }
        if (TRACE_ON(pid))
            ret += wine_dbg_printf( "%04x:", GetCurrentProcessId() );
        ret += wine_dbg_printf( "%04x:", GetCurrentThreadId() );
        if (*format == '\1')  /* special magic to avoid standard prefix */
            format++;
        else if (cls < ARRAY_SIZE( classes ))
            ret += wine_dbg_printf( "%s:%s:%s ", classes[cls], channel->name, function );
    }
    if (format)
        ret += NTDLL_dbg_vprintf( format, args );
    return ret;
}


static const struct __wine_debug_functions funcs =
{
    get_temp_buffer,
    release_temp_buffer,
    wine_dbgstr_an,
    wine_dbgstr_wn,
    NTDLL_dbg_vprintf,
    NTDLL_dbg_vlog
};

/***********************************************************************
 *		debug_init
 */
void debug_init(void)
{
    if (!initial_info.str_pos) initial_info.str_pos = initial_info.strings;
    if (!initial_info.out_pos) initial_info.out_pos = initial_info.output;
    ntdll_get_thread_data()->debug_info = &initial_info;
    init_done = TRUE;
    __wine_dbg_set_functions( &funcs, NULL, sizeof(funcs) );
}
