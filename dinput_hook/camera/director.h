#pragma once

// Camera director: chooses which pod the in-race camera-man follows. Cuts are the game's own
// 'NAsn' reassignment (the event swrObjHang_AssignRacerCameras sends at spawn), so every cut lands
// on a freshly initialized chase camera and the stock spectator-mode cycling keeps running on the
// new pod. Only acts when nobody local is racing (it would hijack a human's camera otherwise).
//
// Auto policy: dwell on a target, then pick the next one from {leader, closest battle, random};
// cut away at once from a pod that finished or crashed. A manual pick (leaderboard click, panel)
// holds for manual_hold_s before auto resumes.

void director_RegisterPanel();

// Per-frame on the game thread (after the telemetry snapshot is refreshed).
void director_Service();

// Consumer switch (the race orchestrator turns the director on while its loop is armed).
void director_SetEnabled(bool on);
bool director_IsEnabled();

// Follow a racer by swrScoresPtr slot now (manual pick). -1 = let auto choose again.
void director_FollowSlot(int slot);
// Cut to a racer on the stock chase view without the manual hold (pre-race grid showcase).
void director_Showcase(int slot);

// Slot currently followed by camera-man 0, or -1.
int director_FollowedSlot();
