# Save-name identification

The shared [built-in table](../data/save-names.txt) has 118 exact-name or prefix
patterns covering 77 game titles. It is compiled into the boot ISO and desktop
app. It identifies files; it does not describe save layouts or add cheat editors.
Unknown filenames are preserved and searchable.

The entries come from literal `/nvram/` paths in game programs on Opera-format
ISO images. [Source disc identifiers](../data/save-name-sources.json) record
which images contained each path. No game programs or disc contents are included
in this repository. Generic numbered names, obvious debug/scratch files, and
ambiguous short prefixes were omitted from the built-in fallback. Dynamically
constructed filenames and other regions may still be missing. A shared name may
list more than one game; identification by filename alone cannot distinguish them.

`=FILE<TAB>Game title` is an exact match; `*PREFIX<TAB>Game title` is a prefix.
Matching ignores ASCII case. The console selects learned exact names first,
then exact disc names, then exact built-in names, then the longest matching
prefix (disc names win ties). This preserves names learned from actual launches
and library-specific names while providing a fallback on any drive.

The ISO scanner used by the desktop app and `tools/index-media.py` recognizes
uppercase and lowercase paths, spaces, brackets and apostrophes. Its per-disc
cache is versioned so the expanded scanner revisits old results once. It scans
program-size files, not arbitrary large data files, and does not inspect CUE/BIN
images. File patterns found by scanning are hints, not proof that a game still
uses a particular file at runtime.

After changing the table, run `python3 tools/build-save-names.py` to regenerate
`src/save_name_db.h`. Builds check that the generated header matches the shared
table. Both save-name lookup tests and the scanner tests cover lowercase paths,
bracketed names and prefix matching.
