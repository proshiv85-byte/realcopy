#define _GNU_SOURCE
#include "copy.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define CHUNK_SIZE (4 * 1024 * 1024) // 4 MB
#define DIRECT_IO_ALIGN 4096
#define PROGRESS_INTERVAL_US (100 * 1000)
#define SPEED_WINDOW_US (3 * G_USEC_PER_SEC)
#define N_SAMPLES 64
#define FAT_MAX_FILE_SIZE 0xFFFFFFFFULL

typedef struct {
    char *src;      // absolute source path
    char *rel;      // path relative to the target directory
    gboolean is_dir;
    gboolean skip;  // file already exists and policy says skip
    guint64 size;
} Entry;

typedef struct {
    gint64 time;
    guint64 bytes;
} Sample;

typedef struct {
    const RcOptions *opt;
    RcSummary *summary;
    GPtrArray *entries;

    RcPhase phase;
    const char *current;
    guint64 work_total;
    guint64 work_done;
    guint files_total;
    guint files_done;

    gint64 start_time;
    gint64 last_report;
    Sample samples[N_SAMPLES];
    guint n_samples;
    guint sample_head;

    // sync_file_range() isn't supported by the target; fall back to
    // fdatasync() after every chunk.
    gboolean use_fdatasync;
    void *buffer; // CHUNK_SIZE, aligned for O_DIRECT
} Ctx;

static void entry_free(gpointer data) {
    Entry *e = data;
    g_free(e->src);
    g_free(e->rel);
    g_free(e);
}

void rc_format_duration(double seconds, char *buffer, gsize size) {
    if (seconds < 0) seconds = 0;

    int total_s = (int)seconds;
    int hours = total_s / 3600;
    int minutes = (total_s % 3600) / 60;
    int secs = total_s % 60;

    if (hours > 0)
        g_snprintf(buffer, size, "%d:%02d:%02d", hours, minutes, secs);
    else
        g_snprintf(buffer, size, "%d:%02d", minutes, secs);
}

char *rc_summary_to_string(const RcSummary *s) {
    GString *out = g_string_new(NULL);
    char *size = g_format_size(s->bytes_copied);
    char duration[32];
    rc_format_duration(s->elapsed, duration, sizeof(duration));

    g_string_append_printf(out, "Copied %u file%s (%s) in %s", s->files_copied,
                           s->files_copied == 1 ? "" : "s", size, duration);
    if (s->elapsed >= 1 && s->bytes_copied > 0) {
        char *speed = g_format_size((guint64)(s->bytes_copied / s->elapsed));
        g_string_append_printf(out, ", average %s/s", speed);
        g_free(speed);
    }
    g_string_append(out, s->verified ? ". Verified — the data on the drive matches."
                                     : ". The data is on the drive.");
    if (s->files_skipped)
        g_string_append_printf(out, " Skipped %u existing file%s.", s->files_skipped,
                               s->files_skipped == 1 ? "" : "s");
    if (s->special_skipped)
        g_string_append_printf(out, " Left out %u symlink%s or special file%s.",
                               s->special_skipped, s->special_skipped == 1 ? "" : "s",
                               s->special_skipped == 1 ? "" : "s");
    g_free(size);
    return g_string_free(out, FALSE);
}

// Sets @error from a failed syscall; call with errno intact.
G_GNUC_PRINTF(3, 4)
static gboolean fail_errno(GError **error, int err, const char *format, ...) {
    const char *hint = "";
    if (err == ENOSPC)
        hint = " (the drive is full)";
    else if (err == EFBIG)
        hint = " (file too large for this filesystem — FAT32 is limited to 4 GiB)";
    else if (err == EIO || err == ENODEV || err == ENXIO)
        hint = " (was the drive removed?)";

    va_list args;
    va_start(args, format);
    char *what = g_strdup_vprintf(format, args);
    va_end(args);

    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "%s: %s%s",
                what, g_strerror(err), hint);
    g_free(what);
    return FALSE;
}

static gboolean same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
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

// ---- progress --------------------------------------------------------------

