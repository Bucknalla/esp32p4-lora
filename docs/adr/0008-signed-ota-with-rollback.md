# ADR-0008: Signed OTA pulled from GitHub releases, with bootloader rollback

- Status: accepted
- Date: 2026-11-22

## Context

The plan puts OTA and recovery in phase 2, before any LoRaWAN logic, so later
phases ship over the network and roll back on their own. The plan asks for
signed images and a health check with rollback. Hardware secure boot and
flash encryption stay off (ADR-0002): both burn eFuses, and the user declined
eFuse burns (ADR-0006).

A first version used a push endpoint (HTTP upload with a bearer token). It
worked, but the upload handler and token were our own code, and the token
travelled in clear text. The user chose ESP-IDF's `esp_https_ota` (pull over
TLS) instead, with images on GitHub releases.

## Decision

- **Transport:** `esp_https_ota` pulls
  `https://github.com/Bucknalla/esp32p4-lora/releases/latest/download/lorawan_gateway-<rev>.bin`
  (`CONFIG_GW_OTA_URL`; rev1 builds get the rev1 file). The source repo is
  public (Apache-2.0), so the gateway needs no GitHub token. TLS checks
  GitHub against the ESP-IDF CA bundle; HTTP is not allowed.
  (First the source was private and releases went to a public files-only
  repo, `esp32p4-lora-firmware`, now archived.)
- **When:** every `CONFIG_GW_OTA_POLL_MIN` minutes (15) and on the console
  command `OTA_CHECK`. A check reads only the image header
  (`esp_https_ota_get_img_desc`, no flash write). It installs when the remote
  app's ELF SHA-256 differs from the running one, is not the image last
  rolled back, and did not fail to install earlier in this boot.
- **Image trust:** "signed apps without hardware secure boot"
  (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA-3072, verify on update).
  `esp_https_ota_finish()` → `esp_ota_end()` checks the image's Secure Boot v2
  signature against the public key in the running image. No eFuse is
  involved. TLS protects the download; the signature decides what may run.
- **Key:** `~/.config/esp32p4-lora/ota_signing_key.pem` on the build machine
  (mode 600), made by `tools/ota_keygen.sh`. Never in the repo (`*.pem` is
  ignored). `tools/build.sh` signs when the key exists, else builds unsigned
  (CI). Public key digest:
  `eafc8c75915007e51630b49bf51818d86d84097a036aa8cb4a1bf6ba0438f8e9`.
- **Releases:** `tools/ota_release.sh vX.Y.Z` builds rev3 and rev1 from
  scratch with that version, checks both signatures, and publishes them with
  SHA256SUMS as a release whose tag points at the (pushed) source commit. It
  refuses a dirty tree unless `--allow-dirty`.
- **Public repo hygiene:** no device identifiers in the repo (the gateway EUI
  and MAC are redacted: Semtech UDP has no authentication, so a known EUI lets
  anyone impersonate the gateway on TTN). Keys and tokens live only in
  `~/.config/esp32p4-lora/`.
- **Rollback:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A new image boots
  once as pending-verify. The health task starts right after the boot report,
  before anything that can hang, and marks the image valid when, for 30 s in
  a row: an IP is up, a check has reached the update URL
  (`CONFIG_GW_OTA_HEALTH_REQUIRE_SERVER`, which proves the image can fetch the
  next fix), and (SX1301 builds) the concentrator started. If that does not
  happen within `CONFIG_GW_OTA_HEALTH_TIMEOUT_S` (300 s), or the board resets
  in the window, the bootloader boots the previous image. The TTN link is
  reported but not required.
- **Boot report:** every boot logs reset reason, running slot and state, a
  rolled-back image, and a stored coredump summary.
- **No anti-rollback** (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` burns eFuses).

## Consequences

- Source and release files are public. The binaries contain no secrets:
  Wi-Fi credentials live in NVS, the signing key never leaves the build
  machine.
- Publishing a release updates every gateway within one poll interval. A
  `--draft` release is not seen by gateways.
- If GitHub is unreachable for the whole 300 s window after an update, that
  update rolls back (and is not retried, because it is "rolled back before").
  A newer release clears that.
- An image that fails to install (bad signature, cut download) is downloaded
  again once per boot, then skipped.
- Rollback and the partition table are in the bootloader and table, which OTA
  cannot change: they arrive only by USB flash. Done once on 2026-11-22.
- An unsigned image on the board refuses every update; recovery is a USB
  flash of a signed build.
- Any image signed with our key is accepted, including an older one, if it is
  published as the latest release. Losing the key means the next update goes
  over USB.
- Someone with physical access can still flash anything over USB.
- A restart while the image is pending verify rolls it back. So the C6 update
  is skipped in that window, and `WIFI_SET`/`WIFI_CLEAR` there roll back.
  App OTA does not update the C6 image (`cp_fw`); a release that needs a new
  C6 version needs a USB flash.
- Flash writes during a download can delay a downlink past its slot. Not
  measured.
- After an OTA, `idf.py app-flash` writes ota_0 while otadata may boot
  ota_1. Use the full `flash` target (it resets otadata).
- Not done yet: coredump upload, task watchdog on every task, persisted fault
  counters, web UI.

## Verification (2026-11-22, bench board, Wi-Fi)

Pull (current design):

| Test | Result |
| --- | --- |
| Header check against the latest release (two GitHub redirects, TLS) | ~3.5 s, "up to date" |
| New image already running, pending verify | first check reached the server, marked valid after 35 s |
| New release v0.2.0-dev.2, `OTA_CHECK` | installed, valid, then "up to date" |
| Release that never confirms (60 s window) | 1.2 MB in 18 s, rolled back, next check "rolled back before" (no loop) |
| Release signed with another key | downloaded, refused at `esp_ota_end` (`ESP_ERR_OTA_VALIDATE_FAILED`), next check "failed before", slot unchanged |

Bootloader and image checks (proven with the earlier push version, same
`esp_ota_*` path):

| Test | Result |
| --- | --- |
| Unsigned image | refused at `esp_ota_end`, slot unchanged |
| Upload cut at 300 KB of 1 MB | slot unchanged, still valid |
| Good image, board reset while pending | bootloader booted the previous slot |
| Image that stops start-up before the update task | health check started at 1.6 s, rolled back at 61 s |
