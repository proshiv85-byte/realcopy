#include <gtk/gtk.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <string.h>

#define CHUNK_SIZE (4 * 1024 * 1024) // 4 MB

typedef struct {
    GtkApplication *gtk_app;
    GtkWidget *window;
    GtkWidget *btn_src;
    GtkWidget *btn_dst_dir;
    GtkWidget *btn_start;
    GtkWidget *btn_cancel;
    GtkWidget *progress_bar;
    GtkWidget *lbl_status;
    GtkWidget *lbl_speed;

    char *src_path;
    char *dst_dir_path;
    char *full_dst_path;

    gboolean is_copying;
    gboolean quit_after_copy;
    gint cancel_requested; // accessed atomically from the copy thread
} AppWidgets;

// Everything the copy thread needs, owned by the thread so the UI can't
// change it underneath.
typedef struct {
    AppWidgets *app;
    char *src_path;
    char *dst_dir_path;
    char *dst_path;
} CopyJob;

typedef enum {
    COPY_RUNNING,
    COPY_DONE,
    COPY_FAILED,
    COPY_CANCELLED,
} CopyState;

typedef struct {
    AppWidgets *app;
    CopyState state;
    double fraction;
    double speed_mbps;
    double eta_sec;
    goffset copied_bytes;
    goffset total_bytes;
    char *error_msg;
} ProgressData;

static void format_eta(double total_seconds, char *buffer, size_t buf_size) {
    if (total_seconds < 0) total_seconds = 0;

    int total_s = (int)total_seconds;
    int hours = total_s / 3600;
    int minutes = (total_s % 3600) / 60;
    int seconds = total_s % 60;

    if (hours > 0) {
        snprintf(buffer, buf_size, "%d:%02d:%02d", hours, minutes, seconds);
    } else {
        snprintf(buffer, buf_size, "%d:%02d", minutes, seconds);
    }
}

static void set_controls_sensitive(AppWidgets *app, gboolean idle) {
    gtk_widget_set_sensitive(app->btn_src, idle);
    gtk_widget_set_sensitive(app->btn_dst_dir, idle);
    gtk_widget_set_sensitive(app->btn_start, idle);
    gtk_widget_set_visible(app->btn_cancel, !idle);
    gtk_widget_set_sensitive(app->btn_cancel, !idle);
}

static gboolean update_ui_callback(gpointer user_data) {
    ProgressData *pd = (ProgressData *)user_data;
    AppWidgets *app = pd->app;

    if (pd->state == COPY_RUNNING) {
        char status_buf[128];
        char speed_buf[128];
        char eta_buf[32];

        format_eta(pd->eta_sec, eta_buf, sizeof(eta_buf));

        snprintf(status_buf, sizeof(status_buf), "%.2f MB / %.2f MB",
                 (double)pd->copied_bytes / (1024 * 1024),
                 (double)pd->total_bytes / (1024 * 1024));

        snprintf(speed_buf, sizeof(speed_buf), "%.2f MB/s • ETA %s",
                 pd->speed_mbps, eta_buf);

        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), pd->fraction);
        if (!g_atomic_int_get(&app->cancel_requested))
            gtk_label_set_text(GTK_LABEL(app->lbl_status), status_buf);
        gtk_label_set_text(GTK_LABEL(app->lbl_speed), speed_buf);
        g_free(pd);
        return G_SOURCE_REMOVE;
    }

    // The copy thread has exited.
    switch (pd->state) {
    case COPY_DONE:
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 1.0);
        gtk_label_set_text(GTK_LABEL(app->lbl_status), "Complete — data is on the drive");
        break;
    case COPY_CANCELLED:
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
        gtk_label_set_text(GTK_LABEL(app->lbl_status), "Cancelled — partial file removed");
        break;
    default:
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
        gtk_label_set_text(GTK_LABEL(app->lbl_status), pd->error_msg);
        break;
    }
    gtk_label_set_text(GTK_LABEL(app->lbl_speed), "");
    app->is_copying = FALSE;
    set_controls_sensitive(app, TRUE);

    if (app->quit_after_copy)
        g_application_quit(G_APPLICATION(app->gtk_app));

    g_free(pd->error_msg);
    g_free(pd);
    return G_SOURCE_REMOVE;
}

