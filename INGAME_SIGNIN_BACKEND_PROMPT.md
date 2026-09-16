# Prompt: in-game Discord sign-in for SW_RACER_RE (botto-api + junkyard)

Paste into a session in `bottos-junkyard` (botto-api); the junkyard half is the last section. The
game client is being built in parallel against the exact contract below, so treat the request and
response shapes as frozen; anything not specified is yours to decide.

---

## Context

The SW_RACER_RE mod can now download community tracks from `/api/v1/customtracks` and records
times into `assets/custom_times.json` (schema 2, the format `POST /customtracks/times` already
accepts verbatim). The first end-to-end run finished with a manual step: dragging that file onto
`/customtracks/times`, because **the game has no identity** -- the site holds a 7-day HS256 JWT
from the Discord OAuth code flow; the game holds nothing.

Discord does not implement the OAuth device grant (RFC 8628), and a 1999 executable cannot host an
OAuth redirect. So: **the website's existing login is the approval surface, and botto-api mints the
game a token of its own.** Build it once as a device link; record submission uses it now, publish
from the game uses it later by adding a scope.

```
game                          botto-api                         junkyard (browser)
 |-- POST /auth/game/start -->|                                  |
 |<-- {code, poll_token,      |                                  |
 |     verify_url, ...}       |                                  |
 |-- opens verify_url?code=XXXX in the default browser --------->|  user is (or logs) in
 |                            |<-- POST /auth/game/approve ------|  "Link SW_RACER_RE on
 |                            |    {code}   (web JWT)            |   <host>?"  [Approve] [Deny]
 |-- POST /auth/game/poll --->|                                  |
 |   {poll_token} every 3s    |   428 pending | 410 expired/denied
 |<-- 200 {token, user} ------|                                  |
```

The displayed `code` and the `poll_token` are **different secrets**: someone who sees the code on
a stream cannot claim the token; someone holding the poll_token cannot approve.

## The game token is a JWT

Sign it with the same `JWT_SECRET`, so `server/middlewares/authMiddleware.js` branch 1
(`Authorization: Bearer <jwt>` -> `req.user`) accepts it and **no customtracks route changes**.
Claims:

```json
{ "id": "<user id>", "username": "...", "source": "game", "scope": ["times:submit"],
  "jti": "<random>", "iat": ..., "exp": <iat + 180 days> }
```

Middleware: when `decoded.source === 'game'`, look up `gameTokens/{jti}`; 401 if missing or
`revoked`; otherwise set `req.auth.scope` and touch `lastUsedAt` (throttle the write, e.g. once per
hour per token). Add a tiny `requireScope('times:submit')` and put it on `POST /customtracks/times`
**only for `source === 'game'`** -- web sessions keep working as today. Everything else the game
token could reach (publish, blob PUT) must be refused for `source === 'game'` until a
`tracks:publish` scope exists; refuse by default, allow by scope.

## Firestore

- `gameLinkCodes/{code}`: `{ pollTokenHash, client, build, host, createdAt, expiresAt (+10 min),
  status: pending|approved|denied|claimed, userId?, approvedAt? }`. Store a hash of the poll
  token, not the token. TTL-delete or sweep expired docs.
- `gameTokens/{jti}`: `{ userId, client, build, host, createdAt, lastUsedAt, revokedAt? }`.

## Endpoints (contract frozen; the client is coded against these)

All under the existing `/api/v1` mount. JSON in and out. Rate-limit `start` and `poll` like the
blob routes (they are anonymous).

**`POST /auth/game/start`** -- anonymous.
Body: `{ "client": "SW_RACER_RE", "build": "feature/x@6db49ce5", "host": "DESKTOP-ABC" }`
(`host` is a machine name the user will recognise on the approval page; treat as untrusted text,
cap at 64 chars.)
200: `{ "code": "ABCD-1234", "poll_token": "<opaque, >= 32 random bytes>",
        "verify_url": "https://bottosjunkyard.com/link?code=ABCD-1234",
        "expires_in": 600, "interval": 3 }`
