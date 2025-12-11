/*
   Dynamic file list updater: inotify-based directory watcher

   Copyright (C) 2025
   Free Software Foundation, Inc.

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "dirwatch.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <stdint.h>
#endif

#include "lib/tty/key.h"  // add_select_channel(), delete_select_channel()
#include "lib/vfs/vfs.h"

#include "filemanager.h"  // left_panel/right_panel/current_panel
#include "panel.h"
#include "layout.h"  // dynamic_file_list_debounce_sec
#include "lib/widget/dialog-switch.h"  // do_refresh()

#ifdef __linux__

static int inotify_fd = -1;
static int timer_fd = -1;
static int wd_left = -1;
static int wd_right = -1;
static char *left_path = NULL;
static char *right_path = NULL;
static gboolean watcher_enabled = FALSE;
static gboolean watcher_quiet = FALSE;
static gboolean pending_left = FALSE;
static gboolean pending_right = FALSE;
/* default debounce interval in milliseconds */
/* debouncing interval comes from layout setting */

static const uint32_t DW_MASK =
    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;

static void dirwatch_reset_paths (void)
{
    g_free (left_path);
    g_free (right_path);
    left_path = right_path = NULL;
}

static void dirwatch_clear_watches (void)
{
    if (inotify_fd >= 0)
    {
        if (wd_left >= 0)
            (void) inotify_rm_watch (inotify_fd, wd_left);
        if (wd_right >= 0)
            (void) inotify_rm_watch (inotify_fd, wd_right);
    }
    wd_left = wd_right = -1;
    dirwatch_reset_paths ();
}

static void dirwatch_disarm_timer (void)
{
    if (timer_fd >= 0)
    {
        struct itimerspec its = { { 0, 0 }, { 0, 0 } };
        (void) timerfd_settime (timer_fd, 0, &its, NULL);
    }
}

static void dirwatch_arm_timer (void)
{
    if (timer_fd < 0)
        return;
    struct itimerspec its;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    int secs = dynamic_file_list_debounce_sec;
    if (secs < 1)
        secs = 1;
    its.it_value.tv_sec = secs;
    its.it_value.tv_nsec = 0;
    (void) timerfd_settime (timer_fd, 0, &its, NULL);
}

static gboolean dirwatch_panel_should_watch (WPanel *p, const char **out_path)
{
    const char *path;

    if (p == NULL)
        return FALSE;
    if (get_panel_type (p == left_panel ? 0 : 1) != view_listing)
        return FALSE;
    if (p->is_panelized)
        return FALSE;

    path = vfs_path_as_str (p->cwd_vpath);
    if (path == NULL)
        return FALSE;

    /* only local FS */
    if (!vfs_file_is_local (p->cwd_vpath))
        return FALSE;

    if (out_path != NULL)
        *out_path = path;
    return TRUE;
}

static void dirwatch_update_watches (void)
{
    if (!watcher_enabled || inotify_fd < 0)
        return;

    /* Left panel */
    {
        const char *path = NULL;
        if (dirwatch_panel_should_watch (left_panel, &path))
        {
            if (left_path == NULL || strcmp (left_path, path) != 0)
            {
                if (wd_left >= 0)
                    (void) inotify_rm_watch (inotify_fd, wd_left);
                wd_left = inotify_add_watch (inotify_fd, path, DW_MASK);
                g_free (left_path);
                left_path = g_strdup (path);
            }
        }
        else if (wd_left >= 0)
        {
            (void) inotify_rm_watch (inotify_fd, wd_left);
            wd_left = -1;
            g_free (left_path);
            left_path = NULL;
        }
    }

    /* Right panel */
    {
        const char *path = NULL;
        if (dirwatch_panel_should_watch (right_panel, &path))
        {
            if (right_path == NULL || strcmp (right_path, path) != 0)
            {
                if (wd_right >= 0)
                    (void) inotify_rm_watch (inotify_fd, wd_right);
                wd_right = inotify_add_watch (inotify_fd, path, DW_MASK);
                g_free (right_path);
                right_path = g_strdup (path);
            }
        }
        else if (wd_right >= 0)
        {
            (void) inotify_rm_watch (inotify_fd, wd_right);
            wd_right = -1;
            g_free (right_path);
            right_path = NULL;
        }
    }
}

