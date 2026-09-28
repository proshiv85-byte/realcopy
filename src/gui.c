// realcopy-gui: GTK4 front end.
#include "copy.h"

#include <gtk/gtk.h>

typedef struct {
    GtkApplication *gtk_app;
    GtkWidget *window;
    GtkWidget *src_list;
    GtkWidget *btn_add_files;
    GtkWidget *btn_add_folders;
    GtkWidget *btn_clear;
    GtkWidget *btn_dst_dir;
    GtkWidget *chk_verify;
    GtkWidget *btn_start;
    GtkWidget *btn_cancel;
    GtkWidget *progress_bar;
    GtkWidget *lbl_status;
    GtkWidget *lbl_file;
    GtkWidget *lbl_speed;
    GtkWidget *btn_eject;

    GPtrArray *sources; // char *, absolute paths
    char *dst_dir_path;
    GMount *mount;      // drive of the last successful copy, for "Safely Remove"

    gboolean is_copying;
    gboolean quit_after_copy;
    GCancellable *cancellable;
    guint pulse_id;
} AppWidgets;

// Owned by the copy thread, so the UI can't change it underneath.
typedef struct {
    AppWidgets *app;
    char **sources;
    char *dest_dir;
    RcOptions options;
} CopyJob;

typedef struct {
    AppWidgets *app;
    RcProgress progress;
    char *current_file;
} ProgressMsg;

typedef struct {
    AppWidgets *app;
    gboolean ok;
    RcSummary summary;
    GError *error;
} FinishedMsg;

// ---- progress --------------------------------------------------------------

static gboolean pulse_cb(gpointer user_data) {
    AppWidgets *app = user_data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress_bar));
    return G_SOURCE_CONTINUE;
}

static void set_pulsing(AppWidgets *app, gboolean pulsing) {
    if (pulsing && !app->pulse_id) {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), "");
        app->pulse_id = g_timeout_add(100, pulse_cb, app);
    } else if (!pulsing && app->pulse_id) {
        g_source_remove(app->pulse_id);
        app->pulse_id = 0;
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress_bar), NULL);
    }
}

static gboolean progress_ui_cb(gpointer user_data) {
    ProgressMsg *msg = user_data;
    AppWidgets *app = msg->app;
    const RcProgress *p = &msg->progress;
    gboolean cancelling = g_cancellable_is_cancelled(app->cancellable);

    if (p->phase == RC_PHASE_SCANNING || p->phase == RC_PHASE_SYNCING) {
        set_pulsing(app, TRUE);
        if (!cancelling)
            gtk_label_set_text(GTK_LABEL(app->lbl_status),
                               p->phase == RC_PHASE_SCANNING
                                   ? "Preparing…"
                                   : "Finishing — writing the last data to the drive…");
        gtk_label_set_text(GTK_LABEL(app->lbl_file), "");
        gtk_label_set_text(GTK_LABEL(app->lbl_speed), "");
        goto out;
    }

    set_pulsing(app, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar),
                                  p->total_bytes ? MIN((double)p->done_bytes / p->total_bytes, 1.0) : 1.0);

    if (!cancelling) {
        char *status = g_strdup_printf("%s file %u of %u",
                                       p->phase == RC_PHASE_VERIFYING ? "Verifying" : "Copying",
                                       MIN(p->files_done + 1, p->files_total), p->files_total);
        gtk_label_set_text(GTK_LABEL(app->lbl_status), status);
        g_free(status);
    }
    gtk_label_set_text(GTK_LABEL(app->lbl_file), msg->current_file ? msg->current_file : "");

    GString *detail = g_string_new(NULL);
    if (p->speed > 0) {
        char *speed = g_format_size((guint64)p->speed);
        g_string_append_printf(detail, "%s/s", speed);
        g_free(speed);
    }
    if (p->eta >= 0) {
        char eta[32];
        rc_format_duration(p->eta, eta, sizeof(eta));
        g_string_append_printf(detail, "%sETA %s", detail->len ? " • " : "", eta);
    }
    if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(app->chk_verify))) {
        char *done = g_format_size(p->done_bytes);
        char *total = g_format_size(p->total_bytes);
        g_string_append_printf(detail, "%s%s of %s", detail->len ? " • " : "", done, total);
        g_free(done);
        g_free(total);
    }
    gtk_label_set_text(GTK_LABEL(app->lbl_speed), detail->str);
    g_string_free(detail, TRUE);

