# SW_RACER_RE — Multiplayer Roadmap

Working roadmap for multiplayer bugs, quality-of-life, and the lag/stability epic.
All fixes live in the `dinput_hook` Detours/delta layer unless noted. Reverse-engineered
addresses are from the Ghidra DB. Effort: **S** < ~half day, **M** ~1–2 sessions, **L** multi-session.

> **Shared root theme.** Several MP bugs come from the same place: in multiplayer the game
> takes *different state-transition / indexing paths* than single-player, and those paths either
> skip an init step or desync host vs client indices. Select-vehicle-stuck and the trigger-desync
> bug are both instances of this. Worth keeping in mind when triaging new MP bugs.

---

## Epic 0 — Lag & stability (TOP PRIORITY)

**Key finding:** the engine is already **client-local-authoritative** — your own pod's physics
and lap/total timer are computed locally every frame (`swrObjTest_F3` publishes the local `'Locl'`
pod; only `'REMO'` pods read the network; `swrRace_LapCompletion` times the local player locally).
The network never drives your own car. So the reported **framerate drop / 1–2 s hard freeze** is
**frame-time theft / a blocking op on the game thread**, NOT simulation coupling. Per-frame sends
are unreliable fire-and-forget (DP `dwFlags=0`) and receive is non-blocking, so a multi-second
freeze is almost certainly a *single* blocking operation triggered by a network event (peer
timeout, player join/drop, DirectPlay connection cleanup).

**The end goal:** impossible for a client to lag, and total-time integrity preserved even if
dropped. Three parts: (1) no blocking on the game thread, (2) results use the client's *local*
time, (3) a dropped client keeps racing locally to the finish.

