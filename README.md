# eXamineLink

A Qt 6 editor for Nintendo's link databases, which tie game events to
effects and sounds:

| Format | Versions | Games checked |
| --- | --- | --- |
| XLink (`XLNK`, `.belnk`, `.bslnk`) | 19 | Splatoon (ELink2 v19, SLink2 v19) |
| | 27 to 31, 32-bit | Mario Kart 8 Deluxe prototype (ELink2 v29, SLink2 v27); Splatoon 2, ARMS, Mario Kart 8 Deluxe, Game Builder Garage, The Legend of Zelda: Breath of the Wild on Wii U and Switch (ELink2 v30, SLink2 v28); Animal Crossing: New Horizons (ELink2 v31, SLink2 v29) |
| | 31 to 36, 64-bit | Splatoon 3 (ELink2 v34, SLink2 v31); The Legend of Zelda: Tears of the Kingdom (ELink2 v36, SLink2 v33); Splatoon Raiders, Nintendo Switch Sports Resort and Mario Kart World (ELink2 v36, SLink2 v34) |
| ELink (`eflk`) | 9 | Mario Kart 8 (Wii U) |
| | 10 (shared pools) | Animal Crossing: Happy Home Designer (3DS) |
| SLink (`SLNK`) | 50 | Animal Crossing Plaza (Wii U) |
| | 69 | Mario Kart 8 (Wii U), The Legend of Zelda: Tri Force Heroes (3DS) |
| | 73 (shared pools) | Animal Crossing: Happy Home Designer (3DS) |
| ELink0 (`BLNK`, `.belink`) | none; effects and sounds in one record | Nintendo Land (Wii U) |

Databases open straight from their containers: Yaz-compressed SARC
archives (`.szs`, `.sbelnk`, `.sbslnk`), Zstandard (`.zs`) or uncompressed.
Saving puts the same containers back. Nintendo Land keeps its `ELink/<Scene>.szs`
archives inside the packs in `content/Common/Package`; take them out with a
SARC tool first.

Some games compress with Zstandard dictionaries (Tears of the Kingdom's
`.zs` files). Put the game's dictionary pack (`romfs/Pack/ZsDic.pack.zs`,
any name works) or its loose `.zsdic` files in the `ZsDic` folder beside
the executable, which eXamineLink creates on start. Packs of several games
can sit side by side; of dictionaries sharing an ID, the one that decodes
the file is used, and saving compresses with it again. Byte order and text encoding belong to
the platform and game, not the version, so every version reads and writes in
either.

## Building