out:
    g_free(msg->current_file);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

// Called on the copy thread.
static void on_progress(const RcProgress *p, gpointer user_data) {
    ProgressMsg *msg = g_new0(ProgressMsg, 1);
    msg->app = user_data;
    msg->progress = *p;
    msg->current_file = g_strdup(p->current_file);
    msg->progress.current_file = NULL;
    g_idle_add(progress_ui_cb, msg);
}

// ---- controls --------------------------------------------------------------

static void set_controls_sensitive(AppWidgets *app, gboolean idle) {
    gtk_widget_set_sensitive(app->src_list, idle);
    gtk_widget_set_sensitive(app->btn_add_files, idle);
    gtk_widget_set_sensitive(app->btn_add_folders, idle);
    gtk_widget_set_sensitive(app->btn_clear, idle);
    gtk_widget_set_sensitive(app->btn_dst_dir, idle);
    gtk_widget_set_sensitive(app->chk_verify, idle);
    gtk_widget_set_sensitive(app->btn_start, idle);
    gtk_widget_set_visible(app->btn_cancel, !idle);
    gtk_widget_set_sensitive(app->btn_cancel, !idle);
}

static void set_mount(AppWidgets *app, GMount *mount) {
    g_clear_object(&app->mount);
    app->mount = mount;
    if (mount) {
        char *name = g_mount_get_name(mount);
        char *label = g_strdup_printf("Safely Remove “%s”", name);
        gtk_button_set_label(GTK_BUTTON(app->btn_eject), label);
        g_free(label);
        g_free(name);
    }
    gtk_widget_set_visible(app->btn_eject, mount != NULL);
    gtk_widget_set_sensitive(app->btn_eject, TRUE);
}

// Finds the removable drive the target folder lives on, if any.
static GMount *find_removable_mount(const char *path) {
    GFile *file = g_file_new_for_path(path);
    GMount *mount = g_file_find_enclosing_mount(file, NULL, NULL);
    g_object_unref(file);
    if (mount && !g_mount_can_eject(mount) && !g_mount_can_unmount(mount))
        g_clear_object(&mount);
    return mount;
}

static void set_dst_dir(AppWidgets *app, const char *path) {
    g_free(app->dst_dir_path);
    app->dst_dir_path = g_strdup(path);
    gtk_button_set_label(GTK_BUTTON(app->btn_dst_dir), path ? path : "Select Target Folder");
    set_mount(app, NULL);
}

// ---- source list -----------------------------------------------------------

static void update_source_buttons(AppWidgets *app) {
    gtk_widget_set_sensitive(app->btn_clear, app->sources->len > 0 && !app->is_copying);
}

static void on_remove_source_clicked(GtkButton *btn, gpointer user_data) {
    AppWidgets *app = user_data;
    GtkWidget *row = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_LIST_BOX_ROW);
    const char *path = g_object_get_data(G_OBJECT(row), "path");

    for (guint i = 0; i < app->sources->len; i++) {
        if (g_strcmp0(app->sources->pdata[i], path) == 0) {
            g_ptr_array_remove_index(app->sources, i);
            break;
        }
    }
    gtk_list_box_remove(GTK_LIST_BOX(app->src_list), row);
    update_source_buttons(app);
}

