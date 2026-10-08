# Portable save states

Save states come in two kinds that share the five slots:

| | Full (default) | Portable (bug reports) |
|---|---|---|
| File | `slotN.bin` (+ `slotN.png` picture) | `slotN.wwstate` |
| Size | about 250 MB | a few KB (refused above 64 KB) |
| Holds | all guest memory (game code, decompressed assets), threads, HLE state | the save data of the current Quest Log, Link's place, a header |
| Shareable | **never**: it contains game code and data | yes, attach it to bug reports |
| Loads into | only the same build, exactly where it was saved | any session of the same game release, any build |
| Exact | yes | no (see below) |

Both live in the states folder (`states/` in the app's files folder; `WWHD_STATE_DIR` in
`wwhd.env` overrides it). Loading a slot loads whichever kind it holds (the newer file if it
holds both). Saving one kind never deletes the other.

Unlike upstream, full snapshots stay this app's default (slots keep their picture and exact
restore); a portable state is written on request: the bug-report flow below, an import, or
`WWHD_PORTABLE_SAVE` (personal builds).

## When a portable state can be saved

Only while the player controls Link: the conditions under which the game itself opens its pause
menu: no event or cutscene running (and not for 5 frames after one, like the game), no message or
dialogue box, no game menu open, no stage change or wipe in progress, Link is the controlled actor,
and Link is not on a rope (on a rope or swinging from the Grappling Hook he would restart in
mid-air). The Telescope / Picto Box aiming checks are left out: Link restarts standing at the same
spot, which is harmless. Otherwise nothing is written; the screen shows why ("can't save during a
cutscene or dialogue ...") and the log names the reason (`[savestate] ... portable state refused:
...`). Full save states can be made at any time. Loading is not affected.

Game addresses are the canonical USA ones mapped to the running release (`release.h`), so portable
states work with the USA, European and Japanese games; on EUR/JPN they are new – if a load lands
somewhere odd, the full state and the game save are unaffected, and a bug report with the
`.wwstate` is exactly what helps.

## Bug reports

The Saves tab's **Copy save for bug report** saves a fresh portable state where Link stands
(`states/bugreport.wwstate`) and shares it with `cking.sav` through Android's share menu (both go
through the app's read-only provider, since other apps can't open its files folder). The state
holds no game code and no game data, so it is safe to send.

A developer loads a received state by importing it (it becomes a slot's `.wwstate`, and the slot
loads it when it is the newer file) or with `WWHD_PORTABLE_LOAD=<file>` in `wwhd.env` (applied as
soon as a Quest Log is being played). `tools/savegame/wwstate.py info <file>` shows what a state
holds (place, hearts, items, songs, ...) and `wwstate.py to-sav <file> -o <dir>` turns it into a
`cking.sav`.

## What a portable state holds

A UTF-8 text file of `key = value` lines (`runtime/src/portable_state.h`):

- header: `format` (1), `title_id` and `title_version` (from `meta/meta.xml`), `game_hash` (a hash
  of `cking.rpx`, to tell executables apart; not its contents), `runtime` (app version and commit),
  `created`, `file_slot` (Quest Log 0–2), `player_name`;
- place: `stage`, `start_point`, `start_room`, `layer` (how the stage was entered), `room` (Link's
  room), `link_pos`, `link_angle_y` (shape angle), `link_proc`, `on_ship` (Link rides the boat),
  `has_ship`, `ship_pos`, `ship_angle_y` (the boat, when it is in the stage), `time_of_day` and
  `date` (day counter; day of week = date % 7);
- `savedata`: the Quest Log's block of `cking.sav` (0xA94 bytes), made by the game's own save
  functions: `dSv_info_c::putSave` for the current stage and `dComIfGs_setGameStartStage` as the
  in-game save does before writing (both undone afterwards, so making a state changes nothing in
  the game), then `dSv_info_c::memory_to_card` (025BA9FC). Inventory, flags, progress, dungeon
  memory, time of day;
- `hd_player`, `hd_status`, `hd_event`, `hd_map`: the HD per-file sections of `cking.sav` (16, 4, 20
  and 220 bytes), the stored ones with the live data copied over by the SaveMgr's own copy functions;
- `checksum`: CRC-32 of everything before it.

Weather is not recorded: it follows from the progress, the place and the time of day.

**Guard**: the writer and the reader accept only these fields, the binary ones at exactly these
sizes, text values up to 128 characters and files up to 64 KB; a file that breaks any of this is
not written, and not read. A state can therefore never carry a memory dump.

## Loading

At the frame boundary on the game thread, once a Quest Log is being played (the title screen and
file select wait):

1. `dSv_info_c::card_to_memory` (025BA7B0) puts the save data into the game;
   `dSv_info_c::getSave` of the current stage makes the stage memory the loaded one, the dungeon
   bits and temporary flags start fresh, the HD sections are copied in, and the SaveMgr's refresh
   (02721880) sets the item buttons and equipment from the loaded data;
2. Link's room, position and angle go into `dSv_restart_c` (as when the game restarts a room after a
   fall), and the next stage is the recorded one with spawn point -1: the game creates Link at that
   position. With the boat in the stage, the restart the Song of Passing uses instead (spawn
   point -3): the boat is put back, and Link starts on it or standing beside it, with the sail down.

The log then says `[savestate] portable load: arrived in <stage> room <n> at x y z (distance d from
the saved position)`.

The state goes into the Quest Log being played. When it was made in another Quest Log, the screen
says so for a few seconds (and the log notes it); loading goes ahead.

**Not restored** (it is not a snapshot): enemies, items lying around, moving platforms and other
actor state start as the stage starts them; a running cutscene, dialogue or minigame is not resumed;
Link starts standing, or sitting in the boat with the sail down; the camera starts behind Link;
the file is loaded into the current Quest Log of the session (saving in game afterwards writes it
there).
