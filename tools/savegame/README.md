# Save tools (PC side)

Plain Python 3, no dependencies beyond the standard library. Work on copies:
none of the tools writes to its input.

| Tool | What it does |
| ---- | ------------ |
| `wwstate.py info STATE.wwstate` | A portable save state (`slotN.wwstate`, e.g. from a bug report): check both checksums and show its header, Link's place and the Quest Log progress. |
| `wwstate.py to-sav STATE.wwstate -o OUTDIR [--into cking.sav] [--file N]` | Writes `OUTDIR/cking.sav` whose Quest Log N (default: the state's own) is the state's save data; the other Quest Logs come from `--into`, else they are empty "New Game" files. Copy the folder's `user/` over the app's save folder to use it. |
| `hd_save_info.py PATH` | Progress of each file in a `cking.sav` (hearts, rupees, items, songs, shards, ...). `--short`, `--json`, `--file N`. |
| `wwsave.py` | Library both tools use (HD/GameCube save layouts, checksums). Not run directly. |

The app's own copies live in its files folder (`save/user/cking.sav`,
`states/slotN.wwstate`); pull them over USB or with the in-app export. See
[docs/portable-save-states.md](../../docs/portable-save-states.md).
