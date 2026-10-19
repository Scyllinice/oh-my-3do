# Diagnostics and recovery

Use **L+R → Diagnostics** to inspect the connection and last media operation.
Support-file paths below are relative to `.ohmy3do/` at the USB root.

L+R → Diagnostics has five pages (Left/Right): reported identity/revision and
raw `93` reply; active settings and session results; XBUS replies, command
count, last fault, timeout and navigation state; a preview loading breakdown;
and the last requested USB filename with media index counts. Use the preview
page after a slow preview to distinguish reading from checks, folder restoration
and audio setup. The filename remains visible after a timeout, when export is
unavailable. Viewing diagnostics sends no
commands and remains available after a transport failure. USB write-check
results appear only after the automatic check has run.

After a failed folder restoration, **B** attempts to return to the remembered
USB root if the ODE is still responding with complete replies. Recovery walks
fresh parent entries, selects the original storage volume, and checks two fresh
root listings against the root recorded earlier in this session. It resumes
browsing only when those checks pass. Covers, previews and automatic position
saving remain off until restart; settings on USB are unchanged. A timeout,
partial exchange, unknown root or failed recovery requires a console power cycle.

To test this on a console without provoking a crash, enter a nested game folder,
open **L+R → Diagnostics**, and press **X: Test return**. This runs the same recovery
path and should return to the USB root with `MEDIA OFF`. Check that folders and
Back work, then restart to re-enable media. This validates the recovery path on
your ODE, but cannot prove recovery from a genuinely hung firmware. Host tests
inject rejected and stuck navigation, wrong roots, malformed replies and I/O
failures, using the reference firmware's directory implementation as well as
the production menu controller.

**C** exports a verified `.ohmy3do/diagnostics.txt`. The report includes
the raw identity reply, command/reply and retained fault, active settings, queue
state, save results, and position/history results. It snapshots these values
before export I/O changes the bus counters. Export is unavailable after a
transport timeout or failed directory restoration, with startup bypass, or at
the virtual volume selector; viewing the screens still works. Result codes are
`1` for success, `0` for not attempted, and `-1` for failure. Export replaces
the previous report. It is padded to a fixed 4 KiB, so blank lines at the end
are expected.

`LOCODE 1.1.0` matches the reference source but is hard-coded there, along with
boot flags. It does not identify the exact firmware binary, board or flash size.
USB file operations are gated to `LOCODE`; that gate is not proof that every
binary using the name behaves identically. Hold X at boot for recovery without
these optional file operations. No firmware dumping or flashing is performed.

The reference firmware opens files with read/write access and creates missing
files even on reads. Reads can misreport EOF and writes can misreport short
writes. Text buffers are fully cleared before reading; binary inputs are bounded
and checksummed. Verified writes poison every requested readback byte with the
complement of its expected value before comparing actual data. No unsupported
create/seek/truncate/rename commands are used. Read-only USB storage can reject
these operations. All XBUS requests have a three-second timeout; a partial FIFO
exchange requires console restart rather than blind retries.

Other limits: 2,048 entries per directory, 32 navigation levels, ASCII natural
sorting/case folding, and `?` display for unsupported characters. Queue paths
are bounded. No in-game overlay, automatic return-to-menu, NVRAM
formatting, defragmentation, save deletion or Unicode collation is provided.
Image formats visible to the browser depend on the installed firmware.

[Project overview](../README.md)