static void report(Ctx *c, gboolean force) {
    gint64 now = g_get_monotonic_time();
    if (!force && now - c->last_report < PROGRESS_INTERVAL_US)
        return;
    c->last_report = now;

    c->samples[c->sample_head] = (Sample){now, c->work_done};
    c->sample_head = (c->sample_head + 1) % N_SAMPLES;
    if (c->n_samples < N_SAMPLES) c->n_samples++;

    // Speed over the last few seconds, so it follows the drive when its
    // write cache fills up and it slows down.
    double speed = 0;
    for (guint i = 0; i < c->n_samples; i++) {
        const Sample *s = &c->samples[(c->sample_head + N_SAMPLES - c->n_samples + i) % N_SAMPLES];
        if (now - s->time <= SPEED_WINDOW_US) {
            if (now > s->time)
                speed = (double)(c->work_done - s->bytes) / ((double)(now - s->time) / G_USEC_PER_SEC);
            break;
        }
    }

    if (!c->opt->progress)
        return;

    RcProgress p = {
        .phase = c->phase,
        .current_file = c->current,
        .done_bytes = c->work_done,
        .total_bytes = c->work_total,
        .files_done = c->files_done,
        .files_total = c->files_total,
        .speed = speed,
        .eta = speed > 0 ? (double)(c->work_total - c->work_done) / speed : -1,
    };
    c->opt->progress(&p, c->opt->user_data);
}

static void set_phase(Ctx *c, RcPhase phase, const char *current) {
    c->phase = phase;
    c->current = current;
    report(c, TRUE);
}

static void account(Ctx *c, guint64 bytes) {
    c->work_done += bytes;
    report(c, FALSE);
}

// ---- scanning --------------------------------------------------------------

static int compare_names(gconstpointer a, gconstpointer b) {
    return g_strcmp0(*(const char *const *)a, *(const char *const *)b);
}

static void add_entry(Ctx *c, const char *src, const char *rel, const struct stat *st) {
    Entry *e = g_new0(Entry, 1);
    e->src = g_strdup(src);
    e->rel = g_strdup(rel);
    e->is_dir = S_ISDIR(st->st_mode);
    e->size = e->is_dir ? 0 : (guint64)st->st_size;
    g_ptr_array_add(c->entries, e);
}

static gboolean scan_dir(Ctx *c, const char *src, const char *rel, GError **error) {
    DIR *dir = opendir(src);
    if (!dir)
        return fail_errno(error, errno, "cannot read folder “%s”", src);

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    struct dirent *de;
    errno = 0;
    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            g_ptr_array_add(names, g_strdup(de->d_name));
        errno = 0;
    }
    if (errno) {
        int err = errno;
        closedir(dir);
        g_ptr_array_unref(names);
        return fail_errno(error, err, "cannot read folder “%s”", src);
    }
    g_ptr_array_sort(names, compare_names);

    gboolean ok = TRUE;
    for (guint i = 0; ok && i < names->len; i++) {
        const char *name = names->pdata[i];
        struct stat st;

        if (g_cancellable_set_error_if_cancelled(c->opt->cancellable, error)) {
            ok = FALSE;
            break;
        }
        if (fstatat(dirfd(dir), name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            ok = fail_errno(error, errno, "cannot read “%s/%s”", src, name);
            break;
        }

        char *child_src = g_build_filename(src, name, NULL);
        char *child_rel = g_build_filename(rel, name, NULL);
        if (S_ISDIR(st.st_mode)) {
            add_entry(c, child_src, child_rel, &st);
            ok = scan_dir(c, child_src, child_rel, error);
        } else if (S_ISREG(st.st_mode)) {
            add_entry(c, child_src, child_rel, &st);
        } else {
            // Symlinks, sockets, fifos and devices can't be stored on
            // FAT/exFAT drives; leave them out and say so in the summary.
            c->summary->special_skipped++;
        }
        g_free(child_src);
        g_free(child_rel);
    }

    closedir(dir);
    g_ptr_array_unref(names);
    return ok;
}