```sh
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Needs CMake 3.21, a C++20 compiler and Qt 6 (Core and Widgets). Zstandard is
a submodule built with the project; Yaz and SARC are implemented in-tree.

This builds:

- `examinelink`: the editor.
- `examinelink-cli`: conversions, XML export and import, validation, and the
  round-trip self test.
- `examinelink-uitest`: an offscreen test of the editor window.

`-DXL_DEPLOY_QT=ON` (Qt 6.5 or later) makes `cmake --install` copy Qt's
libraries and plugins beside the executables, and on Windows the MSVC
runtime, so the install folder runs on machines without Qt. On Linux the
bundle holds only Qt and ICU; the X11 platform plugin still needs the
system's `libxcb-cursor0` and related XCB libraries.

Pushing a tag that matches the project version, such as `v1.0.0`, runs
`.github/workflows/release.yml`: it builds bundled Windows (zip) and Linux
(tar.gz) packages and publishes them as a GitHub release. Running the
workflow by hand builds the packages as artifacts without releasing.

## The editor

Pick a user on the left, or **[Database tables]** for the database-wide
tables. The tabs group a user's lists the way the link editors did: assets
(calls and containers), actions (slots, actions, action triggers),
properties, always-triggers and the user's own parameters. The panel on the
right edits the selected entry; a field that points at another entry (a
call's parameter set, condition or container, a trigger's overwrite set)
expands to that entry so shared data can be edited in place. Double-click a
reference to go to it.

Right-click a user, an entry or a field for the actions that apply to it
(export or replace a user from XML, duplicate, remove, move, copy names,
go to a reference). Click the Category or Problem header of the problems
list to sort ascending, again for descending, and once more for the order
validation found them in; Ctrl+C copies the selected problems.

Adding, duplicating, removing and moving entries keeps every reference
pointing at the same entries: indices, child ranges and parameter
definitions are renumbered, and references to a removed entry are cleared.
Every edit can be undone.

Users can be dragged into a new order, or moved with Move Up and Move Down,
in every format except XLink 27 and later: those games binary-search users
by hash, so their order is fixed and kept on saving. The other games search
users by name from the first, so order only matters when two users share a
name; the first is the one found.

Problems show along the bottom, graded Information, Warning, Error and
Fatal like the link libraries grade theirs. **Validate (with test save)**
also serializes the database and reports anything that would stop a save.

XLink versions after 19 store only a hash of each user's name. **Tools >
Name Users from a List** takes any text file of candidate names (an actor
list, for instance) and names every user whose hash matches.

### Saving into other versions

**File > Save As** offers every version of the same format (an ELink2
database converts only to other ELink2 versions), with a byte order
and text encoding. A **Preset** picks all three as a game ships them. The
effect and sound systems carry their own versions, so each has its own
presets; games whose system agrees share one, and presets exist only for
games whose databases were checked. Keys are the games' codenames.

Effect presets:

| Preset | Games | Version | Byte order | Encoding |
|---|---|---|---|---|
| `lunch` | Nintendo Land (Wii U) | ELink0 | big | Shift-JIS |
| `turbo` | Mario Kart 8 (Wii U) | ELink 9 | big | Shift-JIS |
| `takumi` | Animal Crossing: Happy Home Designer (3DS) | ELink 10 | little | Shift-JIS |
| `gambit` | Splatoon (Wii U) | ELink2 19 | big | Shift-JIS |
| `turbo-s-proto` | Mario Kart 8 Deluxe prototype (Switch) | ELink2 29 | little | Shift-JIS |
| `u-king` | The Legend of Zelda: Breath of the Wild (Wii U) | ELink2 30 | big | UTF-8 |
| `u-king-s` | The Legend of Zelda: Breath of the Wild, Game Builder Garage (Switch) | ELink2 30 | little | UTF-8 |
| `blitz` | Splatoon 2, ARMS, Mario Kart 8 Deluxe (Switch) | ELink2 30 | little | Shift-JIS |
| `park` | Animal Crossing: New Horizons (Switch) | ELink2 31 | little | UTF-8 |
| `thunder` | Splatoon 3 (Switch) | ELink2 34 | little | UTF-8 |
| `ex-king` | The Legend of Zelda: Tears of the Kingdom (Switch), Splatoon Raiders, Nintendo Switch Sports Resort, Mario Kart World (Switch 2) | ELink2 36 | little | UTF-8 |

Sound presets:

| Preset | Games | Version | Byte order | Encoding |
|---|---|---|---|---|
| `lounge` | Animal Crossing Plaza (Wii U) | SLink 50 | big | Shift-JIS |
| `turbo` | Mario Kart 8 (Wii U) | SLink 69 | big | Shift-JIS |
| `alice` | The Legend of Zelda: Tri Force Heroes (3DS) | SLink 69 | little | Shift-JIS |
| `takumi` | Animal Crossing: Happy Home Designer (3DS) | SLink 73 | little | Shift-JIS |
| `gambit` | Splatoon (Wii U) | SLink2 19 | big | Shift-JIS |
| `turbo-s-proto` | Mario Kart 8 Deluxe prototype (Switch) | SLink2 27 | little | Shift-JIS |
| `u-king` | The Legend of Zelda: Breath of the Wild (Wii U) | SLink2 28 | big | UTF-8 |
| `u-king-s` | The Legend of Zelda: Breath of the Wild, Game Builder Garage (Switch) | SLink2 28 | little | UTF-8 |
| `blitz` | Splatoon 2, ARMS, Mario Kart 8 Deluxe (Switch) | SLink2 28 | little | Shift-JIS |
| `park` | Animal Crossing: New Horizons (Switch) | SLink2 29 | little | UTF-8 |
| `thunder` | Splatoon 3 (Switch) | SLink2 31 | little | UTF-8 |
| `ex-king` | The Legend of Zelda: Tears of the Kingdom (Switch) | SLink2 33 | little | UTF-8 |
| `spiral` | Splatoon Raiders, Nintendo Switch Sports Resort, Mario Kart World (Switch 2) | SLink2 34 | little | UTF-8 |

The command line's `convert` takes a preset key, looked up among the presets
of the database's own format.

Choosing a version by hand takes the byte order and encoding of a preset
using it (of several, the one sharing the database's byte order, then its
encoding), or keeps the database's own when the version stays the same. Before writing,
it lists what the chosen version cannot hold: version 19 stores no
parameter defaults or local properties and finds users by name; 32-bit
layouts have no grid containers; ELink 9 and SLink 50 and 69 have no
shared pools or aliases, so those become copies. SLink 69 also stores
per-user distance settings and surround panning, which 73 has no place for.
Going the other way the distances are written as 0, which Mario Kart 8
replaces with its own defaults. SLink 50 has no panning and its call tables
and actions are a name and a range; they are saved sorted by name, as the
game looks them up by binary search.

Conversions rest on the layouts each game reads; the meaning of fields that
moved between versions is inferred, so test converted databases in the game.

### XML

The text form is XML, as the link editors' own per-user files were
(`Work/ELink2/UserFile/<User>.elink2.xml`). Their schema never shipped, so
eXamineLink's XML follows its own model rather than imitating a format it
cannot see.

- **File > Export Database as XML** writes the whole database; opening an
  `.xml` file reads it back. Nothing is lost: an unedited database exported
  and re-imported writes back byte for byte. Layout details (original string
  table order, padding) appear as `_`-prefixed attributes and elements so
  they stand apart from the data.
- **File > Export User as XML** writes one user together with every
  database-wide entry it points at. **Import User from XML** adds it to, or
  replaces it in, any database of the same format, reusing
  identical entries that already exist. A user exported from another
  version is converted on import, and what that version could not carry is
  reported the way Save As reports it.

### Importing users from another database

**File > Import Users from Database** (also on the user list's context menu)
copies users from another database of the same link system: any version
of a format into any other, and between the two generations, ELink to and
from ELink2 and SLink to and from SLink2. Pick the users, and whether users
the database already has are replaced or left alone; everything lands as
one undo step, and what has no place in the target is listed afterwards.

Between generations the records are rebuilt, following how Mario Kart 8
Deluxe ported Mario Kart 8's databases:

- An ELink call table plays one of its effects: the one whose
  conditions all match the three values the game passes, preferring the
  most conditions set. In XLink that becomes nested switch containers on
  three named properties, which the dialog asks for. They are guessed from
  the values both databases use: Mario Kart 8's conditions come out as
  GroundAttribute, WaterState and CourseID, as in Mario Kart 8 Deluxe.
- A SLink call table marked random becomes a random container, weighted
  as its assets are in SLink 50; one that is not plays its first asset.
  SLink 50 games pick among those assets by two conditions, which are not
  carried over. Volume, pitch, filter, pan and delay ranges become random
  parameters.
- Effect and sound fields become the ELink2 and SLink2 parameters of the
  same meaning (emitter set or label, bone, scale, position, rotation,
  colour, volume...), triggers become action triggers in one action slot
  (named in the dialog), and trigger overrides become overwrite sets.
- Going back, switches on those properties become conditions again, other
  containers keep their first child, and fields XLink has no counterpart for
  take the value the target's records use most.

Mario Kart 8's effect and sound users, Happy Home Designer's, taken into
Mario Kart 8 Deluxe, Splatoon 2 or Splatoon 3 databases and back, play the
same in every call table and trigger (`bridge-check` below). Whether the
games accept the result is untested.

## Command line

```sh
examinelink-cli info FILE...
examinelink-cli validate FILE
examinelink-cli to-xml FILE OUT.xml
examinelink-cli from-xml IN.xml OUT        # .szs and .zs outputs are compressed
examinelink-cli convert FILE OUT VERSION|PRESET [big|little] [shift-jis|utf-8]
examinelink-cli presets
examinelink-cli matches FILE REFERENCE
examinelink-cli export-user FILE USER OUT.xml
examinelink-cli import-user FILE IN.xml OUT
examinelink-cli import-users FILE SOURCE OUT [USER...] [--replace] [--slot NAME] [--conditions P1,P2,P3]
examinelink-cli bridge-check TARGET SOURCE
examinelink-cli check FILE...
```

`check` is the soundness test. For each database it verifies that:

- binary, container and XML round trips are byte-identical;
- every user survives export and re-import unchanged;
- swapping byte order and back is byte-identical, and swapping text
  encoding and back keeps every string;
- conversion to every other version, in either byte order, writes a file
  that reads back stably;
- users exported from every other version import and save;
- structural edits, undone, restore the original bytes;
- random arbitrary edits either save, or are refused only where validation
  already reports why, and whatever saves reads back stably;
- random edits that keep the database valid save and read back as exactly
  the edited database.

The databases are not part of the repository. Point the tests at your own:

```sh
cmake -S . -B build -DXL_SAMPLES="/path/ELink2DB.szs;/path/slink.bin" \
  -DXL_PAIRS="/switch/ELink2DB.sbelnk|/wiiu/ELink2DB.sbelnk" \
  -DXL_BRIDGES="/mk8dx/elink2.bin|/mk8/elink.bin"
ctest --test-dir build
```

`bridge-check` imports every user of SOURCE into TARGET, writes and reads
the result, and for an ELink or SLink source brings the users back
and compares what each call table picks for every combination of
condition values, and every action's triggers. `XL_BRIDGES` runs it on
TARGET|SOURCE pairs.

`matches` converts a database into another's version, byte order and
encoding and compares the two byte for byte. `XL_PAIRS` runs it on the same
database shipped for two platforms; Breath of the Wild's Wii U and Switch
link databases each convert into the other exactly.

## Format notes

[docs/FORMATS.md](docs/FORMATS.md) records the layouts, how each was
established, and the quirks shipped databases contain.

## Credits

Layouts build on nnfck for version identification,
[WoomLink](https://github.com/shadowninja108/WoomLink) for the Splatoon 2 resource format, and
[the xlink2 tool](https://github.com/dt-12345/xlink2) for Tears of the Kingdom,
and were checked against the loaders in each game's executable.

## License

MIT; see [LICENSE](LICENSE).
