# Times & Records Roadmap

**Status:** design (2026-09-10). Living document. Owner: lightningpirate.
**Scope:** a times system for **every** track, stock or custom, that records enough about a run to
be trusted, compared, and later replayed as a ghost. Grew out of the custom-track sidecar
(`CUSTOM_TRACKS_ROADMAP.md` M6) once it was clear the same machinery is worth having on stock
tracks. Related: run-verification notes (trust), `REPLAY_ROADMAP.md` (ghosts), and the merged #273
which reimplemented the timing pipeline this sits on.

---

## 1. Why the game's own records are not enough

The save image holds exactly one number per track per record kind: `record3LapTimes[50]` and
`recordLapTimes[50]`, indexed `bMirror + track_index * 2`, plus a holder name and a pilot id. That
is all. It cannot say:

- which pod the time was set on, beyond the pilot;
- what the pod was carrying -- upgrade levels change the physics outright;
- what condition those parts were in (a damaged part performs worse, so a time set on fresh parts
  is a different achievement);
- how many laps, beyond the 3-lap / best-lap split, which the 100-lap work makes meaningful;
- anything about the machine it ran on;
- how the run was actually driven, which is what a ghost is.

There is also no room to extend it: `swrSaveData` is a fixed 0xfd4 bytes with a CRC32 over it, and
custom tracks have no slot at all (racing one used to corrupt the image -- #306).

So: a separate store, which never touches the save image, and which is a **superset** of what the
game records. The game keeps its own records and keeps displaying them; nothing about vanilla
behaviour changes.

---

## 2. The one design decision that matters: key vs payload

A record is `key -> best run`. Everything hangs on which facts *split* the records and which
merely *describe* them.

- Put too much in the key and nothing ever beats anything: if part health is part of the key, a run
  on 98%-health parts and one on 97% are different records, and there is no leaderboard, just a
  list of runs.
- Put too little in the key and incomparable runs fight for one slot: a fully-upgraded pod beats a
  stock one every time, and the stock time is gone.

**Decision.** The key is what the community already splits on, because that is the proven answer to
"which times belong on the same board":

```
key = (track identity, mirror, laps, upgrade class)
```

`upgrade class` starts as the coarse split the boards use (stock vs upgraded); the exact levels
live in the payload, so a finer split can be derived later without re-recording anything.

Everything else is **payload**: evidence about the run, carried with the record.

```
payload = pod + pilot + upgrade levels[7] + part health[7] + lap splits[]
        + fps (min/avg) + duration + date + build id + (later) ghost hash
```

The rule of thumb: **if two runs with different values should appear on the same board, it belongs
in the payload.**

### Track identity

- Custom track: `slug` + `content_hash` (a re-published track is a different track to race).
- Stock track: the canonical `vanilla:track:NN` slug, plus the hash of the model and spline entries
  actually loaded. That last part is deliberate -- it is what tells a leaderboard the run happened
  on unmodified geometry, and it costs one hash of data already in memory at load.

---

## 3. Why record parts, health and fps

Not for their own sake -- each answers a question a leaderboard or a suspicious reviewer will ask:

| Field | The question it answers |
|---|---|
| upgrade levels[7] | Was this pod faster than the one I raced? (traction/turning/accel/top speed/airbrake/cooling/repair) |
| part health[7] | Were the parts fresh? A damaged part is slower, so a worn-parts run is a harder run |
| pilot / pod | Pods differ in stats; a board usually splits or at least shows it |
| lap splits | Where the time went, and the only way a "best lap" claim can be checked against the race it came from |
| fps min/avg | The physics is fixed-timestep since #194, but a run recorded at 12 fps or 900 fps is still worth flagging |
| build id | Which mod build produced it, so a later behaviour change can be accounted for |
| ghost hash | The run itself (section 5) |

---

### 3.1 Point-to-point tracks (found by playtest 2026-09-10)

Not every track has laps. Bomb-omb Battlefield is point-to-point: it ends after one traversal
however many laps the menu asked for. That broke two assumptions worth writing down, because
anything built on this store will meet them again:

- **An unrun lap does not read back as zero.** The results sanitizer clamps out-of-range lap times
  to the empty-record value, so a one-traversal run on a three-lap setting produced splits of
  `[63.598, 3599.99, 3599.99]`. Stopping the split scan at `<= 0` is not enough; stop at the
  sentinel.
- **The requested lap count is not the run's lap count.** Keying on the menu setting would file a
  one-traversal run against three-lap runs of a track that has no laps, and would split two
  identical runs made under different settings. The key now takes the laps the run actually had.

The reading side follows: the course-info screen cannot know in advance that a track ends early, so
it looks for the exact key and then for the only record the track holds under those conditions. A
track that genuinely holds records at several lap counts always has the exact match, so the
fallback never fires there.

**Resolved 2026-09-10:** whether the spline loops is decided when a track is converted (walk the
control points from the start and see whether they come back) and carried in the manifest, so a
catalog entry knows before the track is downloaded -- which is what install-on-first-play needs. The
lap row then shows a dash and refuses to change, and the record draws as "Record" rather than a
heading about laps it does not have. Two of the nine packs on hand are point to point (bomb-omb
battlefield, bigroad).

**No stock track is point to point** (confirmed by Lou), so the flag only ever comes from a
manifest and stock tracks need no spline walk at runtime.

Left over: "Record" is an untranslated literal where the neighbouring headings are localized
strings; it wants a real string entry before this ships.

## 4. Phases

| Phase | Scope |
|---|---|
| **T1** | The store, generalized: every track records, stock included; key as section 2; payload = pod, pilot, upgrade levels, part health, lap splits, fps, date, build id. Vanilla display untouched; custom tracks keep the course-info display already built. |
| T2 | Granularity: per-lap-count records (the 100-lap work makes "best 5-lap" a real thing), and lap splits captured for runs longer than the 5 the score struct holds -- that needs the lap-completion event rather than the results screen. |
| T3 | Display: a records screen that can show more than two numbers, and shows the payload (pod, parts, splits) behind a record. The stock course-info screen has room for two columns and no more. |
| T4 | Ghosts: record the run, replay it against the record. See section 5. |
| T5 | Submission: the backend ingest exists (M7, 2026-09-15, takes the sidecar verbatim) and a record was pushed by hand. What remains is the game doing it: sign in via the device link in `CUSTOM_TRACKS_ROADMAP.md` 4.3, then each improved record gets `submitted: false` and the worker POSTs it and flips the flag - the sidecar is the outbox, so offline runs catch up on the next boot. |

---

## 5. Ghosts, and why the store is shaped this way

A ghost is a recording of the run that set a record, so it belongs *with* the record, not in a
parallel system. Two things make that cheap here:

- The content store is already content-addressed, so a ghost is one more blob: the record gains a
  `ghost` hash, the blob holds the recording, and dedup, download and garbage collection work on it
  unchanged.
- `REPLAY_ROADMAP.md` established that the recording problem is pod-state capture (the MP
  `PublishPodState` path already serializes what a ghost needs), and that the hard part is
  playback fidelity, not storage.

So T4 is: capture on a record-setting run, store as a blob, and play back. Nothing above needs to
change to allow it -- which is the point of settling the shape now.

---

## 6. Open questions

- **Upgrade class granularity.** Stock vs upgraded is the boards' split today. If the community
  wants per-level boards, the payload already carries the levels; the key would gain a class field.
  Worth asking the leaderboard people before assuming.
- **Multiple runs per key.** The store keeps the best. A ghost of the *second* best, or a history,
  needs a list per key -- cheap to add, but only if anyone wants it.
- **Where a stock-track record displays.** T1 records silently and leaves the vanilla screens
  alone, because those records are part of the save and players expect them. A richer records
  screen is T3.
