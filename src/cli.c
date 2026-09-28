// realcopy: command-line front end.
//
//   realcopy [OPTION]... SOURCE... TARGET_DIR
#include "copy.h"

#include <glib-unix.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef RC_VERSION
#define RC_VERSION "dev"
#endif

typedef struct {
    RcOptions options;
    GMainLoop *loop;
    gboolean ok;
    RcSummary summary;
    GError *error;

    gboolean quiet;
    gboolean tty;
    char *last_file; // for non-tty output: print each file once
    gboolean line_open;
} Cli;

static const char *phase_name(RcPhase phase) {
    switch (phase) {
    case RC_PHASE_SCANNING: return "Scanning";
    case RC_PHASE_COPYING: return "Copying";
    case RC_PHASE_VERIFYING: return "Verifying";
    case RC_PHASE_SYNCING: return "Finishing";
    }
    return "";
}

static int terminal_width(void) {
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20)
        return ws.ws_col;
    return 80;
}

static void on_progress(const RcProgress *p, gpointer user_data) {
    Cli *cli = user_data;
    if (cli->quiet)
        return;

    if (!cli->tty) {
        // Log files; avoid spamming a log with progress lines.
        if (p->current_file && g_strcmp0(p->current_file, cli->last_file) != 0) {
            fprintf(stderr, "%s %s\n", phase_name(p->phase), p->current_file);
            g_free(cli->last_file);
            cli->last_file = g_strdup(p->current_file);
        }
        return;
    }

    GString *line = g_string_new(NULL);
    if (p->phase == RC_PHASE_SCANNING || p->phase == RC_PHASE_SYNCING) {
        g_string_append_printf(line, "%s…", phase_name(p->phase));
    } else {
        int percent = p->total_bytes ? (int)(100.0 * p->done_bytes / p->total_bytes) : 100;
        char *speed = g_format_size((guint64)p->speed);
        char eta[32] = "--:--";
        if (p->eta >= 0)
            rc_format_duration(p->eta, eta, sizeof(eta));
        g_string_append_printf(line, "[%3d%%] %u/%u files · %s/s · ETA %s · %s %s",
                               percent, MIN(p->files_done + 1, p->files_total), p->files_total,
                               speed, eta, phase_name(p->phase),
                               p->current_file ? p->current_file : "");
        g_free(speed);
    }

    int width = terminal_width() - 1;
    if ((int)g_utf8_strlen(line->str, -1) > width) {
        char *cut = g_utf8_substring(line->str, 0, width - 1);
        g_string_assign(line, cut);
        g_string_append(line, "…");
        g_free(cut);
    }
    fprintf(stderr, "\r\033[K%s", line->str);
    fflush(stderr);
    cli->line_open = TRUE;
    g_string_free(line, TRUE);
}

static gboolean quit_loop(gpointer user_data) {
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

static gpointer copy_thread(gpointer user_data) {
    Cli *cli = user_data;
    cli->ok = rc_copy(&cli->options, &cli->summary, &cli->error);
    // Via an idle source: a direct quit could land before the loop runs.
    g_idle_add(quit_loop, cli->loop);
    return NULL;
}

static gboolean on_signal(gpointer user_data) {
    Cli *cli = user_data;
    if (g_cancellable_is_cancelled(cli->options.cancellable)) {
        // Second Ctrl+C: the user really wants out, even mid-flush.
        _exit(130);
    }
    g_cancellable_cancel(cli->options.cancellable);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");

    gboolean overwrite = FALSE, skip = FALSE, verify = FALSE, quiet = FALSE, version = FALSE;
    char **args = NULL;
    GOptionEntry entries[] = {
        {"overwrite", 'f', 0, G_OPTION_ARG_NONE, &overwrite, "Replace files that already exist in the target", NULL},
        {"skip-existing", 'n', 0, G_OPTION_ARG_NONE, &skip, "Skip files that already exist in the target", NULL},
        {"verify", 'c', 0, G_OPTION_ARG_NONE, &verify, "Read every file back from the drive and compare it", NULL},
        {"quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet, "Only print errors", NULL},
        {"version", 0, 0, G_OPTION_ARG_NONE, &version, "Show the version", NULL},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &args, NULL, "SOURCE... TARGET_DIR"},
        {NULL, 0, 0, 0, NULL, NULL, NULL},
    };

    GOptionContext *ctx = g_option_context_new(NULL);
    g_option_context_set_summary(ctx,
        "Copy files and folders to a drive, and only finish once the data is\n"
        "really written to it — so it is safe to unplug afterwards.");
    g_option_context_add_main_entries(ctx, entries, NULL);

    GError *error = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
        fprintf(stderr, "realcopy: %s\nTry 'realcopy --help'.\n", error->message);
        return 2;
    }
    if (version) {
        printf("realcopy %s\n", RC_VERSION);
        return 0;
    }
    guint n_args = args ? g_strv_length(args) : 0;
    if (n_args < 2) {
        fprintf(stderr, "realcopy: need at least one SOURCE and a TARGET_DIR\nTry 'realcopy --help'.\n");
        return 2;
    }
    if (overwrite && skip) {
        fprintf(stderr, "realcopy: --overwrite and --skip-existing can't be used together\n");
        return 2;
    }
    g_option_context_free(ctx);

    Cli cli = {0};
    cli.quiet = quiet;
    cli.tty = isatty(STDERR_FILENO);
    cli.loop = g_main_loop_new(NULL, FALSE);

    char *dest = args[n_args - 1];
    args[n_args - 1] = NULL;
    cli.options.sources = (const char *const *)args;
    cli.options.dest_dir = dest;
    cli.options.conflict = overwrite ? RC_CONFLICT_OVERWRITE : skip ? RC_CONFLICT_SKIP : RC_CONFLICT_FAIL;
    cli.options.verify = verify;
    cli.options.cancellable = g_cancellable_new();
    cli.options.progress = on_progress;
    cli.options.user_data = &cli;

    g_unix_signal_add(SIGINT, on_signal, &cli);
    g_unix_signal_add(SIGTERM, on_signal, &cli);

    GThread *thread = g_thread_new("copy", copy_thread, &cli);
    g_main_loop_run(cli.loop);
    g_thread_join(thread);

    if (cli.line_open)
        fprintf(stderr, "\r\033[K");

    int status = 0;
    if (cli.ok) {
        if (!quiet) {
            char *text = rc_summary_to_string(&cli.summary);
            fprintf(stderr, "%s\n", text);
            g_free(text);
        }
    } else if (g_error_matches(cli.error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        fprintf(stderr, "realcopy: cancelled — the partly copied file was removed "
                        "(%u file%s completed)\n",
                cli.summary.files_copied, cli.summary.files_copied == 1 ? "" : "s");
        status = 130;
    } else {
        fprintf(stderr, "realcopy: %s\n", cli.error->message);
        if (cli.summary.files_copied)
            fprintf(stderr, "realcopy: completed before the error: %u file%s\n",
                    cli.summary.files_copied, cli.summary.files_copied == 1 ? "" : "s");
        status = 1;
    }

    g_clear_error(&cli.error);
    g_object_unref(cli.options.cancellable);
    g_main_loop_unref(cli.loop);
    g_free(cli.last_file);
    g_free(dest);
    g_strfreev(args);
    return status;
}