static gboolean path_is_inside(const char *path, const char *dir) {
    char *real_path = realpath(path, NULL);
    char *real_dir = realpath(dir, NULL);
    gboolean inside = FALSE;

    if (real_path && real_dir) {
        size_t len = strlen(real_dir);
        inside = strcmp(real_path, real_dir) == 0 ||
                 (strncmp(real_path, real_dir, len) == 0 &&
                  (real_path[len] == '/' || strcmp(real_dir, "/") == 0));
    }
    free(real_path);
    free(real_dir);
    return inside;
}

static gboolean scan_sources(Ctx *c, GError **error) {
    const RcOptions *opt = c->opt;
    GHashTable *top_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean ok = TRUE;

    for (const char *const *s = opt->sources; ok && *s; s++) {
        char *src = g_canonicalize_filename(*s, NULL);
        char *name = g_path_get_basename(src);
        struct stat st;

        if (strcmp(name, "/") == 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "cannot copy the root folder");
            ok = FALSE;
        } else if (stat(src, &st) < 0) {
            ok = fail_errno(error, errno, "cannot read “%s”", src);
        } else if (g_hash_table_contains(top_names, name)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "two of the selected items are named “%s”", name);
            ok = FALSE;
        } else if (S_ISDIR(st.st_mode)) {
            if (path_is_inside(opt->dest_dir, src)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "cannot copy folder “%s” into itself", src);
                ok = FALSE;
            } else {
                add_entry(c, src, name, &st);
                ok = scan_dir(c, src, name, error);
            }
        } else if (S_ISREG(st.st_mode)) {
            add_entry(c, src, name, &st);
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                        "“%s” is not a regular file or folder", src);
            ok = FALSE;
        }

        if (ok)
            g_hash_table_add(top_names, g_strdup(name));
        g_free(src);
        g_free(name);
    }

    g_hash_table_unref(top_names);
    return ok;
}

// Checks every entry against what is already in the target, and that the
// target has room for everything, before anything is written.
static gboolean check_target(Ctx *c, GError **error) {
    const RcOptions *opt = c->opt;
    struct stat dst_dir_st;

    if (stat(opt->dest_dir, &dst_dir_st) < 0)
        return fail_errno(error, errno, "cannot open target folder “%s”", opt->dest_dir);
    if (!S_ISDIR(dst_dir_st.st_mode)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                    "target “%s” is not a folder", opt->dest_dir);
        return FALSE;
    }
    if (access(opt->dest_dir, W_OK) < 0)
        return fail_errno(error, errno, "cannot write to target folder “%s”", opt->dest_dir);

    struct statvfs vfs;
    struct statfs fs;
    if (statvfs(opt->dest_dir, &vfs) < 0 || statfs(opt->dest_dir, &fs) < 0)
        return fail_errno(error, errno, "cannot query target drive");
    gboolean is_fat = fs.f_type == MSDOS_SUPER_MAGIC;
    guint64 block = vfs.f_bsize ? vfs.f_bsize : 4096;

    guint64 needed = 0, reclaimed = 0;
    for (guint i = 0; i < c->entries->len; i++) {
        Entry *e = c->entries->pdata[i];
        char *dst = g_build_filename(opt->dest_dir, e->rel, NULL);
        struct stat src_st, dst_st;
        gboolean ok = TRUE;

        if (lstat(dst, &dst_st) == 0) {
            if (e->is_dir) {
                if (!S_ISDIR(dst_st.st_mode)) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                "“%s” already exists in the target and is not a folder", e->rel);
                    ok = FALSE;
                }
            } else if (!S_ISREG(dst_st.st_mode)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "“%s” already exists in the target and is not a regular file", e->rel);
                ok = FALSE;
            } else if (stat(e->src, &src_st) == 0 && same_file(&src_st, &dst_st)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "source and target are the same file: “%s”", e->src);
                ok = FALSE;
            } else if (opt->conflict == RC_CONFLICT_SKIP) {
                e->skip = TRUE;
            } else if (opt->conflict == RC_CONFLICT_FAIL) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "“%s” already exists in the target", e->rel);
                ok = FALSE;
            } else {
                reclaimed += (guint64)dst_st.st_blocks * 512;
            }
        } else if (errno != ENOENT) {
            ok = fail_errno(error, errno, "cannot check “%s”", dst);
        }
        g_free(dst);
        if (!ok)
            return FALSE;

        if (e->is_dir || e->skip)
            continue;

        if (is_fat && e->size > FAT_MAX_FILE_SIZE) {
            char *size = g_format_size(e->size);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "“%s” is %s, but the target drive uses FAT32, which can't store "
                        "files of 4 GiB or more. Reformat the drive as exFAT to copy it.",
                        e->rel, size);
            g_free(size);
            return FALSE;
        }

        needed += (e->size + block - 1) / block * block;
        c->files_total++;
        c->work_total += e->size;
    }

    guint64 available = (guint64)vfs.f_bavail * vfs.f_frsize + reclaimed;
    if (needed > available) {
        char *need_str = g_format_size(needed);
        char *free_str = g_format_size(available);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                    "not enough space on the target drive: need %s, only %s free",
                    need_str, free_str);
        g_free(need_str);
        g_free(free_str);
        return FALSE;
    }

    if (opt->verify)
        c->work_total *= 2;
    return TRUE;
}

