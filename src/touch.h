#pragma once

// One-shot touch handling; call once per loop().
void handleTouch();

// Re-arm the Settings screen's tap edge after a long blocking operation
// (keyboard modal, geocoding, weather fetch, Wi-Fi scan/join). While the
// operation blocks, handleTouch() does not run, so the "finger up" edge that
// normally clears the arm flag is missed; without re-arming, the first tap
// after the block is swallowed. No-op while a finger is still down.
void settingsTouchRearm();