static void post_finished(AppWidgets *app, CopyState state, char *error_msg) {
    ProgressData *pd = g_new0(ProgressData, 1);
    pd->app = app;
    pd->state = state;
    pd->error_msg = error_msg;
    g_idle_add(update_ui_callback, pd);
}

// Builds a user-facing message for a failed syscall; call with errno intact.
static char *describe_error(const char *what, int err) {
    const char *hint = "";
    if (err == ENOSPC)
        hint = " (the drive is full)";
    else if (err == EFBIG)
        hint = " (file too large for this filesystem — FAT32 is limited to 4 GiB)";
    else if (err == EIO || err == ENODEV || err == ENXIO)
        hint = " (was the drive removed?)";
    return g_strdup_printf("Error: %s: %s%s", what, g_strerror(err), hint);
}

static ssize_t read_retry(int fd, void *buf, size_t len) {
    ssize_t n;
    do {
        n = read(fd, buf, len);
    } while (n < 0 && errno == EINTR);
    return n;
}

// Writes the whole buffer, looping over short writes. Returns 0 or -1 with errno set.
static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static gboolean same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static gpointer copy_thread_func(gpointer user_data) {
    CopyJob *job = (CopyJob *)user_data;
    AppWidgets *app = job->app;
    char *error_msg = NULL;
    CopyState result = COPY_FAILED;
    gboolean created_dst = FALSE;
    int dst_fd = -1;
    void *buffer = NULL;

    int src_fd = open(job->src_path, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        error_msg = describe_error("cannot open source file", errno);
        goto out;
    }

    struct stat src_st;
    if (fstat(src_fd, &src_st) < 0) {
        error_msg = describe_error("cannot stat source file", errno);
        goto out;
    }
    if (!S_ISREG(src_st.st_mode)) {
        error_msg = g_strdup("Error: source is not a regular file");
        goto out;
    }
    goffset total_size = src_st.st_size;

    // Open without O_TRUNC so we can make sure the target isn't the source
    // itself before throwing away its contents.
    dst_fd = open(job->dst_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (dst_fd < 0) {
        error_msg = describe_error("cannot open target", errno);
        goto out;
    }

    struct stat dst_st;
    if (fstat(dst_fd, &dst_st) < 0) {
        error_msg = describe_error("cannot stat target", errno);
        goto out;
    }
    if (same_file(&src_st, &dst_st)) {
        error_msg = g_strdup("Error: source and target are the same file");
        goto out;
    }
    created_dst = TRUE;
    if (ftruncate(dst_fd, 0) < 0) {
        error_msg = describe_error("cannot truncate target", errno);
        goto out;
    }

    buffer = malloc(CHUNK_SIZE);
    if (!buffer) {
        error_msg = g_strdup("Error: out of memory");
        goto out;
    }

    goffset copied_bytes = 0;
    gint64 start_time = g_get_monotonic_time();
    gint64 last_ui_update = start_time;

    while (TRUE) {
        if (g_atomic_int_get(&app->cancel_requested)) {
            result = COPY_CANCELLED;
            goto out;
        }

        ssize_t bytes_read = read_retry(src_fd, buffer, CHUNK_SIZE);
        if (bytes_read < 0) {
            error_msg = describe_error("read from source failed", errno);
            goto out;
        }
        if (bytes_read == 0) break;

        if (write_all(dst_fd, buffer, (size_t)bytes_read) < 0) {
            error_msg = describe_error("write to target failed", errno);
            goto out;
        }

        // Flush dirty pages to drive, so progress reflects what is really on it
        if (fdatasync(dst_fd) < 0) {
            error_msg = describe_error("flushing to drive failed", errno);
            goto out;
        }

        copied_bytes += bytes_read;

        gint64 now = g_get_monotonic_time();
        if ((now - last_ui_update) > 100000 || copied_bytes == total_size) {
            double elapsed_sec = (double)(now - start_time) / 1000000.0;
            double speed = (elapsed_sec > 0) ? ((double)copied_bytes / (1024 * 1024)) / elapsed_sec : 0.0;
            double remaining_bytes = (double)(total_size - copied_bytes) / (1024 * 1024);
            double eta = (speed > 0) ? remaining_bytes / speed : 0.0;

            ProgressData *pd = g_new0(ProgressData, 1);
            pd->app = app;
            pd->state = COPY_RUNNING;
            pd->fraction = (total_size > 0) ? MIN((double)copied_bytes / (double)total_size, 1.0) : 1.0;
            pd->speed_mbps = speed;
            pd->eta_sec = eta;
            pd->copied_bytes = copied_bytes;
            pd->total_bytes = total_size;

            g_idle_add(update_ui_callback, pd);
            last_ui_update = now;
        }
    }

    // Final full sync (data + metadata such as the file size), then make
    // sure close() didn't report a deferred write error.
    if (fsync(dst_fd) < 0) {
        error_msg = describe_error("flushing to drive failed", errno);
        goto out;
    }
    int rc = close(dst_fd);
    dst_fd = -1;
    if (rc < 0) {
        error_msg = describe_error("closing target failed", errno);
        goto out;
    }

    // Persist the directory entry too; otherwise on FAT/exFAT the new file
    // can be missing after the drive is unplugged.
    int dir_fd = open(job->dst_dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        error_msg = describe_error("cannot open target directory", errno);
        goto out;
    }
    if (fsync(dir_fd) < 0 && errno != EINVAL && errno != ENOTSUP) {
        error_msg = describe_error("flushing directory to drive failed", errno);
        close(dir_fd);
        goto out;
    }
    close(dir_fd);

    result = COPY_DONE;

out:
    free(buffer);
    if (src_fd >= 0) close(src_fd);
    if (dst_fd >= 0) close(dst_fd);
    if (result != COPY_DONE && created_dst)
        unlink(job->dst_path);

    post_finished(app, result, error_msg);

    g_free(job->src_path);
    g_free(job->dst_dir_path);
    g_free(job->dst_path);
    g_free(job);
    return NULL;
}

static void request_cancel(AppWidgets *app) {
    g_atomic_int_set(&app->cancel_requested, 1);
    gtk_widget_set_sensitive(app->btn_cancel, FALSE);
    gtk_label_set_text(GTK_LABEL(app->lbl_status), "Cancelling…");
}

static void on_confirm_close_choose(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source_object);
    AppWidgets *app = (AppWidgets *)user_data;

    GError *error = NULL;
    int button = gtk_alert_dialog_choose_finish(dialog, res, &error);

    if (error) {
        g_error_free(error);
        return;
    }

    if (button == 0) {
        if (!app->is_copying) {
            g_application_quit(G_APPLICATION(app->gtk_app));
            return;
        }
        // Let the copy thread stop and remove its partial file; we quit once
        // it reports back.
        app->quit_after_copy = TRUE;
        request_cancel(app);
    }
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    if (!app->is_copying) {
        return FALSE;
    }

    GtkAlertDialog *dialog = gtk_alert_dialog_new("Copy process is running");
    gtk_alert_dialog_set_detail(dialog, "Are you sure you want to cancel copying and exit?");

    const char *buttons[] = {"Exit", "Cancel", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 1);
    gtk_alert_dialog_set_default_button(dialog, 1);

    gtk_alert_dialog_choose(dialog, window, NULL, on_confirm_close_choose, app);
    g_object_unref(dialog);

    return TRUE;
}