// ---- copying ---------------------------------------------------------------

static void drop_cache(int fd, off_t offset, off_t len) {
    posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED);
}

// Reads @path back from the drive (bypassing the page cache) and compares
// its checksum with @expected.
static gboolean verify_file(Ctx *c, const char *path, const char *rel,
                            const char *expected, GError **error) {
    set_phase(c, RC_PHASE_VERIFYING, rel);

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECT);
    gboolean direct = fd >= 0;
    if (fd < 0 && errno == EINVAL)
        fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fail_errno(error, errno, "cannot reopen “%s” to verify it", rel);
    if (!direct)
        drop_cache(fd, 0, 0);

    GChecksum *sum = g_checksum_new(G_CHECKSUM_MD5);
    gboolean ok = TRUE;
    while (TRUE) {
        if (g_cancellable_set_error_if_cancelled(c->opt->cancellable, error)) {
            ok = FALSE;
            break;
        }
        ssize_t n = read_retry(fd, c->buffer, CHUNK_SIZE);
        if (n < 0 && errno == EINVAL && direct) {
            // Filesystem accepted O_DIRECT at open but not for reads.
            int plain = open(path, O_RDONLY | O_CLOEXEC);
            if (plain < 0) {
                ok = fail_errno(error, errno, "cannot reopen “%s” to verify it", rel);
                break;
            }
            off_t pos = lseek(fd, 0, SEEK_CUR);
            close(fd);
            fd = plain;
            direct = FALSE;
            drop_cache(fd, 0, 0);
            lseek(fd, pos, SEEK_SET);
            continue;
        }
        if (n < 0) {
            ok = fail_errno(error, errno, "reading “%s” back from the drive failed", rel);
            break;
        }
        if (n == 0) break;
        g_checksum_update(sum, c->buffer, n);
        account(c, n);
    }
    close(fd);

    if (ok && strcmp(g_checksum_get_string(sum), expected) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "verification failed for “%s”: the data read back from the drive "
                    "differs from the source. The drive may be faulty or fake.", rel);
        ok = FALSE;
    }
    g_checksum_free(sum);
    return ok;
}