Code alphabet: uppercase letters + digits without `0/O/1/I`, 8 chars, one hyphen. The client only
opens `verify_url` if it starts with `https://`.

**`POST /auth/game/poll`** -- anonymous. Body: `{ "poll_token": "..." }`
- 200 `{ "token": "<game jwt>", "user": { "id": "...", "username": "...", "avatar": "..."? } }`
  -- exactly once; mark the code `claimed` so a replay gets 410.
- 428 `{ "status": "pending" }` -- not approved yet.
- 429 `{ "status": "slow_down" }` -- client doubles its interval.
- 410 `{ "status": "expired" }` or `{ "status": "denied" }`.
- 400 on an unknown poll_token.

**`POST /auth/game/approve`** -- web JWT (`authenticate`, `source === 'web'` only; a game token
must not approve links). Body `{ "code": "ABCD-1234" }`. 200 `{ "client", "host", "build" }` on
success; 404 unknown code; 410 expired/already used. Binds `userId`, mints nothing yet -- the token
is minted at the first successful poll, so it is only ever delivered to the holder of poll_token.
**`POST /auth/game/deny`** -- same auth, same body, sets `denied`.

**`GET /auth/game/me`** -- game token. 200 `{ "user": { "id", "username", "avatar"? },
"scope": [...] }`; 401 when revoked/expired. The client calls this once at boot to learn whether
its stored token still works.

**`POST /auth/game/revoke`** -- game token. Revokes *itself* (`revokedAt` on its own jti). 204,
idempotent. Called on sign-out.

**`GET /auth/game/devices`** / **`DELETE /auth/game/devices/{jti}`** -- web JWT; the user's own
linked games for the profile page (client, host, build, createdAt, lastUsedAt).

**`POST /customtracks/times`** -- already exists. Two small asks:
1. **Ignore unknown fields on a record.** The client now writes a `submission` field
   (`"pending" | "done" | "rejected"`) into each record of `custom_times.json` as its own outbox
   state and sends records verbatim; it must not be a validation error.
2. Keep the current behaviour that the body may contain records for slugs that are not published
   (a player's file mixes local and published tracks). The response should say per record what
   happened: `200 { "accepted": n, "ignored": n, "improved": n, "results": [ { "slug",
   "content_hash", "laps", "mirror", "upgrades", "category", "status": "accepted|ignored|
   not_improved|unknown_track" } ] }`. 401 as today; **422 only for a malformed body**, never for
   "this track is not on the server" -- the client marks a 422 batch `rejected` and stops
   retrying it, so a 422 must mean the data itself is wrong.

## junkyard

- **`/link`** page (`src/pages/link/` or wherever routes live): reads `?code=`; if not signed in,
  runs the normal Discord login and returns here with the code intact; shows
  "Link **SW_RACER_RE** on **<host>** (build <build>) to your account?" with **Approve** / **Deny**
  calling the two endpoints; result states for success, expired, unknown code. Plain and fast --
  the game is polling on the other side, and the user just alt-tabbed out of it.
- **Profile page**: a "Linked games" list from `GET /auth/game/devices` with a revoke button per
  row. That is the user's kill switch for a token sitting in plaintext on a PC they no longer own.
- Add an `authFetch`-style helper for the two approval calls if the existing one does not fit.

## Tests

`node --test` next to the existing customtracks tests: start -> poll pending -> approve -> poll
claims once -> second poll 410; deny -> 410; expiry -> 410; revoke -> `me` 401 -> `times` 401;
game token cannot approve; game token cannot publish (scope); web token still can. Times ingest:
record with `submission` field accepted; mixed known/unknown slugs -> 200 with per-record status.

## Do not

- Do not put the game token in a cookie or localStorage on the site; it never touches the browser
  (only the code does).
- Do not log tokens or poll_tokens. Log code + host + outcome.
- Do not change the download contract (`index.json`, `blobs/`) or the ingest's accepted body.

When done, update the "Custom Tracks" section of both CLAUDE.md files with the auth flow and the
scope rule, and report the exact `verify_url` origin you deploy with (the client currently assumes
`https://bottosjunkyard.com/api/v1` as the API base via the `tracks.api_url` ini key).
