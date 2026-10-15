// Keeps the eight-voice mixer alive across an unattended run of races. See sound_watchdog.cpp.
#pragma once

// Per-frame sweep: recover voice slots the game's own mixer can no longer free.
void sound_watchdog_Service();

// Release every dedicated source we can account for, then reset all eight channels. Backs the
// "Reset sound channels" button.
void sound_watchdog_ForceReset();

void sound_watchdog_RegisterPanel();
