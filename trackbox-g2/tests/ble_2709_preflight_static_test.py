#!/usr/bin/env python3
"""27.09 BLE-only gate: static safety contract, not hardware proof."""
from pathlib import Path
s=Path("trackbox-g2/TrackBox_Gen2/TrackBox_Gen2.ino").read_text()
assert '"2026.11.05.09-BLE-PREFLIGHT-ONLY"' in s
assert "static constexpr bool ENABLE_ITS_DIAG=false;" in s
assert "static constexpr uint32_t BLE_PROBE_MS=15000UL;" in s
a=s.index("bool startEverything()")
b=s.index("void serviceItsShutdown()",a)
section=s[a:b]
assert section.index("if (!ENABLE_ITS_DIAG)") < section.index("setupDiagnosticTransport()")
assert "return true;" in section[section.index("if (!ENABLE_ITS_DIAG)"):section.index("setupDiagnosticTransport()")]
assert "setConnectRetries(0)" not in s
assert "    12,\n    24,\n    0,\n    200" in s
assert "  NimBLEDevice::setMTU(\n    247" in s
assert "NimBLEDevice::setSecurityAuth(\n    true,\n    false" in s
assert 'showMessage("BLE TEST 27.09","AUTO START OFF"' in s
assert "if (state == ST_ERROR && blePairingFailed && !scanNotFound)" in s
assert "client->getLastError()" in s
print("PASS 27.09 preflight source invariants (NO ITS send path, baseline BLE params)")
