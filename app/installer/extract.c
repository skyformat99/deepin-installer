/**
 * Copyright (c) 2011 ~ 2013 Deepin, Inc.
 *               2011 ~ 2013 Long Wei
 *
 * Author:      Long Wei <yilang2007lw@gmail.com>
 * Maintainer:  Long Wei <yilang2007lw@gamil.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 **/

#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <ftw.h>
#include "extract.h"
#include "keyboard.h"

extern int symlink(const char *oldpath, const char *newpath);
extern int mknod(const char *pathname, mode_t mode, dev_t dev);
extern int lstat(const char *restrict path, struct stat *restrict buf);
extern int lchown(const char *path, uid_t owner, gid_t group);

#define BUFFERSIZE 	16 * 1024

static gboolean extract_finish = FALSE;

static gboolean
timeout_emit_cb (gpointer data)
{
    if (extract_finish) {
        return FALSE;
    }
    emit_progress ("extract", "ticker");
    return TRUE;
}

static void
copy_single_file (const char *src, const char *dest)
{
    FILE *sf = fopen (src, "r");
    if (sf == NULL) {
	    g_warning ("copy single file:open src %s failed\n", src);
	    return;
    }

    if (g_file_test (dest, G_FILE_TEST_EXISTS)) {
    	g_unlink (dest);
    }
    FILE *df = fopen (dest, "a");
    if (df == NULL) {
	    g_warning ("copy single file:open dest %s failed\n", dest);
	    fclose (sf);
	    return;
    }

    char buffer[BUFFERSIZE];
    size_t n;

    while ((n = fread (buffer, sizeof(char), sizeof(buffer), sf)) > 0) {
	    if (fwrite (buffer, sizeof(char), n, df) != n) {
	        g_warning ("copy single file:%s failed\n", src);
	        break;
        }
    }

    fclose (sf);
    fclose (df);
}

static gboolean
ancestor_is_symlink (const char *path)
{
    GFile *self = g_file_new_for_path (path);
    GFile *parent = NULL;
    GFile *tmp = self;

    while ((parent = g_file_get_parent (tmp)) != NULL) {
        g_object_unref (tmp);
        gchar *ppath = g_file_get_path (parent);
        if (g_file_test (ppath, G_FILE_TEST_IS_SYMLINK)) {
            g_object_unref (parent);
            g_free (ppath);
            return TRUE;
        }
        g_free (ppath);
        tmp = parent;
    }
    if (parent != NULL) {
        g_object_unref (parent);
    }
    return FALSE;
}

static int 
copy_file_cb (const char *path, const struct stat *sb, int typeflag)
{
    if (ancestor_is_symlink (path)) {
        return 0;
    }

    extern const gchar *target;
    gchar *ts = g_strdup_printf ("%s/squashfs", target);
    if (!g_str_has_prefix (path, ts)) {
    	g_warning ("copy file cb:invalid path->%s with target %s\n", path, ts);
	    return 1;
    }

    gchar **sp = g_strsplit (path, ts, -1);
    g_free (ts);

    gchar *base = g_strdup (sp[1]);
    g_strfreev (sp);

    gchar *dest = g_strdup_printf ("%s%s", target, base);
    g_free (base);

    struct stat st;
    if (lstat (path, &st) != 0) {
    	g_warning ("copy file cb:lstat %s\n", path);
	    return 1;
    }
    mode_t mode = st.st_mode;

    if (S_ISLNK (mode)) {
        GFile *file = g_file_new_for_path (dest);
    	GError *error = NULL;

    	gchar *link = g_file_read_link (path, &error);
    	if (error != NULL) {
    	    g_error_free (error);
    	}
    	error = NULL;

        g_file_make_symbolic_link (file, link, NULL, &error);
        if (error != NULL) {
            g_warning ("copy file cb:make symlink from %s to %s failed-> %s\n", dest, link, error->message);
            g_error_free (error);
        }
        g_free (link);
        error = NULL;
        g_object_unref (file);
    
    } else if (S_ISDIR (mode)) {
	    g_mkdir_with_parents (dest, mode);

    } else if (S_ISREG (mode)) {
	    copy_single_file (path, dest);
	    chmod (dest, mode);

    } else {
	    mknod (dest, mode, st.st_rdev);
    }

    if (lchown (dest, st.st_uid, st.st_gid) != 0) {
        g_warning ("copy file cb:lchown for %s failed\n", dest);
    }
    if (!S_ISLNK (mode)) {
        if (g_chmod (dest, mode) != 0) {
            g_warning ("copy file cb:chmod for %s failed\n", dest);
        }
    }
    g_free (dest);

    return 0;
}