static void on_select_src_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source_object), res, NULL);
    if (file) {
        g_free(app->src_path);
        app->src_path = g_file_get_path(file);
        gtk_button_set_label(GTK_BUTTON(app->btn_src), app->src_path);
        g_object_unref(file);
    }
}

static void on_select_src_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = (AppWidgets *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Source File");
    gtk_file_dialog_open(dialog, GTK_WINDOW(app->window), NULL, on_select_src_response, app);
    g_object_unref(dialog);
}

static void on_select_dst_dir_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source_object), res, NULL);
    if (folder) {
        g_free(app->dst_dir_path);
        app->dst_dir_path = g_file_get_path(folder);
        gtk_button_set_label(GTK_BUTTON(app->btn_dst_dir), app->dst_dir_path);
        g_object_unref(folder);
    }
}

static void on_select_dst_dir_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = (AppWidgets *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Target Directory");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), NULL, on_select_dst_dir_response, app);
    g_object_unref(dialog);
}

static void start_copy(AppWidgets *app) {
    CopyJob *job = g_new0(CopyJob, 1);
    job->app = app;
    job->src_path = g_strdup(app->src_path);
    job->dst_dir_path = g_strdup(app->dst_dir_path);
    job->dst_path = g_strdup(app->full_dst_path);

    app->is_copying = TRUE;
    app->quit_after_copy = FALSE;
    g_atomic_int_set(&app->cancel_requested, 0);
    set_controls_sensitive(app, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    gtk_label_set_text(GTK_LABEL(app->lbl_status), "Copying...");
    gtk_label_set_text(GTK_LABEL(app->lbl_speed), "");

    g_thread_unref(g_thread_new("copy_thread", copy_thread_func, job));
}

static void on_confirm_overwrite_choose(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    GError *error = NULL;
    int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source_object), res, &error);

    if (error) {
        g_error_free(error);
        return;
    }

    if (button == 0 && !app->is_copying)
        start_copy(app);
}

