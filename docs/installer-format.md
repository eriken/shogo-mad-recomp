# GOG's Shogo installer

`setup_shogo_-_mobile_armor_division_2.2.14_(19498).exe` is an Inno Setup 5.5.7
(Unicode) installer built by GOG's Galaxy tooling. `engine/inno_setup.cpp` reads it
directly; nothing in it is run. The layout below covers what that installer uses
(the format is also documented by the zlib-licensed
[innoextract](https://github.com/dscharrer/innoextract), which served as the
reference). `shogo --installer-list <setup.exe>` lists what the engine reads, and
`shogo --import <setup.exe> <dir>` does the launcher's import.

All numbers are little endian.

## Finding the data

The setup stub carries a loader table in its resources, found by its 12-byte id:

```
"rDlPtS" CD E6 D7 7B 0B 2A
u32  table version (1)
u32  total size
u32  offset of the compressed setup program
u32  its size
u32  its CRC32
u32  header offset      (the setup header, below)
u32  data offset        (the files' chunks)
u32  CRC32 of the table up to here
```

At the header offset: a 64-byte version string, `Inno Setup Setup Data (5.5.7) (u)`,
then two blocks. Each block is `u32 CRC32`, `u32 stored size`, `u8 compressed`
(the CRC covers the size and the flag), then the stored bytes in pieces of up to
4096 bytes, each led by its own CRC32. Compressed blocks are LZMA1: 5 property bytes,
then a raw stream with no end marker.

## The first block: header and entries

Strings are `u32 byte length` + UTF-16LE text. Version-specific layouts for 5.5.7:

- **Setup header**: 32 strings (app name first), 16 `u32` entry counts (languages,
  messages, permissions, types, components, tasks, directories, files, data entries,
  icons, ini, registry, delete, uninstall delete, run, uninstall run), then fixed
  fields: Windows versions (20 bytes), two colours, alpha format (1), password SHA-1
  (20) and salt (8), extra disk space (u64), slices per disk (u32), five 1-byte
  enums, the compression method (1 byte: 0 stored, 1 zlib, 2 bzip2, 3 LZMA1,
  4 LZMA2), architectures (2), two page settings (2), uninstall size (u64), and 47
  option flags packed into 6 bytes.
- Then the entries, in that order. The reader skips languages (10 strings + 21
  bytes), messages (2 strings + 4), permissions (1 string), types (4 strings + 30),
  components (5 strings + 42), tasks (6 strings + 26) and directories (7 strings + 27).
- **File entries**: 10 strings (source, destination, font, assembly, components,
  tasks, languages, check, after install, before install), Windows versions (20),
  the data entry index (u32; -1 when the file has no data), attributes (u32),
  external size (u64), permission (i16), 32 flags (4 bytes), type (1).

The rest of the first block (icons, registry, run entries, wizard images) isn't needed.

## The second block: data entries

74 bytes each: first and last slice (u32, both 0 in a single-file installer), chunk
offset from the data offset (u32), offset of the file inside the decompressed chunk
(u64), size (u64), compressed chunk size (u64), SHA-1 of the file (20), file time
(u64), file version (8), and 11 flags in 2 bytes. Flag bit 4 means x86 CALL/JMP
targets were made absolute (undone on reading: in 64 KiB blocks, E8/E9 followed by an
address whose high byte is 00 or FF), bit 6 encrypted (refused), bit 7 compressed.

A chunk starts with `zlb\x1a`, then the data: stored, or compressed with the header's
method (LZMA1: 5 property bytes first; LZMA2: 1). Shogo's installer uses LZMA2, gives
every data entry a chunk of its own, and compresses 118 of its 509 chunks; the rest
are stored, since GOG has already packed the files with zlib. LZMA comes from macOS's
own liblzma (`/usr/lib/liblzma.dylib`, declared in `inno_setup.cpp` as the SDK has no
header for it).

## GOG Galaxy files

Inno Setup's file list only names `{tmp}` files with hashed names. GOG's script joins
them into the game's files at install time, driven by calls in the file entries:

```
check_if_install('en-US#', '32#64#', '')                    languages, bitness, Windows variant
before_install('<file md5>', 'Music/RedRiot/a1.sec', 1)     on a file's first part: name, part count
after_install('<part md5>', <stored size>, <size>)          on every part
```

Each part is a 10 MiB piece of the file, compressed with zlib. Files whose entries use
`before_install_dependency` are redistributables (here, GOG's script interpreter).
The import takes the English (`en-US#`) files, skips `__redist/`, GOG's `goggame-*`
files and the Windows XP variant of a file (`winxp#`; `!winxp#` is the normal one), and
copies `__support/app/*` (the default configs, `autoexec.cfg` among them) into the game
folder as GOG's installer does, unless the player already has that file. Every part's
Inno SHA-1 and every file's MD5 are checked.