gpointer 
thread_extract_iso (gpointer data)
{
    gboolean succeed = FALSE;
    extern const gchar *target;
    GError *error = NULL;
    gchar *target_iso;
    gchar *mount_cmd;
    gchar *umount_cmd;

    if (target == NULL) {
        g_warning ("extract iso:target NULL\n");
        goto out;
    }

    target_iso = g_strdup_printf ("%s/squashfs", target);
    if (g_mkdir_with_parents (target_iso, 0755) == -1) {
        g_warning ("extract iso:mkdir failed\n");
        goto out;
    }

    mount_cmd = g_strdup_printf ("mount /cdrom/casper/filesystem.squashfs %s", target_iso);
    g_spawn_command_line_sync (mount_cmd, NULL, NULL, NULL, &error);
    if (error != NULL) {
        g_warning ("extract iso:mount squashfs->%s\n", error->message);
        goto out;
    }

    guint cb_id = g_timeout_add (1000, (GSourceFunc) timeout_emit_cb, NULL);

    ftw (target_iso, copy_file_cb, 65536);

    umount_cmd  = g_strdup_printf ("umount %s", target_iso);
    g_spawn_command_line_sync (umount_cmd, NULL, NULL, NULL, &error);
    if (error != NULL) {
        g_warning ("extract iso:umount squashfs->%s\n", error->message);
        goto out;
    }
    succeed = TRUE;
    goto out;
out:
    g_free (mount_cmd);
    g_free (umount_cmd);
    g_free (target_iso);
    if (error != NULL) {
        g_error_free (error);
    }
    if (cb_id > 0) {
        g_source_remove (cb_id);
        cb_id = 0;
    }
    extract_finish = TRUE;

    if (succeed) {
        emit_progress ("extract", "finish");
    } else {
        emit_progress ("extract", "terminate");
    }
    return NULL;
}

JS_EXPORT_API
void installer_extract_iso ()
{
    g_printf ("extract intelligent:use extract iso\n");
    emit_progress ("extract", "safe");

    GThread *thread = g_thread_new ("extract_iso", (GThreadFunc) thread_extract_iso, NULL);
    g_thread_unref (thread);
}

static void
watch_extract_child (GPid pid, gint status, gpointer data)
{
    GError *error = NULL;
    if (data == NULL) {
        g_warning ("watch extract child:arg data is NULL\n");
    }
    g_spawn_check_exit_status (status, &error);
    if (error != NULL) {
        g_warning ("watch extract child:error->%s\n", error->message);
        g_error_free (error);
        emit_progress ("extract", "terminate");
    } else {
        g_printf ("watch extract child:extract finish\n");
        emit_progress ("extract", "finish");
    }

    guint *cb_ids = (guint *) data;
    if (cb_ids[0] > 0) {
        g_source_remove (cb_ids[0]);
        cb_ids[0] = 0;
    }
    if (cb_ids[1] > 0) {
        g_source_remove (cb_ids[1]);
        cb_ids[1] = 0;
    }
    if (cb_ids[2] > 0) {
        g_source_remove (cb_ids[2]);
        cb_ids[2] = 0;
    }

    g_free (cb_ids);
    g_spawn_close_pid (pid);

    extract_finish = TRUE;
}

static gboolean
cb_out_watch (GIOChannel *channel, GIOCondition cond, gpointer data)
{
    gchar **progress = (gchar **) data;
    gchar *string;
    gsize size;

    if (cond == G_IO_HUP) {
        g_io_channel_unref (channel);
        return FALSE;
    }

    g_io_channel_read_line (channel, &string, &size, NULL, NULL);
    //g_printf ("cb out watch:read->%s\n", string);
    gchar *match = get_matched_string (string, "\\d{1,3}%");
    //g_printf ("cb out watch:match->%s\n", match);

    if (match == NULL) {
        g_warning ("cb out watch:line without progress->%s\n", string);
    } else {
        if (progress != NULL && *progress != NULL) {
            g_free (*progress);
            *progress = NULL;
        } 
        *progress = g_strdup (match);
    }

    g_free (match);
    g_free (string);

    return TRUE;
}

static gboolean
cb_err_watch (GIOChannel *channel, GIOCondition cond, gpointer data)
{
    gchar *string;
    gsize size;

    if (cond == G_IO_HUP) {
        g_io_channel_unref (channel);
        return FALSE;
    }

    g_io_channel_read_line (channel, &string, &size, NULL, NULL);
    g_printf ("cb err watch:%s\n", string);
    g_free (string);

    return TRUE;
}

static gboolean
cb_timeout (gpointer data)
{
    gchar **progress = (gchar **) data;
    if (progress != NULL) {
        if (*progress != NULL) {
            emit_progress ("extract", *progress);
        } else {
            g_warning ("cb timeout:*progress NULL\n");
        }
    } else {
        g_warning ("cb timeout:progress NULL\n");
    }
    return TRUE;
}