static gboolean copy_file(Ctx *c, const Entry *e, GError **error) {
    const char *rel = e->rel;
    char *dst = g_build_filename(c->opt->dest_dir, rel, NULL);
    GChecksum *sum = c->opt->verify ? g_checksum_new(G_CHECKSUM_MD5) : NULL;
    gboolean created = FALSE, ok = FALSE;
    int dst_fd = -1;
    guint64 file_done_start = c->work_done;

    set_phase(c, RC_PHASE_COPYING, rel);

    int src_fd = open(e->src, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        fail_errno(error, errno, "cannot open “%s”", e->src);
        goto out;
    }
    struct stat src_st;
    if (fstat(src_fd, &src_st) < 0) {
        fail_errno(error, errno, "cannot read “%s”", e->src);
        goto out;
    }

    // Open without O_TRUNC so we can make sure the target isn't the source
    // itself before throwing away its contents.
    dst_fd = open(dst, O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (dst_fd < 0) {
        fail_errno(error, errno, "cannot create “%s”", rel);
        goto out;
    }
    struct stat dst_st;
    if (fstat(dst_fd, &dst_st) < 0) {
        fail_errno(error, errno, "cannot read “%s”", rel);
        goto out;
    }
    if (same_file(&src_st, &dst_st)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "source and target are the same file: “%s”", e->src);
        goto out;
    }
    created = TRUE;
    if (ftruncate(dst_fd, 0) < 0) {
        fail_errno(error, errno, "cannot truncate “%s”", rel);
        goto out;
    }

    // Reserve the space up front: the file ends up contiguous and a full
    // drive fails now instead of at 90%. KEEP_SIZE because on FAT a plain
    // fallocate zero-fills, which would write the file twice.
    if (src_st.st_size > 0 &&
        fallocate(dst_fd, FALLOC_FL_KEEP_SIZE, 0, src_st.st_size) < 0 &&
        (errno == ENOSPC || errno == EFBIG)) {
        fail_errno(error, errno, "cannot allocate space for “%s”", rel);
        goto out;
    }

    posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    // Pipeline: start writeback of each chunk as soon as it is written, and
    // only wait for the previous one, so reading and flushing overlap. At
    // most two chunks are ever unflushed, so progress stays honest.
    off_t offset = 0, pending_off = 0;
    size_t pending_len = 0;
    while (TRUE) {
        if (g_cancellable_set_error_if_cancelled(c->opt->cancellable, error))
            goto out;

        ssize_t n = read_retry(src_fd, c->buffer, CHUNK_SIZE);
        if (n < 0) {
            fail_errno(error, errno, "reading “%s” failed", e->src);
            goto out;
        }
        if (n == 0) break;

        if (sum)
            g_checksum_update(sum, c->buffer, n);

        if (write_all(dst_fd, c->buffer, (size_t)n) < 0) {
            fail_errno(error, errno, "writing “%s” failed", rel);
            goto out;
        }

        if (!c->use_fdatasync &&
            sync_file_range(dst_fd, offset, n, SYNC_FILE_RANGE_WRITE) < 0) {
            if (errno == ENOSYS || errno == EINVAL || errno == ESPIPE || errno == EOPNOTSUPP) {
                c->use_fdatasync = TRUE;
            } else {
                fail_errno(error, errno, "writing “%s” failed", rel);
                goto out;
            }
        }

        if (c->use_fdatasync) {
            if (fdatasync(dst_fd) < 0) {
                fail_errno(error, errno, "flushing “%s” to the drive failed", rel);
                goto out;
            }
            drop_cache(dst_fd, 0, offset + n);
            account(c, pending_len + n);
            pending_len = 0;
        } else {
            if (pending_len) {
                if (sync_file_range(dst_fd, pending_off, pending_len,
                                    SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                                    SYNC_FILE_RANGE_WAIT_AFTER) < 0) {
                    fail_errno(error, errno, "flushing “%s” to the drive failed", rel);
                    goto out;
                }
                drop_cache(dst_fd, pending_off, pending_len);
                account(c, pending_len);
            }
            pending_off = offset;
            pending_len = n;
        }

        drop_cache(src_fd, offset, n);
        offset += n;
    }

    if (pending_len) {
        if (sync_file_range(dst_fd, pending_off, pending_len,
                            SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                            SYNC_FILE_RANGE_WAIT_AFTER) < 0) {
            fail_errno(error, errno, "flushing “%s” to the drive failed", rel);
            goto out;
        }
        drop_cache(dst_fd, pending_off, pending_len);
        account(c, pending_len);
    }

    // The source shrank while we copied it: release the extra reservation.
    if (offset < src_st.st_size && ftruncate(dst_fd, offset) < 0) {
        fail_errno(error, errno, "cannot truncate “%s”", rel);
        goto out;
    }

    // Keep the modification time, like a file manager copy does.
    struct timespec times[2] = {src_st.st_atim, src_st.st_mtim};
    futimens(dst_fd, times);

    int rc = close(dst_fd);
    dst_fd = -1;
    if (rc < 0) {
        fail_errno(error, errno, "closing “%s” failed", rel);
        goto out;
    }

    // Account for a source that changed size since it was scanned.
    guint64 copied = c->work_done - file_done_start;
    if (copied != e->size) {
        guint64 passes = c->opt->verify ? 2 : 1;
        c->work_total = c->work_total - e->size * passes + copied * passes;
    }

    if (sum && !verify_file(c, dst, rel, g_checksum_get_string(sum), error))
        goto out;

    c->summary->bytes_copied += copied;
    ok = TRUE;

out:
    if (src_fd >= 0) close(src_fd);
    if (dst_fd >= 0) close(dst_fd);
    if (!ok && created)
        unlink(dst);
    if (sum) g_checksum_free(sum);
    g_free(dst);
    return ok;
}