static void add_source(AppWidgets *app, GFile *file) {
    char *path = g_file_get_path(file);
    if (!path)
        return; // not a local file (e.g. a network location)

    for (guint i = 0; i < app->sources->len; i++) {
        if (g_strcmp0(app->sources->pdata[i], path) == 0) {
            g_free(path);
            return;
        }
    }
    g_ptr_array_add(app->sources, path);

    gboolean is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 2);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(is_dir ? "folder-symbolic"
                                                                     : "text-x-generic-symbolic"));
    GtkWidget *label = gtk_label_new(path);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_tooltip_text(label, path);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(remove, "flat");
    gtk_widget_set_tooltip_text(remove, "Remove from list");
    g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_source_clicked), app);
    gtk_box_append(GTK_BOX(box), remove);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
    gtk_list_box_append(GTK_LIST_BOX(app->src_list), row);

    update_source_buttons(app);
}

static void add_sources_from_model(AppWidgets *app, GListModel *files) {
    guint n = g_list_model_get_n_items(files);
    for (guint i = 0; i < n; i++) {
        GFile *file = g_list_model_get_item(files, i);
        add_source(app, file);
        g_object_unref(file);
    }
}

static void on_add_files_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source_object), res, NULL);
    if (files) {
        add_sources_from_model(user_data, files);
        g_object_unref(files);
    }
}

static void on_add_files_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Add Files");
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(app->window), NULL, on_add_files_response, app);
    g_object_unref(dialog);
}

static void on_add_folders_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GListModel *folders = gtk_file_dialog_select_multiple_folders_finish(GTK_FILE_DIALOG(source_object), res, NULL);
    if (folders) {
        add_sources_from_model(user_data, folders);
        g_object_unref(folders);
    }
}

static void on_add_folders_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Add Folders");
    gtk_file_dialog_select_multiple_folders(dialog, GTK_WINDOW(app->window), NULL, on_add_folders_response, app);
    g_object_unref(dialog);
}

static void on_clear_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = user_data;
    gtk_list_box_remove_all(GTK_LIST_BOX(app->src_list));
    g_ptr_array_set_size(app->sources, 0);
    update_source_buttons(app);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    AppWidgets *app = user_data;
    if (app->is_copying || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;

    for (GSList *l = gdk_file_list_get_files(g_value_get_boxed(value)); l; l = l->next)
        add_source(app, l->data);
    return TRUE;
}

// ---- target ----------------------------------------------------------------

static void on_select_dst_dir_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = user_data;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source_object), res, NULL);
    if (folder) {
        char *path = g_file_get_path(folder);
        if (path)
            set_dst_dir(app, path);
        g_free(path);
        g_object_unref(folder);
    }
}

static void on_select_dst_dir_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Target Folder");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), NULL, on_select_dst_dir_response, app);
    g_object_unref(dialog);
}

// ---- copying ---------------------------------------------------------------