static int dirwatch_callback (int fd, void *info)
{
    (void) info;

    if (fd != inotify_fd)
        return 0;

    /* Read all pending events */
    char buf[4096] __attribute__ ((aligned (__alignof__ (struct inotify_event))));
    ssize_t len;
    gboolean need_left = FALSE, need_right = FALSE;

    while ((len = read (inotify_fd, buf, sizeof (buf))) > 0)
    {
        const char *ptr = buf;
        while (ptr < buf + len)
        {
            const struct inotify_event *ev = (const struct inotify_event *) ptr;
            /* Note: both panels can watch the same path. In that case
             * inotify returns the same watch descriptor for both adds
             * on the same inotify fd. Check both independently so both
             * panels can be marked for reload when wd_left == wd_right. */
            if (ev->wd == wd_left)
                need_left = TRUE;
            if (ev->wd == wd_right)
                need_right = TRUE;
            ptr += sizeof (struct inotify_event) + ev->len;
        }
    }

    if (need_left)
        pending_left = TRUE;
    if (need_right)
        pending_right = TRUE;

    if (pending_left || pending_right)
        dirwatch_arm_timer ();

    return 0;
}

static void
dirwatch_apply_pending (void)
{
    gboolean do_left, do_right;

    do_left = pending_left;
    do_right = pending_right;

    pending_left = pending_right = FALSE;

    if (do_left && left_panel != NULL)
        panel_reload (left_panel);
    if (do_right && right_panel != NULL)
        panel_reload (right_panel);

    if (do_left || do_right)
        repaint_screen ();

    /* disarm until new events arrive */
    dirwatch_disarm_timer ();
}

static int dirwatch_timer_callback (int fd, void *info)
{
    (void) info;
    if (fd != timer_fd)
        return 0;
    /* drain timerfd */
    uint64_t expirations = 0;
    (void) read (timer_fd, &expirations, sizeof (expirations));

    /* snapshot and clear pending flags */
    if (watcher_quiet)
    {
        /* keep pending flags set; they will be applied when quiet mode ends */
        dirwatch_disarm_timer ();
        return 0;
    }

    dirwatch_apply_pending ();
    return 0;
}

void dirwatch_set_enabled (gboolean enabled)
{
    if (enabled == watcher_enabled)
    {
        /* Ensure watches match current directories */
        if (enabled)
            dirwatch_update_watches ();
        return;
    }

    watcher_enabled = enabled;

    if (!enabled)
    {
        if (inotify_fd >= 0)
        {
            delete_select_channel (inotify_fd);
            dirwatch_clear_watches ();
            close (inotify_fd);
            inotify_fd = -1;
        }
        if (timer_fd >= 0)
        {
            delete_select_channel (timer_fd);
            close (timer_fd);
            timer_fd = -1;
        }
        return;
    }

    /* Enable */
    if (inotify_fd < 0)
    {
        /* IN_NONBLOCK to integrate with select loop easily */
        inotify_fd = inotify_init1 (IN_NONBLOCK);
        if (inotify_fd < 0)
        {
            watcher_enabled = FALSE;
            return;
        }
        add_select_channel (inotify_fd, dirwatch_callback, NULL);
    }

    if (timer_fd < 0)
    {
        timer_fd = timerfd_create (CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer_fd >= 0)
            add_select_channel (timer_fd, dirwatch_timer_callback, NULL);
    }

    dirwatch_update_watches ();
}

void
dirwatch_set_quiet (gboolean quiet)
{
    if (!watcher_enabled || watcher_quiet == quiet)
        return;

    watcher_quiet = quiet;

    if (!watcher_quiet && (pending_left || pending_right))
        dirwatch_apply_pending ();
}

void dirwatch_panel_dir_changed (WPanel *panel)
{
    (void) panel;
    if (!watcher_enabled)
        return;
    dirwatch_update_watches ();
}

#else  /* !__linux__ */

void dirwatch_set_enabled (gboolean enabled)
{
    (void) enabled;
}

void
dirwatch_set_quiet (gboolean quiet)
{
    (void) quiet;
}

void dirwatch_panel_dir_changed (WPanel *panel)
{
    (void) panel;
}

#endif /* __linux__ */
