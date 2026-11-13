# Link database formats

Offsets are in bytes. "Position" means an offset from the start of the file;
"name" means an offset into a string table. Every layout here round-trips the
shipped databases byte for byte.

## XLink (`XLNK`)

No byte order mark: the version word reads small in exactly one byte order.
Byte order follows the platform, not the version: Splatoon and Breath of
the Wild on Wii U are big-endian, Breath of the Wild on Switch ships the
same ELink2 v30 and SLink2 v28 databases little-endian, and the two convert
into each other byte for byte. Every field is stored at its own width: the
16-bit pairs (asset id and flag, trigger flag and overwrite hash, slot
ranges) are two 16-bit values and the asset parameter mask one 64-bit
value. No big-endian 64-bit database exists to compare against; it is
written by the same rule. Text is Shift-JIS in Splatoon 2 and UTF-8 in
Breath of the Wild and Splatoon 3, again independent of the version.

### Versions 27 to 36

Established from the Splatoon 2 and Game Builder Garage builds of
`xlink2::ResourceParamCreator` (`createCommonResourceParam_`,
`solveUserBin_`, `dumpUserBin_`, `dumpCommonResourceRear_`) and, for the
64-bit layout, the Splatoon 3 11.3.0 equivalents (which still load ELink2 v34
and SLink2 v31, the versions Splatoon 3 1.0.0 ships).

| Section | 32-bit | 64-bit |
| --- | --- | --- |
| Header | 0x48 | 0x60; positions are 8 bytes |
| User name hashes | 4 per user, sorted (the game binary-searches them) | same, then aligned to 8 |
| User positions | 4 per user | 8 per user |
| Parameter definitions | 0x14 header, 12-byte entries, own string table | 0x18 header, 24-byte entries (8-byte name, type, 8-byte default: doubles and 64-bit ints) |
| Asset parameter sets | 64-bit mask + one 4-byte word per set bit | same, unpadded |
| Trigger overwrite sets | 32-bit mask + words | same |
| Local property names, enum names | name per entry | 8-byte name |
| Direct values | 4 bytes | 4 bytes |
| Random table | min, max | same |
| Curves | 0x14 | 0x18 (8-byte property name) |
| Arrange groups | count + 8-byte entries | count + 16-byte entries |
| User header | 0x30; 0x2C in ELink2 29 and SLink2 27 | 0x38; two 16-bit counts in place of the local property count from ELink2 36 and SLink2 33 |
| Switch condition | 0x14 of words; 0x10 of bytes in ELink2 31 and SLink2 29 | 0x10, or 0x18 with an 8-byte enum name |

The effect and sound systems of one library build carry different versions
(ELink2 30 with SLink2 28, ELink2 31 with SLink2 29, ELink2 34 with SLink2
31), so the layout follows the version and the system together: ELink2 is
64-bit from 32, SLink2 from 31. A file says which by itself: the 64-bit
header leaves one half of its first position zero. ELink2 29 and SLink2 27
(the October 2016 Mario Kart 8 Deluxe prototype's
`ResourceParamCreator::createUserBinParam`) have no local property count in
the user header, so users list no local properties. Animal Crossing: New
Horizons' switch conditions (ELink2 31, SLink2 29) put the property type,
comparison and both flags in bytes after the parent type, then the value or
enum name position, then the enum name index; the 32-bit layout follows
`xlink2`'s `ResSwitchCondition` and both databases round-trip byte for byte.