static gboolean finished_ui_cb(gpointer user_data) {
    FinishedMsg *msg = user_data;
    AppWidgets *app = msg->app;

    set_pulsing(app, FALSE);
    gtk_label_set_text(GTK_LABEL(app->lbl_file), "");
    gtk_label_set_text(GTK_LABEL(app->lbl_speed), "");

    if (msg->ok) {
        char *text = rc_summary_to_string(&msg->summary);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 1.0);
        gtk_label_set_text(GTK_LABEL(app->lbl_status), text);
        g_free(text);
        set_mount(app, find_removable_mount(app->dst_dir_path));
    } else if (g_error_matches(msg->error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
        gtk_label_set_text(GTK_LABEL(app->lbl_status), "Cancelled — the partly copied file was removed");
    } else {
        GString *text = g_string_new("Error: ");
        g_string_append(text, msg->error->message);
        if (msg->summary.files_copied)
            g_string_append_printf(text, "\n(%u file%s copied completely before the error)",
                                   msg->summary.files_copied,
                                   msg->summary.files_copied == 1 ? " was" : "s were");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
        gtk_label_set_text(GTK_LABEL(app->lbl_status), text->str);
        g_string_free(text, TRUE);
    }

    app->is_copying = FALSE;
    set_controls_sensitive(app, TRUE);
    update_source_buttons(app);

    if (app->quit_after_copy)
        g_application_quit(G_APPLICATION(app->gtk_app));

    g_clear_error(&msg->error);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

static gpointer copy_thread_func(gpointer user_data) {
    CopyJob *job = user_data;
    FinishedMsg *msg = g_new0(FinishedMsg, 1);
    msg->app = job->app;
    msg->ok = rc_copy(&job->options, &msg->summary, &msg->error);
    g_idle_add(finished_ui_cb, msg);

    g_object_unref(job->options.cancellable);
    g_strfreev(job->sources);
    g_free(job->dest_dir);
    g_free(job);
    return NULL;
}

static void start_copy(AppWidgets *app, RcConflictPolicy conflict) {
    CopyJob *job = g_new0(CopyJob, 1);
    job->app = app;
    job->sources = g_new0(char *, app->sources->len + 1);
    for (guint i = 0; i < app->sources->len; i++)
        job->sources[i] = g_strdup(app->sources->pdata[i]);
    job->dest_dir = g_strdup(app->dst_dir_path);

    g_clear_object(&app->cancellable);
    app->cancellable = g_cancellable_new();

    job->options.sources = (const char *const *)job->sources;
    job->options.dest_dir = job->dest_dir;
    job->options.conflict = conflict;
    job->options.verify = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->chk_verify));
    job->options.cancellable = g_object_ref(app->cancellable);
    job->options.progress = on_progress;
    job->options.user_data = app;

    app->is_copying = TRUE;
    app->quit_after_copy = FALSE;
    set_controls_sensitive(app, FALSE);
    set_mount(app, NULL);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    gtk_label_set_text(GTK_LABEL(app->lbl_status), "Preparing…");
    gtk_label_set_text(GTK_LABEL(app->lbl_file), "");
    gtk_label_set_text(GTK_LABEL(app->lbl_speed), "");

    g_thread_unref(g_thread_new("copy_thread", copy_thread_func, job));
}

static void on_confirm_conflict_choose(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = user_data;

    GError *error = NULL;
    int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source_object), res, &error);
    if (error) {
        g_error_free(error);
        return;
    }
    if (app->is_copying)
        return;

    if (button == 0)
        start_copy(app, RC_CONFLICT_OVERWRITE);
    else if (button == 1)
        start_copy(app, RC_CONFLICT_SKIP);
}

static void on_start_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = user_data;

    if (app->is_copying)
        return;
    if (app->sources->len == 0 || !app->dst_dir_path) {
        gtk_label_set_text(GTK_LABEL(app->lbl_status), "Add files or folders and select a target folder first");
        return;
    }

    // Ask once, up front, if anything with the same name is already there.
    // (Same-file and type clashes are rejected by the engine regardless.)
    guint conflicts = 0;
    char *first = NULL;
    for (guint i = 0; i < app->sources->len; i++) {
        char *name = g_path_get_basename(app->sources->pdata[i]);
        char *dst = g_build_filename(app->dst_dir_path, name, NULL);
        if (g_file_test(dst, G_FILE_TEST_EXISTS)) {
            if (!first)
                first = g_strdup(name);
            conflicts++;
        }
        g_free(dst);
        g_free(name);
    }

    if (conflicts == 0) {
        start_copy(app, RC_CONFLICT_FAIL);
        return;
    }

    GtkAlertDialog *dialog = conflicts == 1
        ? gtk_alert_dialog_new("“%s” already exists in the target folder", first)
        : gtk_alert_dialog_new("%u items already exist in the target folder", conflicts);
    gtk_alert_dialog_set_detail(dialog,
        "Replace the existing files, or skip them and copy only what is new? "
        "Folders are merged either way.");
    const char *buttons[] = {"Replace", "Skip Existing", "Cancel", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 2);
    gtk_alert_dialog_set_default_button(dialog, 2);
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(app->window), NULL, on_confirm_conflict_choose, app);
    g_object_unref(dialog);
    g_free(first);
}

