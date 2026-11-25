# ADR-0025: Concentrator cards, fixed-channel regions, CN470 and EU433

- Status: accepted (R3: host-tested; on the board: the 915 MHz card setting
  in US915 and AS923-1, receive-only, transmit chain off; EU868 unchanged)
- Date: 2026-11-25

## Context

ADR-0024 made the region a setting for the dynamic-channel regions. The owner
also wants concentrator cards for 915, 470 and 433 MHz. An SX1301 card is
built for one band: SX1257 radios for 868 and 915 MHz, SX1255 radios for 433
and 470 MHz, and band filters. The firmware assumed an 868 MHz card: SX1257,
the Nebra EU868 RSSI offset, and the transmit chain on radio 0.

The SX1301 cannot tell an 868 MHz card from a 915 MHz card: the radio is the
same. The bench card is a 915 MHz card (GL5712-UX, US915 variant), used so far
for 868 MHz desk tests.

With the transmit chain enabled, `lgw_start` runs the TX DC-offset
calibration on that radio at its centre frequency (`loragw_hal.c`, calibration
command bits 2 and 3). Until now the chain was enabled in every region. On
this bench nothing may transmit at 902-928 MHz.

## Decision

- **The card is a setting.** It is stored in NVS as `radio/card`, and
  `card=868|915|433|470` sets it on `POST /api/radio` together with the
  region. If the key is absent, the card is the one that serves the stored
  region (`lwc_card_for_region`), or 868 when no region is stored, so an
  upgrade changes nothing. An R2 gateway set to AS923 or KR920 gets the 915
  card and stays receive-only. It does not fall back to EU868, where it
  could transmit. Each card has:
  - a band: 863-870 MHz, 902-928 MHz, 433.05-434.79 MHz or 470-510 MHz;
  - a radio type: SX1257, or SX1255 for 433 and 470;
  - an RSSI offset: -158 dB for 868, as before. The others come from the TTN
    frequency plans: -166 dB for SX1257 at 915, and -176 dB for SX1255.
- **The card limits the region.** The card must hold every channel of the
  region: uplink, downlink and RX2 (`lwc_card_region_ok`). Each region fits
  exactly one card:

  | Card | Regions |
  | --- | --- |
  | 868 | EU868, IN865, RU864 |
  | 915 | US915, AU915, AS923-1..4, KR920 |
  | 433 | EU433 |
  | 470 | CN470 |

  The API refuses a card and region pair that does not fit. The UI lists only
  the regions that fit the selected card. If the stored region does not fit
  the card, the gateway starts in the card's default region: EU868, US915,
  EU433 or CN470.
- **The plan check also takes the card.** The checks keep the R2 order, and
  the card's band is checked last (`LWC_PLAN_ERR_CARD`). If the card cannot
  serve the region at all, the check fails first with `LWC_PLAN_ERR_REGION`.
  The EU868 863-870 MHz check is unchanged.
- **Transmit:**
  - The transmit chain is enabled only for EU868 on the 868 card. Everywhere
    else, `lgw_start` runs no TX calibration.
  - The radio task refuses every frame outside that pair with `TX_REGION`.
  - This adds to ADR-0024. For example, AS923 and KR920 (R2) on a 9xx MHz
    radio no longer calibrate TX there.
- **Fixed-channel regions** (US915, AU915, CN470):
  - **One sub-band.** An SX1301 listens on 8 channels at 125 kHz, plus one
    500 kHz channel in US915 and AU915.
  - **Default plan.** The default sub-band is the one in the TTN frequency
    plans: FSB2 for US915 and AU915, FSB11 for CN470. It uses the same
    radios and channels as the TTN plans:
    - 4 channels on each radio;
    - with a 500 kHz channel, radio 0 sits on channel 2, so the 500 kHz
      channel fits at IF +300 kHz.
    - The test checks this layout against the plan files.
  - **Changing the sub-band.** `subband=N` on `POST /api/radio/plan` (1-8,
    CN470 1-12) stores that sub-band's plan.
  - **Plan check.** Every channel must be on the region's uplink grid. The
    std channel must be one of the region's 500 kHz uplink channels at an
    uplink DR, and FSK must be off. Other layouts pass too: ChirpStack's US915 layout puts
    5 channels on radio 0.
- **CN470 and EU433 tables:**
  - **CN470.** The reference keeps the original CN470 plan: 96 uplink
    channels at 470.3 + 0.2n MHz, 48 downlink channels at 500.3 + 0.2n MHz,
    and RX1 on downlink channel (uplink % 48). That plan belongs to LoRaWAN
    1.0.2rB and RP002-1.0.0. RP002-1.0.1 replaced it with new plans and
    sizes, so the generator takes CN470 from the reference's 1.0.2 entry:
    DR0-5, 51 bytes at DR0. Taking it from RP002-1.0.3 would mix the old
    channels with the new sizes.
  - **EU433** is a dynamic region (433.175, 433.375 and 433.575 MHz), so the
    R2 code serves it unchanged.
  - **The other ten regions** are byte-identical in the generated tables.