The local property name and enum name tables are sorted, and containers,
curves and enum conditions store the index of their name in them beside
the name. An action switch (the byte after a switch container's `IsGlobal`)
keeps an action hash in its conditions' index instead. Importing a user
adds the names the database lacks, keeping the tables sorted and moving
the indices that pointed at their own names.
| Users | | |
| Conditions | tagged by container type | tagged; switch conditions are 16 or 24 bytes |
| Name table | | |

A parameter word is an 8-bit reference type and a 24-bit value. Direct and
Bitflag values index the direct value table (`getResParamValueInt_` always
reads `directValueTable[value]`); String values are names; Curve, Random and
the Random variants index their tables; ArrangeGroup values are offsets into
the arrange region.

A user block: header (11 counts, then the trigger table position), local
property names, one word per user parameter definition, call indices sorted
by key name (padded to 4), calls (0x20 / 0x30), containers, then at the
trigger table position: action slots, actions, action triggers, properties,
property triggers, always triggers. In 64-bit files an action's first
trigger index is 16 bits followed by a 16-bit flag word.

Containers: switch 0x18 / 0x20 bytes, others 0x0C / 0x10. 64-bit files add
type 5, a grid: two watched properties, two axes of values and a cell per
pair naming a call. In 64-bit files the container type is a byte; the next
byte marks a blend container that blends by a property, which then carries
the switch container's fields (watched property, index, global flag) and is
0x20 bytes, and the two after it are written at run time. ELink2 36 and
SLink2 33 add type 6, a jump container with one child call and no
conditions.

A curve's last 16-bit field is its update type: 0 follows the property as it
changes, 1 does not (`xlink2`'s `CurveUpdateType`).

ELink2 36 and SLink2 33 and later split the user header's local property
count into two 16-bit counts. The first is the local property count, which
Tears of the Kingdom 1.0.0's loader, `UserResource::setupResourceParam_` and
`ResourceParamCreator::solveUserBinAboutGlobalProperty_`, and Splatoon
Raiders 1.1.0's loader and `solveUserBinAboutGlobalProperty_`, read
unsigned; none of them reads the second. That one counts the distinct global properties the user reads:
through global switch and blend-by containers, grid axes marked global
(grid flag bits 0 and 1), curves in its assets' parameters, and its global
properties. Every Tears of the Kingdom and Mario Kart World user matches
that, but three Splatoon Raiders users (two of them have no calls at all)
and three Nintendo Switch Sports Resort sound users do not, so
eXamineLink keeps the stored count, sets it from the contents only when
converting from a version without it, and validation notes a mismatch.
`setupResourceParam_` finds local property names by binary search
(`strcmp`) in both the database's table and the user's list, so validation
requires both sorted in these versions.

A jump container's child is a call another container holds, so it keeps that
container's parent index and condition.

Users go out in hash order, which the game's binary search needs, so their
order in the file is not the database's to choose.

Quirks kept as found:

- Calls with equal key names appear in the sorted index table in no
  derivable order; the order read is kept while it still sorts the calls.
- Splatoon 3 has calls whose condition points one past the end of the
  condition table.
- Splatoon 3 switch containers set the byte after `isGlobal`.
- Blend and Sequence container children carry conditions made for other
  container types.
- Containers with no children use the range `[-1, -1]`.

### Version 19

Found in Splatoon's `Gambit.rpx`: the resource loader that checks `XLNK`
reads a 0x40-byte header: definition counts, then the counts of asset
parameter words, trigger overwrite sets, random entries, curves and curve
points, the user count, and the positions of the conditions, the arrange
region and the name table. The user loader reads a 100-byte header holding
the counts, the trigger table position, the name and the base user's name.

Differences from later versions:

- Parameters are 8 bytes, a reference type and a full 32-bit value; there is
  no direct value table. Every asset has its full parameter array.
- Definitions carry no defaults.
- Users are found by name, not hash, with a linear search
  (`Gambit.rpx`), so any order works; the first of two users with the same
  name is the one found.
- Calls are `key, asset id (32-bit), guid, duration, parent, flags (32-bit),
  parameters, condition`; triggers also store the call's key name.
- Container types are numbered Switch, Random, Blend, Sequence, Random2 (the
  same children appear in a Blend in Splatoon 2).
- Conditions have no type tag: each is read as its parent container's type,
  and identical byte runs are shared, so a condition may start inside
  another. Switch conditions start with a zero word; the property's type
  comes from the game, so it is inferred per property from the values used.
  Each user header repeats the condition, random and curve counts.

### Version 21

Found in Super Mario Maker's `Block.rpx`, whose loader checks `XLNK` and
the version, then binds the tables in place. It sits between version 19 and
ELink2 29:

| Part | Layout |
| --- | --- |
| Header | 0x4C: version 19's counts with the local property name, enum name and direct value counts after the trigger overwrite set count; then the counts of random entries, curves and curve points, the arrange region's position, the user count and the positions of the conditions and the name table |
| Users | a position and a name per user; found by name with a linear search |
| Definitions | name and type, no defaults, as in version 19 |
| Parameters | one word each, a reference type and a 24-bit value, with the direct value table of later versions |
| Asset parameter sets | every asset's full array, without a mask |
| Trigger overwrite sets | a mask, then a word for every trigger parameter (zero where the mask is clear) |
| Curves | 0x14, as ELink2 29 |
| User header | 0x2C, as ELink2 29: counts of calls, assets and Random2 containers, then of slots, actions and triggers, then the trigger table position |
| Calls | 0x18: name, asset id (16-bit), flags (16-bit), duration, parent, parameters, condition; no guid or name hash |
| Triggers, actions, properties | as ELink2 29 |

Container types are numbered as in later versions. Conditions have no type
tag and share byte runs, as in version 19. A switch condition holds the
property type, comparison and value, then a 16-bit enum name index and the
solved and global bytes (0x10); random conditions hold a float weight. The
game reads only switch and random conditions: blend and sequence children
almost never have one, and eXamineLink keeps a blend condition as its
minimum and maximum. The condition pool's byte order is swapped field by
field, since its switch conditions mix widths.

Converting from version 21 gives calls guids derived from the user and call
names; converting to it drops them, as it does a base user's name.

## ELink (`eflk`) and SLink (`SLNK`)

From Mario Kart 8's loaders (`Turbo.rpx`, with symbols from the 2014 linker
map) and Happy Home Designer's `eflk_*` / `slnk_*` functions. Strings are
Shift-JIS.

| | ELink 9 | SLink 69 | ELink 10 | SLink 73 |
| --- | --- | --- | --- | --- |
| Header | 0x10: users, name table | 0x10: size, users | 0x14: users, pools | 0x18: size, users, pools |
| Users | position + name each | positions, then shared strings | shared assets and triggers, positions, shared strings | same |
| User header | 0x1C: counts, own strings | 0x30: counts, trailer, distance settings | 0x1C, names, base name | 0x18, names, base name, trailer |
| Call table | 0x14 | 0x14 | 0x14 | 0x14 |
| Action | 0x0C | 0x14 | 0x0C | 0x14 |
| Asset / effect | 0x68 | 0x5C | 0x68 | 0x4C |
| Action trigger | 0x3C | 0x4C | 0x3C | 0x40 |

SLink 69 user settings at +0x20 are three floats and a flags word.
Mario Kart 8 (v81) passes the floats to its 3D sound calculation
(`audio::AudCalc3DParam::setMaxVolumeDistPtr`, `setUnitDistPtr`,
`setInteriorSizePtr`): the distance of full volume, the falloff's distance
unit and the size of the source. Each setter skips a 0, keeping the
defaults 80, 250 and 70. Bit 31 of the flags word makes the game measure
distance on the horizontal plane only; no other bit is read.

3DS users list pool indices and store `-1` in place of their own records,
or alias another user (16 bytes: kind 2, name, base name). A trailer word of
zero means records use the shared strings. One Happy Home Designer user is
followed by eight `0xFF` bytes, kept as found.

All four loaders find users by name with a linear search, so users may be
in any order.

Mario Kart 8 plays one effect per call table (0x2A5E368 in v81): of the
effects whose set conditions (+0x08, +0x10, +0x18) all equal the three
values the game passes, the one with the most conditions set, the first on
a tie. Effect triggers fire while start frame (+0x10) <= frame < end frame
(+0x14) (0x2A5E8CC); a nonzero word at +0x04 lets the trigger with the
same word in the next action take over the running emitter. Their floats
override the effect's scale, position and rotation, the ones bit 1 to 7 of
the 16-bit flags at +0x18 select (bit 0 selects the bone). Sound triggers
keep their frames at +0x10 and +0x14 too, -1 ending with the action. Sound
values come in ranges: a value and the top of the range a random value is
drawn from (0 for none): 0.9 and 1.1 for a volume, -0.3 and 0.3 for a pan.
A sound call table with bit 0 of its third flag byte set picks one of its
assets at random; Mario Kart 8 Deluxe made those random containers and kept
only the first asset of the others.

Several 32-bit words of these records are bytes or 16-bit halves the games
read one by one (Mario Kart 8 with `lbz`/`lhz`, Happy Home Designer with
`LDRB`/`LDRH`), and the same values in the Wii U and 3DS files only agree
when read that way. eXamineLink keeps each part at its width, so either
byte order lays them out correctly:

| Record | Offset | Parts |
| --- | --- | --- |
| Call table (both) | +0x0C | four bytes |
| Effect | +0x30, +0x34 | four bytes each |
| Effect | +0x38 | signed 16-bit, two bytes |
| Effect trigger | +0x18 | 16-bit, two bytes |
| Sound asset | 50: +0x40, 69: +0x58, 73: +0x48 | four bytes |
| Sound trigger | +0x18 | 16-bit, signed 16-bit |

The two words after a sound action's name, and the last word of a
SLink 69 trigger, are zero in every shipped file, so their width cannot
be told; they are kept as 32-bit words.

String fields were identified from the loaders (ELink 10: effect
+0x08, +0x10, +0x18, +0x20 emitter set, +0x28 bone, +0x2C; trigger +0x08,
+0x1C. SLink 73: asset +0x08, +0x10, +0x18 label, +0x44. SLink 69:
asset +0x08, +0x10, +0x18 label, +0x20, +0x54; its triggers name their call
by index). Words the loaders overwrite at run time are kept but hidden.
ELink parameter words belong to the user's effect slots.

### SLink 50

From Animal Crossing Plaza's loader (`Lounge.rpx`), which accepts only
`SLNK` version 50. The file is laid out like version 69: header, one
offset per user, the shared strings, then the user blocks, each ending
with a zero word when its records use the shared strings. Users are found
by name with a linear search.

| Record | Size | Fields |
| --- | --- | --- |
| User header | 0x3C | kind, block size, name, call table, action, asset and trigger counts, trailer position, then seven words |
| Call table | 0x0C | name, the game's, asset range |
| Action | 0x0C | name, the game's, trigger range |
| Asset | 0x44 | as version 73 without the pan pair: id, the game's, two conditions each followed by an id, label, label id, volume, pitch and low-pass ranges, delay, bone, four flag bytes |
| Trigger | 0x3C | id, the game's, call index, label, start and end frame, 16-bit flags, signed 16-bit, volume, pitch and low-pass ranges, delay, a word |

The seven words after a user's counts are 1.0, 1.0, 0.5, 4, 1.0, 0 and 50
in every user. Nothing in the game reads them: the loader keeps a pointer
to them that no code uses, and no other code reads past a user's
counts.

Call tables and actions are found by binary search on their names
(byte-wise), so they must be sorted; eXamineLink saves them sorted and
moves the triggers' call indices with them. A call table with any asset
whose third flag byte is set picks one of its assets at random, weighted
by the word at +0x0C; when the weights add up to nothing it falls back to
the conditions. Otherwise the game replaces +0x0C and +0x14 with the ids of
the two condition names (0xFFFF when empty or unknown) and plays the first
asset matching both values it passes, else the best partial match, an
empty condition matching anything. Mario Kart 8 and Happy Home Designer
assets carry the same weights and flag, and in Mario Kart 8 and Tri Force
Heroes a call table sets bit 0 of its third flag byte exactly when one of
its assets is random, so conversion moves the mark between the two.

The first flag byte holds a sound while the game keeps asking for it, the
second updates its position every frame. The other parameters: label id,
when nonzero, is played in place of the label; volume, pitch and low-pass
ranges draw at random between their two values when the second is
nonzero (`SetVolume`, `SetPitch` and `SetLpfFreq` of the sound); delays
are frames, and a trigger's adds to its asset's. Trigger flags: 1 fires
once per action, 4 follows the position, 8 plays only while the frame is
in range, 0x10 plays the trigger's own label in place of its call. A
trigger fires while start <= frame < end, -1 lasting until the action
ends; a signed 16-bit value of 0 or more stops its sound when the action
changes.

## ELink0 (`BLNK`)

From Nintendo Land 1.0.0's loader (`Lunch.rpx`), which binds a database in
place and fills in its run-time words. Every number is big-endian; the size
word decides the byte order on reading. Strings are offsets from the start
of the entry holding them, and every hash is the CRC-32 of the string next
to it, so both are rebuilt on save. No string has a byte above 0x7F, so the
encoding is taken to be Shift-JIS.

| Offset | Header (0x20) |
| --- | --- |
| 0x00 | `BLNK` |
| 0x04 | file size |
| 0x08 | user table position, 0x20 (nnfck reads this as version 32) |
| 0x0C | user count |
| 0x10 | record table position |
| 0x14 | record count |
| 0x18 | string table position |

A user (0x14 bytes) is its name hash, first record, record count and name;
the last word is the game's. The game finds a user by hash and plays the
first of its records whose event hash matches and whose variant fits the
model.

| Offset | Record (0xCC) |
| --- | --- |
| 0x00 | user hash |
| 0x04, 0x08 | event and emitter set hashes |
| 0x0C | scale |
| 0x10 | red, green, blue, alpha: copied into the emitter set's colour |
| 0x20 | follow: 0 the whole transform, 1 without scale, 3 none, 4 without rotation, anything else position only |
| 0x24 | nonzero: place the effect when emitted, then stop following |
| 0x28 | bone hash |
| 0x2C | position offset |
| 0x38 | eight frames of the animations below that emit the record; the list ends at the first -1, and a value below -1 is the negated offset of a named trigger |
| 0x58 | frame that stops the effect, -1 for none |
| 0x5C | play type: 0 once, 1 on every trigger, 2 held (the sound loops) |
| 0x60 | variant, by the game's names: 0 any owner; 1 to 10 (TV1 to TV4, DRC, Op0 to Op4) an owner of that variant; 11 to 17 (Sand, Water, Ground, Kusa, Rock, Mud, Snow) a ground material, Ground standing for any material no other record of the event names |
| 0x64 to 0x80 | user, event, emitter set and bone names, four animation names |
| 0x84, 0x86 | never read by the game; zero in every shipped database |
| 0x85 | nonzero: the emitter set takes its controller's eight parameters |
| 0x88 | effect group, -1 for the controller's |
| 0x8C, 0x90 | camera shake: 0 none, 1 for 20 frames, 2 for 30, 3 or more for 60, with its strength (0 or less means 1) |
| 0x94 to 0xB7 | the game's |
| 0xB8 | sound name |
| 0xBC, 0xC0 | sound volume and pitch |
| 0xC4 | frames the sound fades over when the effect stops; -1 leaves it playing |
| 0xC8 | the game's |

Nintendo's tool sorts users by name, ignoring case, and writes their
records in that order. Records under an empty user name come first and no
user lists them, so the game loads but never plays them; eXamineLink keeps
them apart as unlisted records. Strings are added in the order records use
them (names, then trigger names, then the sound), then user names.

## Converting between versions

Fields carry over by name between the schemas of two versions; anything
without a counterpart is reported before saving.

- XLink 19, 21 and later: container types are renumbered (19 orders them
  Switch, Random, Blend, Sequence, Random2), parameters gain or lose the
  direct value table, and switch conditions take the property type inferred
  from their values. These mappings follow the loaders but were not seen
  working in a game.
- ELink 9 and 10, SLink 50, 69 and 73: going to a Wii U version, pooled
  records are copied into each user that lists them (with that user's
  parameter words) and aliases become copies of their base user. SLink
  records are rebuilt field by field: 69's distance settings and surround
  panning have no place in 73 (going to 69, the distances are 0, which
  Mario Kart 8 replaces with its defaults), and a 69
  trigger's call index becomes the call's name in 73 and back. A database
  taken 69 to 73 to 69 differs from the original only in those values and
  in string table layout. Version 50 has no pan pair and its seven user
  words, which nothing reads, have no place in the others; going to 50
  users get the words Nintendo's tool writes, and a call table's random
  bit marks its assets (each weighing 1 when none has a weight). Animal
  Crossing Plaza's database taken to 69 or 73 and back differs only in
  string table layout.

## Between ELink or SLink and XLink

Mario Kart 8 Deluxe ported Mario Kart 8's databases; matching their users
by name hash (387 effect and 222 sound users) shows the mapping, which the
user import follows:

| ELink effect | ELink2 parameter |
| --- | --- |
| EmitterSet | RuntimeAssetName (AssetName is the call's key) |
| Bone, Group | Bone, EffectGroup |
| byte +0x30 | Matrix |
| byte +0x32 | RotateSource |
| byte +0x37 | Clip, less 1 |
| +0x38 | ForceCalc |
| floats +0x3C to +0x64 | Scale, PositionXYZ, RotationXYZ, Red, Green, Blue, Alpha |
| byte +0x31 with a matrix mode | clears BitFlag bit 1 (IsFollow), set otherwise |

| SLink asset | SLink2 parameter |
| --- | --- |
| Label | RuntimeAssetName |
| Volume, Pitch, Lpf, Pan, SurroundPan and their tops | the same, random when the top lies above |
| DelayMin, DelayMax | Delay |
| Bone | Bone |
| user InteriorSize | user parameter InteriorSize |

Calls nest as Mario Kart 8 Deluxe nested them: a switch per condition in
order, keyed `Call_<value>` for containers, `Call_NN` for assets and
`Call_Default` for the case no value matches. Effect triggers become action
triggers (start 0 to the end as the always type), their overrides an
overwrite set. Parameters equal to their default are left out, except
overrides, which replace the effect's value whatever it is. The parameter
words ELink effects carry, SLink's distance settings, the
handover word and fields no ELink2 or SLink2 parameter matches are reported
as dropped. The 16-bit hash beside a trigger's overwrite set follows the
set by a scheme not yet known; new sets reuse the hash the database pairs
with an identical set, or its most common one.

## Not covered

XLink 27 to 36 other than
27 to 31, 33, 34 and 36 open with the layout of their width and save as
their own version. ELink2 36 pairs with SLink2 33 in Tears of the Kingdom
and with SLink2 34 in Splatoon Raiders, Nintendo Switch Sports Resort,
Mario Kart World and Donkey Kong Bananza, which share one layout.