static void request_cancel(AppWidgets *app) {
    g_cancellable_cancel(app->cancellable);
    gtk_widget_set_sensitive(app->btn_cancel, FALSE);
    gtk_label_set_text(GTK_LABEL(app->lbl_status), "Cancelling…");
}

static void on_cancel_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = user_data;
    if (app->is_copying)
        request_cancel(app);
}

// ---- safely remove ---------------------------------------------------------

static void on_eject_done(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = user_data;
    GMount *mount = G_MOUNT(source_object);
    GError *error = NULL;
    char *name = g_mount_get_name(mount);

    gboolean ok = g_object_get_data(G_OBJECT(mount), "rc-eject")
        ? g_mount_eject_with_operation_finish(mount, res, &error)
        : g_mount_unmount_with_operation_finish(mount, res, &error);

    if (ok) {
        char *text = g_strdup_printf("“%s” can be unplugged now", name);
        gtk_label_set_text(GTK_LABEL(app->lbl_status), text);
        g_free(text);
        if (app->mount == mount)
            set_dst_dir(app, NULL);
    } else {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
            char *text = g_strdup_printf("Could not remove “%s”: %s", name, error->message);
            gtk_label_set_text(GTK_LABEL(app->lbl_status), text);
            g_free(text);
        }
        gtk_widget_set_sensitive(app->btn_eject, TRUE);
        g_error_free(error);
    }
    g_free(name);
    g_object_unref(mount);
}

static void on_eject_clicked(GtkWidget *btn, gpointer user_data) {
    (void)btn;
    AppWidgets *app = user_data;
    if (!app->mount || app->is_copying)
        return;

    GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(app->window));
    GMount *mount = g_object_ref(app->mount);
    gtk_widget_set_sensitive(app->btn_eject, FALSE);
    gtk_label_set_text(GTK_LABEL(app->lbl_status), "Removing drive…");

    // Eject powers the stick off where possible; otherwise just unmount.
    if (g_mount_can_eject(mount)) {
        g_object_set_data(G_OBJECT(mount), "rc-eject", GINT_TO_POINTER(1));
        g_mount_eject_with_operation(mount, G_MOUNT_UNMOUNT_NONE, op, NULL, on_eject_done, app);
    } else {
        g_object_set_data(G_OBJECT(mount), "rc-eject", NULL);
        g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, op, NULL, on_eject_done, app);
    }
    g_object_unref(op);
}

// ---- window ----------------------------------------------------------------

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

static GtkWidget *section_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "heading");
    return label;
}

