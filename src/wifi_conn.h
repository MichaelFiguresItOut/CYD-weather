#pragma once

// Connect/reconnect to WiFi; idempotent (returns true if already up).
bool wifiEnsureConnected();
