#include <gtk/gtk.h>
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
    GtkWidget *progress_bar;
    GtkWidget *lbl_status;
    GtkWidget *lbl_speed;

    char *src_path;
    char *dst_dir_path;
    char *full_dst_path;

    gboolean is_copying;
} AppWidgets;

typedef struct {
    AppWidgets *app;
    double fraction;
    double speed_mbps;
    double eta_sec;
    goffset copied_bytes;
    goffset total_bytes;
    gboolean is_finished;
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

static gboolean update_ui_callback(gpointer user_data) {
    ProgressData *pd = (ProgressData *)user_data;
    AppWidgets *app = pd->app;

    if (pd->error_msg) {
        gtk_label_set_text(GTK_LABEL(app->lbl_status), pd->error_msg);
        gtk_widget_set_sensitive(app->btn_start, TRUE);
        app->is_copying = FALSE;
        g_free(pd->error_msg);
        g_free(pd);
        return G_SOURCE_REMOVE;
    }

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), pd->fraction);

    if (pd->is_finished) {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), "100%");
        gtk_label_set_text(GTK_LABEL(app->lbl_status), "Complete");
        gtk_label_set_text(GTK_LABEL(app->lbl_speed), "");
        gtk_widget_set_sensitive(app->btn_start, TRUE);
        app->is_copying = FALSE;
    } else {
        char status_buf[128];
        char speed_buf[128];
        char eta_buf[32];

        format_eta(pd->eta_sec, eta_buf, sizeof(eta_buf));

        snprintf(status_buf, sizeof(status_buf), "%.2f MB / %.2f MB",
                 (double)pd->copied_bytes / (1024 * 1024),
                 (double)pd->total_bytes / (1024 * 1024));

        snprintf(speed_buf, sizeof(speed_buf), "%.2f MB/s • ETA %s",
                 pd->speed_mbps, eta_buf);

        gtk_label_set_text(GTK_LABEL(app->lbl_status), status_buf);
        gtk_label_set_text(GTK_LABEL(app->lbl_speed), speed_buf);
    }

    g_free(pd);
    return G_SOURCE_REMOVE;
}

static gpointer copy_thread_func(gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    int src_fd = open(app->src_path, O_RDONLY);
    if (src_fd < 0) {
        ProgressData *pd = g_new0(ProgressData, 1);
        pd->app = app;
        pd->error_msg = g_strdup("Error: Cannot open source file");
        g_idle_add(update_ui_callback, pd);
        return NULL;
    }

    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        close(src_fd);
        ProgressData *pd = g_new0(ProgressData, 1);
        pd->app = app;
        pd->error_msg = g_strdup("Error: Cannot stat source file");
        g_idle_add(update_ui_callback, pd);
        return NULL;
    }
    goffset total_size = st.st_size;

    int dst_fd = open(app->full_dst_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (dst_fd < 0) {
        close(src_fd);
        ProgressData *pd = g_new0(ProgressData, 1);
        pd->app = app;
        pd->error_msg = g_strdup("Error: Permission denied / Cannot open target");
        g_idle_add(update_ui_callback, pd);
        return NULL;
    }

    void *buffer = malloc(CHUNK_SIZE);
    goffset copied_bytes = 0;

    gint64 start_time = g_get_monotonic_time();
    gint64 last_ui_update = start_time;

    while (TRUE) {
        ssize_t bytes_read = read(src_fd, buffer, CHUNK_SIZE);
        if (bytes_read <= 0) break;

        ssize_t bytes_written = write(dst_fd, buffer, bytes_read);
        if (bytes_written < 0) break;

        copied_bytes += bytes_written;

        // Flush dirty pages to drive
        fdatasync(dst_fd);

        gint64 now = g_get_monotonic_time();
        if ((now - last_ui_update) > 100000 || copied_bytes == total_size) {
            double elapsed_sec = (double)(now - start_time) / 1000000.0;
            double speed = (elapsed_sec > 0) ? ((double)copied_bytes / (1024 * 1024)) / elapsed_sec : 0.0;
            double remaining_bytes = (double)(total_size - copied_bytes) / (1024 * 1024);
            double eta = (speed > 0) ? remaining_bytes / speed : 0.0;

            ProgressData *pd = g_new0(ProgressData, 1);
            pd->app = app;
            pd->fraction = (total_size > 0) ? (double)copied_bytes / (double)total_size : 1.0;
            pd->speed_mbps = speed;
            pd->eta_sec = eta;
            pd->copied_bytes = copied_bytes;
            pd->total_bytes = total_size;
            pd->is_finished = FALSE;

            g_idle_add(update_ui_callback, pd);
            last_ui_update = now;
        }
    }

    free(buffer);
    close(src_fd);
    close(dst_fd);

    ProgressData *pd_done = g_new0(ProgressData, 1);
    pd_done->app = app;
    pd_done->fraction = 1.0;
    pd_done->is_finished = TRUE;
    g_idle_add(update_ui_callback, pd_done);

    return NULL;
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
        g_application_quit(G_APPLICATION(app->gtk_app));
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

static void on_select_src_response(GtkFileDialog *dialog, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, NULL);
    if (file) {
        if (app->src_path) g_free(app->src_path);
        app->src_path = g_file_get_path(file);
        gtk_button_set_label(GTK_BUTTON(app->btn_src), app->src_path);
        g_object_unref(file);
    }
}

static void on_select_src_clicked(GtkWidget *btn, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Source File");
    gtk_file_dialog_open(dialog, GTK_WINDOW(app->window), NULL, (GAsyncReadyCallback)on_select_src_response, app);
}

static void on_select_dst_dir_response(GtkFileDialog *dialog, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, NULL);
    if (folder) {
        if (app->dst_dir_path) g_free(app->dst_dir_path);
        app->dst_dir_path = g_file_get_path(folder);
        gtk_button_set_label(GTK_BUTTON(app->btn_dst_dir), app->dst_dir_path);
        g_object_unref(folder);
    }
}

static void on_select_dst_dir_clicked(GtkWidget *btn, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Target Directory");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), NULL, (GAsyncReadyCallback)on_select_dst_dir_response, app);
}

static void on_start_clicked(GtkWidget *btn, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;

    if (!app->src_path || !app->dst_dir_path) {
        gtk_label_set_text(GTK_LABEL(app->lbl_status), "Select source and target first");
        return;
    }

    char *filename = g_path_get_basename(app->src_path);
    if (app->full_dst_path) g_free(app->full_dst_path);
    app->full_dst_path = g_build_filename(app->dst_dir_path, filename, NULL);
    g_free(filename);

    app->is_copying = TRUE;
    gtk_widget_set_sensitive(app->btn_start, FALSE);
    gtk_label_set_text(GTK_LABEL(app->lbl_status), "Copying...");

    g_thread_new("copy_thread", copy_thread_func, app);
}

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
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

    app->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress_bar), TRUE);
    gtk_box_append(GTK_BOX(vbox), app->progress_bar);

    app->lbl_status = gtk_label_new("Ready");
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