static void on_start_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = (AppWidgets *)user_data;

    if (app->is_copying)
        return;

    if (!app->src_path || !app->dst_dir_path) {
        gtk_label_set_text(GTK_LABEL(app->lbl_status), "Select source and target first");
        return;
    }

    char *filename = g_path_get_basename(app->src_path);
    g_free(app->full_dst_path);
    app->full_dst_path = g_build_filename(app->dst_dir_path, filename, NULL);

    struct stat src_st, dst_st;
    if (stat(app->full_dst_path, &dst_st) == 0) {
        if (stat(app->src_path, &src_st) == 0 && same_file(&src_st, &dst_st)) {
            gtk_label_set_text(GTK_LABEL(app->lbl_status),
                               "Error: source and target are the same file");
            g_free(filename);
            return;
        }
        if (!S_ISREG(dst_st.st_mode)) {
            gtk_label_set_text(GTK_LABEL(app->lbl_status),
                               "Error: target exists and is not a regular file");
            g_free(filename);
            return;
        }

        char *msg = g_strdup_printf("\"%s\" already exists", filename);
        GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", msg);
        gtk_alert_dialog_set_detail(dialog, "Do you want to replace it?");
        const char *buttons[] = {"Replace", "Cancel", NULL};
        gtk_alert_dialog_set_buttons(dialog, buttons);
        gtk_alert_dialog_set_cancel_button(dialog, 1);
        gtk_alert_dialog_set_default_button(dialog, 1);
        gtk_alert_dialog_choose(dialog, GTK_WINDOW(app->window), NULL, on_confirm_overwrite_choose, app);
        g_object_unref(dialog);
        g_free(msg);
        g_free(filename);
        return;
    }

    g_free(filename);
    start_copy(app);
}

static void on_cancel_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = (AppWidgets *)user_data;
    if (app->is_copying)
        request_cancel(app);
}

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
    (void)user_data;
    AppWidgets *app = g_new0(AppWidgets, 1);
    app->gtk_app = gtk_app;

    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "RealCopy");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 480, 220);

    g_signal_connect(app->window, "close-request", G_CALLBACK(on_close_request), app);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(app->window), vbox);

    app->btn_src = gtk_button_new_with_label("Select File");
    g_signal_connect(app->btn_src, "clicked", G_CALLBACK(on_select_src_clicked), app);
    gtk_box_append(GTK_BOX(vbox), app->btn_src);

    app->btn_dst_dir = gtk_button_new_with_label("Select Target Directory");
    g_signal_connect(app->btn_dst_dir, "clicked", G_CALLBACK(on_select_dst_dir_clicked), app);
    gtk_box_append(GTK_BOX(vbox), app->btn_dst_dir);

    app->btn_start = gtk_button_new_with_label("Copy");
    g_signal_connect(app->btn_start, "clicked", G_CALLBACK(on_start_clicked), app);
    gtk_box_append(GTK_BOX(vbox), app->btn_start);

    app->btn_cancel = gtk_button_new_with_label("Cancel Copy");
    g_signal_connect(app->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), app);
    gtk_widget_set_visible(app->btn_cancel, FALSE);
    gtk_box_append(GTK_BOX(vbox), app->btn_cancel);

    app->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress_bar), TRUE);
    gtk_box_append(GTK_BOX(vbox), app->progress_bar);

    app->lbl_status = gtk_label_new("Ready");
    gtk_label_set_wrap(GTK_LABEL(app->lbl_status), TRUE);
    gtk_box_append(GTK_BOX(vbox), app->lbl_status);

    app->lbl_speed = gtk_label_new("");
    gtk_box_append(GTK_BOX(vbox), app->lbl_speed);

    gtk_window_present(GTK_WINDOW(app->window));
}

int main(int argc, char **argv) {
    GtkApplication *gtk_app = gtk_application_new("com.realcopy.app", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);
    return status;
}
