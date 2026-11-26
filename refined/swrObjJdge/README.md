# swrObjJdge race timing (refined)

Per-lap times and the race-time clamp.

Retail keeps lap times in five floats of `swrScore` (`results_P1_Lap1..Lap5`, offsets 0x60..0x70),
indexed directly by the lap number. The vanilla menu caps races at five laps, so the index never
leaves the array; with more laps it walks into `total_time` (0x74), the lap counter (0x78) and
`obj_test_ptr` (0x84) and the game crashes within a few laps. Every timer clamps at 3000 s (50:00).

## Boundary
| Retail | Faithful | Refined | Tier | Lap-slot accesses |
|---|---|---|---|---|
| 0x0045ea30 | `swrObjJdge_F2` | `refined_swrObjJdge_F2` | sacred | 8, plus 4 clamp constants |
| 0x00460950 | `swrRace_InRaceTimer` | `refined_swrRace_InRaceTimer` | equivalent (draws `swrUtils_Rand`) | 2 |
| 0x00462320 | `swrRace_InRaceEndStatistics` | `refined_swrRace_InRaceEndStatistics` | equivalent | 1 |
| 0x00439ce0 | `swrObjHang_ResultsMenu` | `refined_swrObjHang_ResultsMenu` | equivalent | 5 (2 writes) |
| 0x0041da00 | `swrMultiplayer_InRace` | `refined_swrMultiplayer_InRace` | equivalent | 1 |

Each refined function is its faithful counterpart with only those accesses changed (they go
through `race_timing_lap()`), so the rest (control flow, float operations, RNG draws) is
identical by construction.

State: `race_timing.c` owns laps 5..255 of each of the 20 racers, and the clamp.

## Contract
- `race_timing_lap(score, lap, num_laps)`: for a race of five laps or fewer, exactly retail's
  address `(&score->results_P1_Lap1)[lap]`, including the one read past the array (the HUD reads
  lap `num_laps` after the finish, which is `total_time`). For longer races laps 0..4 stay in the
  score and later laps live in the store, which is cleared while the racer is on lap 0.
- `race_timing_set_time_cap()`: the clamp, default 3000 s.

## Flaws addressed
| Tag | Finding | Refinement |
|---|---|---|
| LIMIT / UNSAFE | Lap times indexed past a 5-slot array | Store for laps 5..255 |
| HARDCODE | 3000 s clamp as an immediate and an .rdata constant | `race_timing_set_time_cap()` |

## Band-aids retired (swe1r)
- `lap_time_overflow`: no instruction de-indexing. The compensating deltas (`swrObjJdge_F2_delta`
  lap reconstruction, slot-2 mirror, records sentinel) only run when the byte patch is in place
  (the hook DLL). The >5-lap results summary stays in the modding layer and reads the store.
- `race_time_cap`: sets the clamp to 24 h through the API instead of patching three constants.
  `swrObjJdge_UpdateStandings_delta` (finished racers first beyond ~2h46m) still applies.

## Oracle
- Lockstep tracks 5 and 14 with all five functions active: ALL MATCH (3-lap races; judge,
  HUD timer and multiplayer record run every frame).
- Full 25-track lockstep sweep with all five active: ALL MATCH.
- Unattended 3-lap finish on track 16 (autopilot, RNG pinned), refined vs `SWR_REFINED_OFF=1`:
  the results screen shows the same lap times (51.602 / 56.329 / 50.351, total 2:38.291).
- Unattended 6-lap race on track 16: finishes without corruption; the >5-lap summary reads the
  store (best 50.588 on lap 6, worst 56.330 on lap 2, total 5:16.568).
- Not covered yet: the hangar results/records menu (needs input to reach); a >50-minute race
  (the clamp is a plain setting).