static gboolean make_dir(Ctx *c, const Entry *e, GError **error) {
    char *dst = g_build_filename(c->opt->dest_dir, e->rel, NULL);
    struct stat st;
    gboolean ok = TRUE;

    if (mkdir(dst, 0777) < 0 && !(errno == EEXIST && stat(dst, &st) == 0 && S_ISDIR(st.st_mode)))
        ok = fail_errno(error, errno, "cannot create folder “%s”", e->rel);
    g_free(dst);
    return ok;
}

// Flushes everything on the target filesystem: file sizes, directory
// entries and the FAT itself, and the drive's own write cache.
static gboolean sync_target(Ctx *c, GError **error) {
    int fd = open(c->opt->dest_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return fail_errno(error, errno, "cannot open target folder");
    int rc = syncfs(fd);
    int err = errno;
    close(fd);
    if (rc < 0)
        return fail_errno(error, err, "flushing the drive failed");
    return TRUE;
}

gboolean rc_copy(const RcOptions *options, RcSummary *summary, GError **error) {
    RcSummary dummy;
    Ctx c = {0};
    c.opt = options;
    c.summary = summary ? summary : &dummy;
    memset(c.summary, 0, sizeof(*c.summary));
    c.entries = g_ptr_array_new_with_free_func(entry_free);
    c.start_time = g_get_monotonic_time();

    gboolean ok = FALSE, wrote = FALSE;

    set_phase(&c, RC_PHASE_SCANNING, NULL);
    if (!scan_sources(&c, error) || !check_target(&c, error))
        goto out;

    if (posix_memalign(&c.buffer, DIRECT_IO_ALIGN, CHUNK_SIZE) != 0) {
        c.buffer = NULL;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "out of memory");
        goto out;
    }

    c.n_samples = 0;
    for (guint i = 0; i < c.entries->len; i++) {
        const Entry *e = c.entries->pdata[i];

        if (g_cancellable_set_error_if_cancelled(options->cancellable, error))
            goto out;

        if (e->is_dir) {
            wrote = TRUE;
            if (!make_dir(&c, e, error))
                goto out;
        } else if (e->skip) {
            c.summary->files_skipped++;
        } else {
            wrote = TRUE;
            if (!copy_file(&c, e, error))
                goto out;
            c.files_done++;
            c.summary->files_copied++;
        }
    }

    set_phase(&c, RC_PHASE_SYNCING, NULL);
    if (!sync_target(&c, error))
        goto out;
    wrote = FALSE;

    c.summary->verified = options->verify;
    ok = TRUE;

out:
    // Make what was completed (and the removal of a partial file) durable
    // even when stopping early.
    if (wrote)
        sync_target(&c, NULL);
    c.summary->elapsed = (double)(g_get_monotonic_time() - c.start_time) / G_USEC_PER_SEC;
    free(c.buffer);
    g_ptr_array_unref(c.entries);
    return ok;
}
