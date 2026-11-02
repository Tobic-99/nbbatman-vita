# PS Vita controls

| Vita | Game |
|---|---|
| D-pad, left stick | move |
| Cross | button 1: swing (confirms in menus) |
| Circle | button 2 |
| START | coin, then 1P start (one press) |
| SELECT (short press, alone) | pause / resume |
| SELECT + L | save state |
| SELECT + R | load state |
| SELECT + START, held 2 s | delete the save state |
| SELECT, held at launch | start fresh, without resuming |

Only player 1 is mapped.

## Saves

- `ux0:data/nbbatman/state.bin`: the save state. It is also written every 3 minutes of play and resumed at launch. A bar in the
  corner of the screen confirms: green saved, blue loaded, red failed or deleted.
- `ux0:data/nbbatman/hiscores.bin`: the high score table. The original board forgets it at power-off; here it survives restarts,
  starting fresh and deleting the save state, and loading a state never replaces it with older scores.

## Option file

An empty file `ux0:data/nbbatman/aspect43` shows the picture in 4:3 instead of full screen.