static AppWidgets *create_window(GtkApplication *gtk_app) {
    AppWidgets *app = g_new0(AppWidgets, 1);
    app->gtk_app = gtk_app;
    app->sources = g_ptr_array_new_with_free_func(g_free);

    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "RealCopy");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 560, 520);
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "realcopy");

    g_signal_connect(app->window, "close-request", G_CALLBACK(on_close_request), app);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(app->window), vbox);

    // Sources
    gtk_box_append(GTK_BOX(vbox), section_label("Copy these"));

    app->src_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->src_list), GTK_SELECTION_NONE);
    GtkWidget *placeholder = gtk_label_new("Drop files or folders here, or use the buttons below");
    gtk_widget_add_css_class(placeholder, "dim-label");
    gtk_widget_set_margin_top(placeholder, 24);
    gtk_widget_set_margin_bottom(placeholder, 24);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(app->src_list), placeholder);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app->src_list);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_widget_set_size_request(scroller, -1, 120);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroller);

    GtkWidget *src_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->btn_add_files = gtk_button_new_with_label("Add Files…");
    g_signal_connect(app->btn_add_files, "clicked", G_CALLBACK(on_add_files_clicked), app);
    gtk_box_append(GTK_BOX(src_buttons), app->btn_add_files);
    app->btn_add_folders = gtk_button_new_with_label("Add Folders…");
    g_signal_connect(app->btn_add_folders, "clicked", G_CALLBACK(on_add_folders_clicked), app);
    gtk_box_append(GTK_BOX(src_buttons), app->btn_add_folders);
    app->btn_clear = gtk_button_new_with_label("Clear");
    gtk_widget_set_hexpand(app->btn_clear, TRUE);
    gtk_widget_set_halign(app->btn_clear, GTK_ALIGN_END);
    g_signal_connect(app->btn_clear, "clicked", G_CALLBACK(on_clear_clicked), app);
    gtk_box_append(GTK_BOX(src_buttons), app->btn_clear);
    gtk_box_append(GTK_BOX(vbox), src_buttons);

    // Target
    gtk_box_append(GTK_BOX(vbox), section_label("To this folder"));
    app->btn_dst_dir = gtk_button_new_with_label("Select Target Folder");
    g_signal_connect(app->btn_dst_dir, "clicked", G_CALLBACK(on_select_dst_dir_clicked), app);
    gtk_box_append(GTK_BOX(vbox), app->btn_dst_dir);

    app->chk_verify = gtk_check_button_new_with_label("Verify after copying (reads everything back from the drive)");
    gtk_box_append(GTK_BOX(vbox), app->chk_verify);

    // Actions
    app->btn_start = gtk_button_new_with_label("Copy");
    gtk_widget_add_css_class(app->btn_start, "suggested-action");
    g_signal_connect(app->btn_start, "clicked", G_CALLBACK(on_start_clicked), app);
    gtk_box_append(GTK_BOX(vbox), app->btn_start);

    app->btn_cancel = gtk_button_new_with_label("Cancel Copy");
    g_signal_connect(app->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), app);
    gtk_widget_set_visible(app->btn_cancel, FALSE);
    gtk_box_append(GTK_BOX(vbox), app->btn_cancel);

    // Progress
    app->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress_bar), TRUE);
    gtk_box_append(GTK_BOX(vbox), app->progress_bar);

    app->lbl_status = gtk_label_new("Ready");
    gtk_label_set_wrap(GTK_LABEL(app->lbl_status), TRUE);
    gtk_label_set_justify(GTK_LABEL(app->lbl_status), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(vbox), app->lbl_status);

    app->lbl_file = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(app->lbl_file), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(app->lbl_file, "dim-label");
    gtk_box_append(GTK_BOX(vbox), app->lbl_file);

    app->lbl_speed = gtk_label_new("");
    gtk_box_append(GTK_BOX(vbox), app->lbl_speed);

    app->btn_eject = gtk_button_new_with_label("Safely Remove Drive");
    g_signal_connect(app->btn_eject, "clicked", G_CALLBACK(on_eject_clicked), app);
    gtk_widget_set_visible(app->btn_eject, FALSE);
    gtk_box_append(GTK_BOX(vbox), app->btn_eject);

    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), app);
    gtk_widget_add_controller(app->window, GTK_EVENT_CONTROLLER(drop));

    update_source_buttons(app);
    g_object_set_data(G_OBJECT(gtk_app), "rc-app", app);
    return app;
}

static AppWidgets *get_app(GtkApplication *gtk_app) {
    AppWidgets *app = g_object_get_data(G_OBJECT(gtk_app), "rc-app");
    return app ? app : create_window(gtk_app);
}

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
    (void)user_data;
    gtk_window_present(GTK_WINDOW(get_app(gtk_app)->window));
}

// `realcopy-gui FILE...` or "Open With" from a file manager: add the files
// to the list.
static void on_open(GApplication *gapp, GFile **files, int n_files, const char *hint, gpointer user_data) {
    (void)hint; (void)user_data;
    AppWidgets *app = get_app(GTK_APPLICATION(gapp));
    if (!app->is_copying)
        for (int i = 0; i < n_files; i++)
            add_source(app, files[i]);
    gtk_window_present(GTK_WINDOW(app->window));
}

int main(int argc, char **argv) {
    GtkApplication *gtk_app = gtk_application_new("com.realcopy.app", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(gtk_app, "open", G_CALLBACK(on_open), NULL);
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);
    return status;
}
