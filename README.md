# RealCopy

On Linux, copying a file to a USB stick often looks finished almost
instantly: the data has only gone into RAM and is still being written to the
stick in the background. Unplug it then, and the file is corrupt or missing.

RealCopy copies files and folders and **only reports success once the data is
really on the drive**. Its progress bar and speed show what has actually
reached the stick, not what is sitting in memory.

## Features

- Files and whole folders, several at once; drag and drop onto the window
- Honest progress, speed and time remaining
- Clear errors instead of a false "done": full drive, the FAT32 4 GiB limit,
  a pulled-out drive. Space and FAT32 limits are checked before anything is written
- Optional **verify**: reads everything back from the drive and compares it
- Asks before replacing existing files (replace or skip)
- Cancel at any time; the half-copied file is removed
- **Safely Remove** button once the copy is done
- `realcopy` command-line tool with the same engine

## Usage

Graphical: start **RealCopy** from your application menu, or

    realcopy-gui [FILE...]

Command line:

    realcopy [OPTION]... SOURCE... TARGET_DIR

      -f, --overwrite       replace files that already exist in the target
      -n, --skip-existing   skip files that already exist in the target
      -c, --verify          read every file back from the drive and compare it
      -q, --quiet           only print errors

By default an existing file in the target is an error, so nothing is replaced
by accident. Exit status: 0 success, 1 error, 2 usage error, 130 cancelled.

## Building

Needs GTK 4 and GLib (`gtk4` on Arch, `libgtk-4-dev` on Debian/Ubuntu).

    make
    make check                 # end-to-end tests of the copy engine
    sudo make install          # PREFIX=/usr/local by default

Arch Linux users can install the `realcopy` package from the AUR.

## How it works

Each file is written in 4 MB chunks. Writeback of every chunk is started
right away with `sync_file_range()`, and the program waits for the previous
chunk to reach the device before counting it as done. At most two chunks are
ever in flight, so progress is honest while reading and writing overlap. Used
pages are dropped from the page cache so a big copy doesn't push everything
else out of memory. Space is reserved up front with `fallocate()`.

At the end `syncfs()` flushes the directory entries, the FAT and the drive's
write cache. Only then does RealCopy say it's done. Verification reads the
files back with `O_DIRECT`, bypassing the page cache.

## License

MIT, see [LICENSE](LICENSE).
