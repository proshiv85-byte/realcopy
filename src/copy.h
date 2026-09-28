// RealCopy copy engine: copies files and folders and only reports success
// once the data has really been written to the target device.
//
// Uses GLib/GIO only (no GTK), so it is shared by the GUI and the CLI.
#pragma once

#include <gio/gio.h>

typedef enum {
    RC_CONFLICT_FAIL,      // an existing target file is an error
    RC_CONFLICT_OVERWRITE, // replace existing target files
    RC_CONFLICT_SKIP,      // leave existing target files alone
} RcConflictPolicy;

typedef enum {
    RC_PHASE_SCANNING,
    RC_PHASE_COPYING,
    RC_PHASE_VERIFYING,
    RC_PHASE_SYNCING,
} RcPhase;

typedef struct {
    RcPhase phase;
    const char *current_file; // path relative to the target directory; may be NULL
    guint64 done_bytes;       // bytes flushed to the device (plus verified bytes)
    guint64 total_bytes;      // total work in bytes (doubled when verifying)
    guint files_done;
    guint files_total;
    double speed;             // bytes per second over the last few seconds
    double eta;               // seconds, < 0 when unknown
} RcProgress;

// Called from the copying thread, at most ~10 times per second.
typedef void (*RcProgressFunc)(const RcProgress *progress, gpointer user_data);

typedef struct {
    const char *const *sources; // NULL-terminated list of files and folders
    const char *dest_dir;
    RcConflictPolicy conflict;
    gboolean verify;
    GCancellable *cancellable;
    RcProgressFunc progress;
    gpointer user_data;
} RcOptions;

typedef struct {
    guint files_copied;
    guint files_skipped;   // skipped because they already existed
    guint special_skipped; // symlinks, sockets, devices... inside folders
    guint64 bytes_copied;
    double elapsed;        // seconds
    gboolean verified;
} RcSummary;

// Runs the whole copy synchronously. Returns FALSE and sets @error on
// failure; cancellation is reported as G_IO_ERROR_CANCELLED. A file that
// failed or was cancelled half-way is removed; files completed before it
// stay (they are complete and flushed).
gboolean rc_copy(const RcOptions *options, RcSummary *summary, GError **error);

// Human-readable one-paragraph summary of a finished copy. Free with g_free().
char *rc_summary_to_string(const RcSummary *summary);

// Formats seconds as m:ss or h:mm:ss into @buffer.
void rc_format_duration(double seconds, char *buffer, gsize size);