- **The local network server does not serve fixed-channel regions.**
  `lwc_ns_set_region` refuses them and leaves its state unchanged. The local
  server then handles no frame, so hybrid mode still forwards every frame to
  the network server. Sessions are marked not joined in RAM, as in ADR-0024.
  The UI hides the receive-window form and says why. Reasons:
  - Nothing can transmit outside EU868, so a join could never complete.
  - The server code still assumes DR = 12 - SF at 125 kHz. That is wrong in
    US915, where DR0 is SF10.
  - CN470's original plan has no CFList. Moving a device to one sub-band
    takes 6 LinkADRReq commands, which do not fit in FOpts.
- **Basics Station:**
  - **DNONLY flag.** `lwc_lbs_router_drs_ok` no longer compares it. For
    US915, The Things Stack sends 0 on DR8-13. ChirpStack sends 1, except 0
    on DR12, which repeats DR4's SF8 at 500 kHz.
  - **SF8 at 500 kHz.** An uplink with this pair maps to DR4, not to DR12.
  - **Fixtures.** The tests use ChirpStack's US915 `router_config` test
    fixture, and The Things Stack's DR list with TTN's FSB2 plan.

## Consequences

- The gateway can receive and forward with a 915, 433 or 470 MHz card, but
  only through a network server.
- The bench card is a 915 MHz card. It stays set to 868 for the approved
  868 MHz desk tests (ADR-0007). Its receive at 868 MHz is about 60 dB down.
- The SX1255 settings (433 and 470 MHz cards) are not tested on hardware:
  the bench has an SX1257 card. Configuring an SX1257 card as SX1255 would
  tune it outside its range, so it was not tried.

## Open questions

- **RSSI offsets** come from the TTN frequency plans and are not measured for
  any card. The 868 MHz value is still Nebra's.
- **CN470 plans.** The RP002-1.0.1+ plans (20 MHz and 26 MHz, A and B) are
  not in the reference.
- **Local server in the fixed regions:**
  - joins: the CFList sub-band mask for 1.0.3+ devices, none for 1.0.2;
  - RX1 on the downlink grid;
  - ADR and RX2 through the region table instead of 12 - SF;
  - the two LinkADRReq sub-band masks;
  - all of this goes with their transmit rules (R4).
- **R4:** per-region band edges, duty cycle, dwell time and LBT.

## Verification (2026-11-25)

| Test | Result |
| --- | --- |
| `test_golden_eu868` | unchanged |
| `region_tables.inc` regenerated: the 10 existing regions | byte-identical except the new channel-count fields of US915 and AU915 |
| `test_region` CN470/EU433 spot values | 96/48 channels, RX1 = channel % 48, DR0 51 B, the same DRs both ways, no CFList or LinkADRReq sub-band set |
| `test_region` card check | each region on exactly one card (table above), found by `lwc_card_for_region` (AS923-1, KR920: the 915 card); band edges at the channel edge |
| `test_region` fixed-plan RX1 check | every fixed-plan uplink channel maps to its downlink channel |
| `test_region` mutation checks | a wrong 433 MHz band edge and a wrong RX1 rule are caught |
| `test_plan` all regions | every region's default plan passes on its card, fails on another (`ERR_REGION`), packs and unpacks |
| `test_plan` sub-bands | every sub-band of US915, AU915 and CN470 passes |
| `test_plan` TTN layouts | US915 FSB2, AU915 FSB2 and CN470 FSB11 equal the TTN plan files |
| `test_plan` other layouts and errors | ChirpStack's US915 layout passes; an off-grid channel, FSK on or std at 250 kHz give `ERR_CHANNEL`; std SF7 at 500 kHz gives `ERR_STD`; EU433 channels past 434.79 MHz give `ERR_CARD` |
| `test_lbs` router_config fixtures | ChirpStack US915 (16 DRs, DNONLY mixed) and TTS US915 (14 DRs, DNONLY 0, RFU `[0,0,0]`): DRs match US915, not EU868 or AU915; plans accepted on the 915 card, refused on the 868 card; the TTS plan equals the default |
| `test_lbs` DR mapping | uplink SF8 at 500 kHz is DR4 |
| cppcheck, core include check | clean |
| Firmware builds | rev3, rev1 and the rev3 desk build |
| Board (rev3 desk build, 0 dBm cap): upgrade from EU868 | card 868, EU868, the server's plan and the FiPy session kept; TX chain on |
| Upgrade from a stored 9xx region | host test only. Trying it on the board would need R2 set to AS923, which enables the TX chain there |
| Board: API refusals | `card=868,region=US915`, `card=915,region=EU868` and `card=900` refused (400) |
| Board: `card=915,region=US915` | restart; SX1257, radios 904.3/905.0 MHz, TX chain off, `lgw_start` OK, receiving on US915; local server: "not served"; TTN's EU868 router_config refused (DRs) |
| Board: `subband=1` | radio restart on 902.3-903.7 MHz, TX chain off; `subband=9` refused |
| Board: `card=915,region=AS923-1` | TX chain off, `lgw_start` OK; TTN's plan refused (default channels) |
| Board: back to `card=868,region=EU868` | the server's plan again, TX chain on |
| Board: FiPy over the air at 2 dBm | joined in 5 s (join-accept sent, capped at 0 dBm), 2 uplinks |
| Board: frames received at 9xx MHz | none: nothing may transmit at 902-928 MHz here, so there was no frame source |