| Step | Status |
|------|--------|
| Map netcode model (host-authoritative star, 50ms tick `Main_nut_delay_ms`) | ✅ done |
| Confirm local authority / frame-time-theft diagnosis | ✅ done |
| PumpPackets per-frame cap (bound burst stalls) | ✅ queued — branch `fix/mp-netcode-stability` |
| Game-thread net timing probe (locate the freeze) | ✅ queued — same branch |
| **Capture the freeze on a 2-machine + clumsy session** (`hook.log` `[mp_net]` lines) | ⏳ blocked on home test |
| Targeted fix for the identified blocking op | ⏳ after capture |
| Results/time-integrity: results use local time + graceful drop (ties to Bug #7) | ⏳ |

Test method: two machines + [clumsy](https://jagt.github.io/clumsy/) (Drop/Lag/Throttle filtered
to peer IP; the "Drop 100% for 3s then resume" spike + the NIC-disable drop test are the key repros).
Run the probe build on **both** machines; the freeze may land on the host.

---

## Epic 1 — No-VPN connectivity (replace the DirectPlay transport)

**Problem.** Online play today requires a VLAN/VPN (Hamachi, ZeroTier, Radmin) because the netcode
runs on **DirectPlay** (`IDirectPlay4`, a COM object created in `DirectPlay_Startup` @ `0x486ad0`).
Its TCP/IP service provider discovers hosts via UDP **broadcast** and opens a spread of dynamic ports
while embedding private LAN IPs in its own protocol — none of which survives NAT. A VPN works only
because it fakes a shared LAN subnet. **This epic is the real fix for B6** (can't-see-other-players /
firewall).

**Could we just use [iroh](https://github.com/n0-computer/iroh)?** Not as a drop-in VPN — iroh is an
application-level P2P transport (authenticated QUIC keyed by a public-key `NodeId`, with hole-punching
+ relay fallback), **not** a virtual network adapter, so there's nothing for DirectPlay's IP transport
to bind to. Making DP work unchanged would mean building a TUN adapter on top of iroh (i.e.
reimplementing Tailscale/ZeroTier) — pointless; if "no game changes" were the only goal, just tell
players to use ZeroTier/Tailscale. The viable shape is **embedding a P2P transport into the mod that
bypasses DirectPlay at the `stdComm` seam.**

**Key finding — the transport surface is tiny and already fully reverse-engineered.** Everything the
game needs from the network funnels through ~9 `stdComm_*` functions plus one bridge
(`sithMulti_HandleIncomingPacket` @ `0x404880`). The game keys players by an opaque 32-bit **DPID**,
*never* by IP — so a shim can mint synthetic DPIDs and map them to peer identities, cutting DirectPlay
out entirely below the DPID abstraction. The whole `sithMessage` / `swrMultiplayer_*` protocol above
the seam is untouched.

Critically, **the DirectPlay system-message dependency is only 4 message types** (confirmed in
`stdComm_ProcessSystemMessage` @ `0x487550`), and they're surfaced purely through `stdComm_Receive`'s
integer return code — the shim never has to fabricate `DPMSG_*` structs or even call
`stdComm_ProcessSystemMessage`:

| `stdComm_Receive` ret | Meaning | Game action (`sithMulti_HandleIncomingPacket`) |
|---|---|---|
| `0` | app message | payload at `buf+0x24`, len, sender DPID → dispatch via subtype handler table |
| `2` | player left (DPSYS_DESTROYPLAYERORGROUP) | `sithMulti_ProcessPlayerLost(dpid)` |
| `5` | player joined (DPSYS_CREATEPLAYERORGROUP) | host → `swrMultiplayer_SendPlayerList(dpid,1)` |
| `8` | became host (DPSYS_HOST) | `multiplayer_isHost=1; swrMultiplayer_BecomeHost(...)` |
| `1` | session lost (DPSYS_SESSIONLOST) | (drop) |
| `-1` | no more messages | end pump loop |
| `-2` | error | — |

**The seam — functions the shim must own:**

| Function | Addr | Role today | Shim behaviour |
|---|---|---|---|
| `DirectPlay_Startup` | `0x486ad0` | `CoCreateInstance` DP4 + `EnumConnections` (TCP/IP, IPX, …) | init iroh/GNS endpoint; expose ONE synthetic "P2P (no VPN)" connection |
| `stdComm_InitializeConnection` | `0x486bc0` | bind chosen SP | no-op (transport already up) |
| `stdComm_EnumSessions` | `0x487230` | **LAN broadcast** → fills 32-slot session table | **REPLACE** — no broadcast; populate one session from a pasted host ticket/code |
| `stdComm_JoinSession` | `0x4870d0` | `Open(DPOPEN_JOIN)` from session table | iroh `connect(host NodeId)` |
| host create path (`swrMultiplayer_CreateSession`) | TBD | `Open(DPOPEN_CREATE)` | iroh listen + generate shareable ticket/code |
| `stdComm_CreatePlayer` | `0x4872e0` | DP assigns local DPID | mint local DPID |
| `stdComm_UpdatePlayers` / `EnumPlayersCallback` | `0x4871b0` / `0x4874a0` | `EnumPlayers` → rebuild `stdComm_aPlayerInfos[≤20]{name,DPID}` | rebuild table from the iroh peer set |
| `stdComm_Send` | `0x486ca0` | one `Send(idFrom,idTo,flags,data,size)` | route bytes to peer DPID (fan-out if `idTo==DPID_ALLPLAYERS=0`) |
| `stdComm_Receive` | `0x486cd0` | `Receive` + sysmsg routing | drain our RX queue + synthetic join/leave; return the codes above |
| `stdComm_Close` | `0x487180` | `Close` | tear down endpoint |

**Reliability mapping** (clean — nicer than DP): `dwFlags & DPSEND_GUARANTEED` → reliable ordered
(QUIC stream); else → unreliable (QUIC datagram). The per-frame state broadcast (msg `0x32`) is
non-guaranteed → datagram; lobby/roster/event messages are guaranteed → stream. The send flags flow
down from `sithMessage_SendMessage` @ `0x41b760` → `sithMessage_NetWrite` → `stdComm_Send`.

**Topology.** Already a **host-authoritative star** (host fans `0x32` out to all; clients send up) —
maps 1:1 onto connections: host holds N peer connections and loops them to broadcast; each client
holds 1 (to host). No all-to-all mesh, so no per-pair NAT problem beyond client↔host.

**Library choice — three options on an effort / latency / control spectrum:**
- **derpnet** ([mmozeiko](https://github.com/mmozeiko/derpnet); pure C, single-header, zero deps):
  **relay-only** through Tailscale's free public DERP servers. No NAT problem at all (both peers connect
  *outbound* to a relay), no account / `tailscaled` / TUN, no infra to run, no Rust/protobuf/OpenSSL.
  Peers = keypairs, exchange public keys out-of-band (= our join code). API = async **unreliable datagram
  only** (`DerpNet_Send`/`Recv`, no ACK/ordering). Costs: every packet takes a **relay hop**
  (region-dependent latency — the thing we want to avoid); both peers must pick the same DERP region; leans
  on Tailscale's free relay bandwidth (no stated fair-use). Because it's unreliable-only, the game's
  *guaranteed* messages (join/roster/ready/results) need a thin **reliability shim** (seq+ACK+retransmit,
  ~100 lines) on top.
- **iroh** (Rust): same DERP-relay lineage **plus NAT hole-punching to upgrade to a direct low-latency
  path**, relay as fallback; bundled discovery; reliable (streams) + unreliable (datagrams) built in. Cost:
  Rust↔C++ FFI + a tokio thread in-process — but **the maintainer confirmed Rust is not a blocker** (compile
  a Rust staticlib with a small C interface, link into dinput.dll). Still leans on n0's free relays for
  fallback.
- **GameNetworkingSockets** (C++, Valve): native C++, direct P2P via ICE, per-message reliable/unreliable
  that mirrors DP's flags, built for server-ful topologies + full self-host control. Cost: you run signaling
  + (some) TURN, and the 32-bit MinGW protobuf+OpenSSL build is a chore.

- **Integration is identical for all three** (this answers "how to integrate given it uses DirectPlay"):
  none of them touch DirectPlay — they sit behind the **same `stdComm_Send`/`Receive` shim** (the seam
  above), mapping the game's opaque DPID ↔ the lib's peer key/connection. So *which lib* and *how to
  integrate* are **decoupled**: the how is solved and library-agnostic; the lib is just the pipe behind
  the shim, swappable later without touching the game side.
- **RECOMMENDATION (revised — supersedes the earlier "locked GNS", which predated derpnet + the
  Rust-is-fine confirmation): pick the lib by measurement, not up front.** Use **derpnet for the P0/P1
  spike** — it removes every hard variable at once (NAT, infra, deps, Rust, account), so we prove a real
  cross-internet 2-player race with DirectPlay bypassed in the least possible time, and it may even be a
  fine casual v1. Then **measure the relay latency** (the game is already VLAN-tolerant and runs
  client-side extrapolation, so a racing title may well accept a relay hop). Gate the production lib on
  that number: relay latency acceptable → **derpnet** (+ reliability shim) wins on simplicity; not
  acceptable → **iroh** (lowest-friction direct-path upgrade, Rust now cleared) or **GNS** (full self-host
  + owns the server-ful recording/spectator stack). The transport stays behind the shim, so it's reversible.

## Product topology + UX (split-path: direct play, server-side record/spectate)

**Target experience:** Discord announce when someone hosts, **auto** server-side MP replays (no user
action), public + private + spectator joining — **without** adding latency or bandwidth pain to players.

**Two backends — do NOT conflate them:**
- **App backend** (lobby directory, leaderboards/times, spectator coordination, Discord webhook):
  an ordinary web service + DB; cheap; **transport-agnostic** (you build it identically either way).
- **Connectivity backend** (NAT signaling + TURN relay): the latency/bandwidth-heavy, costly one. This
  design *minimizes* it (below), so it never dominates the bill. "We need a backend anyway" refers to
  the cheap app backend, not this one.

**The split-path insight (dissolves the latency-vs-recording conundrum):** a replay's data == a
spectator's data == **the host's existing per-frame broadcast stream** (`0x32` = all pods'
transform/speed; the same stream the [[replay-system-investigation]] / `REPLAY_ROADMAP.md` capture
plan records). The host already produces it. So stop treating it as one pipe — split by latency-need:
- **Players <-> players: direct P2P** (the latency-critical path). Server NOT in it. **Zero added latency.**
- **Host -> server: one extra (invisible) subscriber** = **+1 copy** of bytes the host already sends.
  The server records the replay AND becomes the spectator source. Negligible host cost, zero player cost.
- **Spectators connect to the SERVER, never the players.** Spectator load = *server* bandwidth
  (cap-able per match), not host upload.
- **Same server = selective relay**, but ONLY for the player-pairs that fail to hole-punch (a minority),
  not a relay for everyone -> relay bandwidth stays low.

**Per-consumer QoS (a knob the split unlocks, and a concrete GNS fit):** players get **unreliable**
low-latency datagrams (a dropped state frame is harmless — next is ~32 ms away); the server gets the
**reliable, complete** copy so the recorded replay has no holes. Same data, two reliability levels,
chosen per-message — exactly GNS's send model.

**Requirements -> where they live:**

| Requirement | Lives in | Cost |
|---|---|---|
| Discord "X is hosting" | app backend fires a webhook when a *public* lobby registers | trivial |
| Auto MP replays (no user action) | server is the invisible reliable subscriber; writes setup + state stream | +1 cheap copy |
| Public joiners | listed in the directory -> matchmake -> connect **direct** to host | direct play |
| Private joiners | unlisted; join by code/invite; Discord announce suppressed | direct play |
| Spectators | connect to the server's per-match feed | server fan-out only |
| No player latency | gameplay is direct P2P; server off the critical path | none |

**Host / discover / join / spectate UX** (replaces DP's LAN `EnumSessions` broadcast):
- **Host create** -> endpoint up + register the lobby with the app backend (visibility public|private,
  join code, spectator policy); if public -> Discord announce fires.
- **Discover** -> public games come from the directory (a real server browser); private games are
  code-only (shared out-of-band: Discord/paste). There is no internet-wide broadcast.
- **Join** -> matchmake/resolve via the server, then connect **direct** to the host.
- **Spectate** -> connect to the server's match feed; never touches the players.

**Caveat / tunable:** the host's **upload** is the scarce resource in any peer-host model. The +1
server copy is tiny, but for very large lobbies / weak host upload, flip to **full server-relay** (host
sends once to the server, the server fans out to everyone) -> trades one hop of player latency for
host-upload relief. A runtime tunable, not a fork to decide now; GNS does both.

**Phased plan:**

| Phase | Goal | Detail |
|---|---|---|
| P0 — loopback spike | prove contract emulation with **no** network | New `stdComm_delta.cpp`: detour `stdComm_Send`/`Receive` with an in-process queue + 2 synthetic DPIDs; confirm a (single-machine, 2-instance) MP session runs with DP's data plane bypassed. De-risks the whole contract before touching any transport lib. **Solo-testable.** |
| P1 — one real link | **derpnet** between 2 machines, manual code | Swap the loopback queue for **derpnet** (pure C, zero deps/infra, no NAT) — the fastest real-internet proof. Host prints its public key as a join code, joiner pastes it (skip `EnumSessions`); per-frame state over unreliable datagrams; resend setup msgs a few times in lieu of reliability for now. Prove a 2-player race over the internet, no VPN. **Then measure relay latency** to pick the production lib (derpnet / iroh / GNS — see Library choice). |
| P2 — lobby integration | wire into the menus | Synthetic "P2P" connection entry; host-ticket UI on create-game; join-by-code field replacing the session list; maintain `stdComm_aPlayerInfos` + synthesize join(5)/leave(2) on connect/disconnect (`swrMultiplayer_SendPlayerList` already fires on join). |
| P3 — robustness | drop / host-migration / scale | disconnect → leave(2)/session-lost(1); optional host migration (8); validate ≤20-player table bounds (shares Q5); reconnect / relay-fallback UX; fold in Epic 0 (run net I/O off the game thread now that we own the transport). |
| P4 — app backend + lobby UX | discovery without LAN broadcast | Coordination server (web service + DB): lobby directory (public browser), private join codes, Discord announce webhook, leaderboards/times. Wire host-create / discover / join into the menus, replacing `EnumSessions`. Transport-agnostic; can start in parallel with P1. |
| P5 — record + spectate | auto replays + spectators | Server subscribes to each match as the invisible **reliable** subscriber; writes setup + state stream to storage (reuses `REPLAY_ROADMAP.md` capture/REMO playback) = auto MP replays. Spectator fan-out + a read-only spectator client mode. Selective TURN relay only for player-pairs that fail to punch. |

**Risks / unknowns:**
- **Async runtime in-process** (iroh): tokio on a background thread, bridged to the game thread via
  lock-free queues; the game thread must never block on it (synergises with Epic 0's "net off the
  frame path").
- **Host create path** not yet decompiled (`swrMultiplayer_CreateSession` / `BuildCreateGameUI` → the
  `Open(DPOPEN_CREATE)` call). Decompile before P2.
- **DPID allocation** must be unique + stable per session (e.g. host=1, clients=2..N in join order);
  audit everything that compares DPIDs.
- **Untestable solo** past P0 — every later phase needs two machines across real NAT.
- **Keep the DirectPlay path** behind an ini/menu toggle (`mp_transport = dplay | p2p`) so VLAN play
  still works and we can A/B.
- **Relay-only libs (derpnet) need a reliability shim**: derpnet is unreliable datagram only, so the
  game's `DPSEND_GUARANTEED` messages (join/roster/ready/results) need seq+ACK+retransmit on top
  (~100 lines). iroh/GNS include reliability natively. Also: derpnet leans on Tailscale's free DERP
  bandwidth (no stated fair-use) and both peers must share a DERP region — fine for the spike + hobby
  scale, reassess before making it a popular mod's permanent backbone.
- **GNS build for 32-bit MinGW** (only if GNS is the production pick): GNS depends on **protobuf** + a
  crypto lib (OpenSSL or libsodium); building those as a 32-bit MinGW static lib is a genuine chore.
  Neither the P0 loopback spike nor the P1 derpnet spike needs it, so it's deferred until/unless GNS wins.
- **Server-side workstream is its own project** (P4/P5): hosting, DB, ops, Discord app registration,
  TURN bandwidth budget. Independent of the in-game transport; can be sequenced separately.

---

## Bugs

| # | Bug | Effort | Status / root cause / locus |
|---|-----|--------|------------------------------|
| B1 | Network update delay default 50ms too conservative; want live tuning | S | `Main_nut_delay_ms` @ `0x004b6718` (binary default 32). Already settable at launch via **`-nut <ms>`**. TODO: live imgui knob + ini. Nuance: lower = more frequent *synchronous* broadcasts; sweet spot is empirical → live knob > forced default. |
| B2 | Players can't join while host not focused (lobby hangs) | M | Hypothesis: host throttles/pauses its main loop or packet pump on focus loss → join handshake stalls. Locus: window focus/activation gate on the update loop / `swrMultiplayer_PumpPackets` cadence. Investigate WM_ACTIVATE handling. |
| B3 | Lobby text entry locks cursor until Enter | M | Text-entry mode captures input with no cancel path. Locus: swrUI text-entry / lobby chat input handler. Add Esc/click-out to exit text mode. |
| B4 | Host can't start race while players selecting (by design) — want a countdown | M | Feature. Host-gated start + broadcast countdown + UI. Depends on lobby/ready state (`swrMultiplayer_*ReadyState`). |
| B5 | Alt-tab → camera faces backward until Tab pressed (NOT MP-specific) | M | Look-back (Tab) state not reset on focus loss/regain. Locus: camera look-back input state + focus handling. See `camera_cman_subsystem`. |
| B6 | Some clients can't see other players (suspected firewall) | L | DirectPlay peer reachability. Confirm topology (host-relay vs peer-to-peer); if peer-to-peer connections are required, blocked pairs go invisible. Mitigation: force host-relay and/or document required ports (TCP 47624, TCP/UDP 2300–2400). Mostly config + some code. **→ Properly solved by Epic 1** (the no-VPN P2P transport removes the firewall/NAT requirement entirely). |
| B7 | Final time shows `00:00.000` on results; can't return to results once left | M | Final time/standings appear host-synced (client shows 0 when not received). **Ties directly to the Epic 0 integrity goal.** Fix: results use the client's locally-accumulated time; allow re-viewing results. Locus: `swrObjHang_UpdateResultsIntro` / `swrRace_ResultsMenu` / standings sync. |
| B8 | **NEW:** wrong in-level event triggers in MP (e.g. Oovo "rotate door" fires as "asteroid smash") | M | **Root cause (grounded):** triggers are identified by **index** into the per-track trigger list `DAT_00e25ac0` (count `DAT_0050cb54`). Host: `swrObjTrig_FindTriggerDescriptionIndex` → `swrMultiplayer_SendEvent('trig', index)`. Client: `swrObjTrig_CreateAndActivateTriggerFromMultiplayerEvent` → `swrObjTrig_GetTriggerDescription(index)`. If host/client build the list in different order/membership, the index resolves to the wrong trigger. Locus: `swrObjTrig_FindAndInitializeTriggersInNode` / `swrObjTrig_AddTriggerDescription` (verify deterministic, identical list construction across SP/MP and host/client). Possible fix: send a stable trigger ID instead of an array index. |

---

## Quality of life

| # | Item | Effort | Notes / locus |
|---|------|--------|---------------|
| Q1 | Customize pod upgrades in MP | M–L | MP load path forces fixed upgrades (`FUN_0045b290` MP branch sets fixed values). Allow upgrade selection in MP. |
| Q2 | Sebulba flamejet doesn't affect other players in MP | M | VERIFIED 2026-06-16. The flamejet emitter `swrRace_UpdateEngineDamageFX` (0x46f9a0, run per-pod from `swrObjTest_F3`) is identity-blind: it `FindNearestObjects('Test',...)` and sets `target->engineStatus[engine] |= 8` on any nearby pod incl. the REMO proxy of a remote human. But that write is DEAD — REMO proxies aren't simulated locally, and the engine-fire->health drain `swrRace_ApplyEngineDamage` (0x46aa30) only runs on the LOCL path (sole caller `UpdatePlayerControl`, gated `flags0 & 0x20` in `CalcTargetTurnRate` 0x46cf00). The existing `'flam'` net event (SendEvent 0x41df10 / ApplyEvent 0x41e260 subtype 0x17 -> FUN_0046bb50 -> `swrRace_SpawnExplosionEffect`) only syncs the explosion/burst VISUAL, not damage. FIX (no physics/damage-gating changes — victim processes it natively since it's LOCL on its own machine): (1) hook the emitter so when the hit pod is REMO, send a NEW event `{victimPlayerIndex, engineIndex}` via the SendEvent/ApplyEvent plumbing instead of the dead local write; (2) receive handler sets `engineStatus[engineIndex] |= 8` on the LOCAL pod -> existing ApplyEngineDamage + full-model fire render kick in for free. Apply-to-self on receipt = desync-safe (client-local-authoritative). Validate/relay via host (anti-cheat: confirm attacker is Sebulba + in range); make opt-in/host-toggled (pairs with Q3); suppress the attacker's dead local write to avoid double-apply. All in `swrMultiplayer_delta` + a `UpdateEngineDamageFX` hook. See [[ai_fidelity_lod_subsystem]] (single-player AI variant needs the opposite: change ApplyEngineDamage's LOCL-only gating) and AI_ROADMAP.md item 4. |
| Q3 | Toggle racer-to-racer collision (default OFF in MP) | S–M | Find the racer-racer collision flag, expose a toggle, default off in MP (low NUT / poor collision logic). |
| Q4 | Full-LOD opponent racers | ? | Likely covered by `ai_fidelity_lod_subsystem` — cross-reference / may already be solved. |
| Q5 | Increase max player count | L / risky | Bounded by fixed-size per-player arrays (e.g. state stride `0x1f28`, several `0x14`=20-slot loops). Raising requires auditing every per-player array bound — overflow risk. |
| Q6 | Add AI racers to MP races | L | Promising: the `'AAII'` (AI) pod tag is already handled in the MP publish path (`swrObjTest_F3` publishes for `'Locl'` *or* `'AAII'`). So pods support it; investigate roster/`SpawnRacers` to actually populate AI in MP. |
| Q7 | Show player names above opponents instead of placement (+ toggle) | M | HUD draws placement label above opponents; swap to player name + add toggle. Locus: opponent nametag draw (see `weather_hud_investigation` / swrPlayerHUD). |
| Q8 | Host-toggleable catch-up (rubber-band) for trailing players | M | The catch-up multiplier already exists (`swrRace_UpdateCatchup` `0x0046ce30`: `1.0 + gap*L/5000`, cap **1.25x**, `gap = +0x130` = `leaderRank - thisRank` written by `swrObjJdge_UpdateStandings` `0x0045d4a0`), but it's gated to `NumLocalPlayers() > 1` so it ONLY runs in 2-player splitscreen — online players (NumLocalPlayers==1 per machine) get none. FIX: lift/extend the gate so the local human applies it when in MP. Each client computes its OWN deficit from `+0x130` (already populated for every pod by UpdateStandings via the host's standings broadcast) and applies its own `multiplayerStats` boost -> **desync-safe** (the speed multiplier is client-local-authoritative; no remote state written). Host toggles on/off (+ optional strength) via the race-settings sync (`swrMultiplayer_BroadcastRaceSettings` 0x3a -> `ApplyRaceSettings`), like laps/track. NOTE: `multiplayerStats` also tightens grip via `swrRace_ApplyTraction` (`2.0 - mult`), so strength affects handling too. Same feature family as the single-player AI-runaway fix (`AI_ROADMAP.md` C5) and the local-co-op toggle (`LOCAL_MULTIPLAYER_ROADMAP.md` "Catch-up"). Locus: `swrMultiplayer_delta` (race-settings field + gate) + `UpdateCatchup` hook. |

---

## Already shipped this session (queued for review/test, separate branches)

| Item | Branch | Status |
|------|--------|--------|
| MP select-vehicle stuck (non-host can't change racer after a race) | `fix/mp-select-vehicle-stuck` | ✅ committed, builds; needs 2-player test |
| Renderer perf: gate per-frame `glFinish` + vsync toggle | `perf/remove-per-frame-glfinish` | ✅ committed, builds; needs in-game test |
| MP pump cap + net timing probe | `fix/mp-netcode-stability` | ✅ committed, builds; needs 2-player + clumsy test |

---

## Suggested sequencing

**Wave 1 — finish what's queued (needs home/2-machine testing):**
verify the three branches above; capture the freeze with the probe; land the targeted stability fix.

**Wave 2 — high-value, lower-risk, mostly independent:**
B1 live `-nut` knob (S), Q3 collision toggle (S–M), Q7 names-above-racers (M), Q8 host-toggle catch-up
(M, desync-safe, reuses existing infra), B8 trigger desync (M), B3 lobby cursor lock (M),
**Epic 1 P0** (loopback shim spike — solo-testable, de-risks the no-VPN transport work).

**Wave 3 — integrity + medium bugs:**
B7 results/time integrity (M, pairs with Epic 0), B2 join-while-unfocused (M), B5 alt-tab camera (M).

**Wave 4 — bigger features / risky:**
Q1 MP upgrades, Q2 flamejet, Q6 AI in MP, **Epic 1 P1–P5** (no-VPN P2P transport — derpnet spike then
measure to pick the production lib — + the lobby/replay/spectator server; subsumes B6, gives auto MP
replays; needs 2-machine testing), Q5 max players (riskiest).