gpointer
thread_extract_squashfs (gpointer data)
{
    gchar *squashfs_cmd = g_find_program_in_path ("unsquashfs");
    if (squashfs_cmd == NULL) {
        g_warning ("extract squashfs: unsquashfs not installed\n");
        emit_progress ("extract", "terminate");
        return NULL;
    }
    g_free (squashfs_cmd);

    extern const gchar *target;
    if (target == NULL) {
        g_warning ("extract squash fs:target is NULL\n");
        emit_progress ("extract", "terminate");
        return NULL;
    }

    gchar *share = g_strdup_printf ("%s%s", target, "/usr/share");
    g_rmdir (share);
    g_free (share);

    const gchar *iso = "/cdrom/casper/filesystem.squashfs";
    if (!g_file_test (iso, G_FILE_TEST_EXISTS)) {
        g_warning ("extract squashfs:iso not exists\n");
        emit_progress ("extract", "terminate");
        return NULL;
    }

    gchar **argv = g_new0 (gchar *, 6);
    argv[0] = g_strdup ("unsquashfs");
    argv[1] = g_strdup ("-f");
    argv[2] = g_strdup ("-d");
    argv[3] = g_strdup (target);
    argv[4] = g_strdup ("/cdrom/casper/filesystem.squashfs");

    gint std_output;
    gint std_error;
    GError *error = NULL;
    GPid pid;
    GIOChannel *out_channel = NULL;
    GIOChannel *err_channel = NULL;
    guint *cb_ids = g_new0 (guint, 3);
    gchar **progress = g_new0 (gchar *, 1);

    g_spawn_async_with_pipes (NULL,
                              argv,
                              NULL,
                              G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                              NULL,
                              NULL,
                              &pid,
                              NULL,
                              &std_output,
                              &std_error,
                              &error);
    if (error != NULL) {
        g_warning ("extract squashfs:spawn async pipes %s\n", error->message);
        g_error_free (error);
    }
    error = NULL;

    out_channel = g_io_channel_unix_new (std_output);
    err_channel = g_io_channel_unix_new (std_error);

    cb_ids[0] = g_io_add_watch_full (out_channel,
                                     G_PRIORITY_LOW,
                                     G_IO_IN | G_IO_HUP,
                                     (GIOFunc) cb_out_watch,
                                     progress,
                                     (GDestroyNotify) g_io_channel_unref);

    cb_ids[1] = g_io_add_watch_full (err_channel,
                                     G_PRIORITY_LOW,
                                     G_IO_IN | G_IO_HUP,
                                     (GIOFunc) cb_err_watch,
                                     progress,
                                     (GDestroyNotify) g_io_channel_unref);

    cb_ids[2] = g_timeout_add (2000, (GSourceFunc) cb_timeout, progress);

    g_child_watch_add (pid, (GChildWatchFunc) watch_extract_child, cb_ids);

    g_strfreev (argv);
    return NULL;
}


JS_EXPORT_API
void installer_extract_squashfs ()
{
    GThread *thread = g_thread_new ("extract_squashfs", (GThreadFunc) thread_extract_squashfs, NULL);
    g_thread_unref (thread);
}

static gboolean
is_outdated_machine ()
{
    const gchar *cmd = "udevadm info --query=property --name=/dev/input/mouse0";
    gchar *output = NULL;
    g_spawn_command_line_sync (cmd, &output, NULL, NULL, NULL);
    if (output != NULL) {
        if (g_strrstr (output, "vmware") != NULL || g_strrstr (output, "virtualbox") != NULL) {
            g_free (output);
            return FALSE;
        }
    } else {
        g_warning ("is outdated machine:udevadm\n");
    }
    g_free (output);

    const gchar *kvm_cmd  = "lscpu";
    gchar *kvm_output = NULL;
    g_spawn_command_line_sync (kvm_cmd, &kvm_output, NULL, NULL, NULL);
    if (kvm_output != NULL) {
        if (g_strrstr (kvm_output, "KVM") != NULL) {
            g_free (kvm_output);
            return FALSE;
        }
    } else {
        g_warning ("is outdated machine:lscpu not kvm\n");
    }
    g_free (kvm_output);
    
    double totalram = installer_get_memory_size ();
    if (totalram > 0 && totalram < 2.0 * 1024 * 1024 * 1024) {
        g_warning ("is outdated machine:total mem %f less than 2G\n", totalram);
        return TRUE;
    }

    guint processors = get_cpu_num ();
    if (processors < 2) {
        return TRUE;
    }

    return FALSE;
}

JS_EXPORT_API
void installer_extract_intelligent ()
{
    gchar *cmd = g_find_program_in_path ("os-prober");
    if (cmd == NULL) {
        g_warning ("os:os-prober not installed\n");
    }
    g_spawn_command_line_async ("pkill -9 os-prober", NULL);
    g_free (cmd);

    extern gchar *extract_mode;
    if (extract_mode != NULL) {
        if (g_strcmp0 (extract_mode, "fast") == 0) {
            installer_extract_squashfs ();

        } else if (g_strcmp0 (extract_mode, "safe") == 0) {
            installer_extract_iso ();

        } else {
            if (is_outdated_machine ()) {
                installer_extract_iso ();
            } else {
                installer_extract_squashfs ();
            }
        }
    } else {
        if (is_outdated_machine ()) {
            installer_extract_iso ();
        } else {
            installer_extract_squashfs ();
        }
    }
}
