# swrRace racer options (refined)

Options that widen one retail rule each. All off = retail.

## Boundary
| Retail | Faithful | Refined | Tier | Changed condition |
|---|---|---|---|---|
| 0x004663e0 | `swrObjJdge_SpawnRacers` | `refined_swrObjJdge_SpawnRacers` | equivalent | `identifier == 'Locl'` picks the full pod + rider model (`|| full_lod_ai`) |
| 0x00471760 | `swrRace_PoddAnimateVariousThings` | `refined_swrRace_PoddAnimateVariousThings` | equivalent | far-pod path `lodDistance > 75`, connector cutoff `lodDistance > 50` (skipped with `full_lod_ai`) |
| 0x0046bec0 | `swrRace_UpdatePlayerControl` | `refined_swrRace_UpdatePlayerControl` | sacred | taunt flame for pilot 2 only (`|| flamejet_local`) |
| 0x0046d170 | `swrObjTest_F0` | `refined_swrObjTest_F0` | sacred | AI flame for pilot 2 only (`|| flamejet_ai`) |

Each refined function is its faithful counterpart with only that condition changed.
State: `racer_options` (`racer_options.h`).

## Band-aids retired (swe1r)
- `ai_full_lod`: NOPs at 0x0046654d, 0x004723ce, 0x00472577 -> `racer_options.full_lod_ai`.
- Flamejet cheat (debug panel): JNZ patches at 0x0046c6e5 / 0x0046d20c ->
  `racer_options.flamejet_local` / `flamejet_ai`.

## Oracle
- Lockstep 25/25 tracks ALL MATCH with all four active (options off; both sacred functions run
  every frame).
- `full_lod_ai` on, track 5, 30 s: 2260 texture uploads vs 2054 with the faithful functions (the
  AI's full pod and rider models).
- Flamejet on: not exercised (needs a taunt input, or an AI past lap 2 behind an opponent).
