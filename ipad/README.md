# AVA OS for iPhone and iPad

One app, two tabs.

- **Bed**: the remote for the bed controller (the Raspberry Pi running AVA Air).
  Status, master level and mute, the Pi's vibration engine (strength, tone,
  breath), each zone (meter, level, tuned frequency, mute, pulse) and a test
  tone per sound card output. Works over Wi-Fi; the phone must be on the same
  network as the Pi. The gear icon sets the Pi's address (default
  `danepi1.local`).
- **Play**: the AVA OS engine running on the device, with the octagon pads.
  The microphone follows the music in the room. On an iPhone it starts only
  when you press START, so opening the app never takes the audio away from
  music that is already playing to the bed.

## Play tab: where the vibration goes

The WIRING control says how the output channels reach the bed.

| Wiring | Needs | Channels |
| --- | --- | --- |
| ZONES | USB interface, 7+ outputs | one channel per body zone, as on the Mac. On a stereo output every zone is summed on both channels. |
| 8 CH | USB interface, 10+ outputs | circle bed, one transducer per channel on Ch 3-10 |
| 4 CH | USB interface, 4+ outputs | Ch 1 outer top + left, Ch 2 inner top + left, Ch 3 inner right + bottom, Ch 4 outer right + bottom |
| 2 CH | any stereo output | left = inner disc, right = outer ring |

A wiring that needs more channels than the output has falls back to the next
smaller one, and the caption under the control says so.

Bluetooth is stereo. Playing from the Play tab to the Pi over Bluetooth
("AVA Bed") reaches the Pi as ordinary phone music, so the Pi's own engine
turns it into vibration a second time. To drive the bed from the phone's
engine, use a USB audio interface on the phone, or use the Bed tab and let the
Pi do the work.

## Build

Needs macOS with Xcode, or the GitHub Actions workflow in
`.github/workflows/ios.yml` (see the comments at the top of that file).

```
brew install xcodegen
cd ipad
xcodegen generate
open AVAOS.xcodeproj
```

## TestFlight from GitHub Actions

1. In App Store Connect, create the app record (Apps > + > New App, iOS) with
   the bundle ID you will use.
2. Create an App Store Connect API key with the Admin role (Users and Access >
   Integrations > App Store Connect API). Note the key ID and issuer ID and
   download the `.p8` file (it can be downloaded once).
3. In the GitHub repository, add the secrets `ASC_KEY_ID`, `ASC_ISSUER_ID`,
   `ASC_KEY_P8` (the whole text of the `.p8` file) and `APPLE_TEAM_ID`. If the
   bundle ID is not `com.avartifacts.ava`, add the repository variable
   `BUNDLE_ID`.
4. Actions > iOS > Run workflow, tick "Upload to TestFlight".
5. After Apple finishes processing (10 to 30 minutes), add yourself under
   TestFlight > Internal Testing and install with the TestFlight app.

The build number is the workflow run number, so every upload is newer than
the last one.

## Files

- `project.yml`: XcodeGen project definition.
- `App/AVAOSApp.swift`: app entry point and the two tabs.
- `App/BedView.swift`, `App/PiClient.swift`, `App/PiModels.swift`: the Bed tab.
- `App/PlayView.swift`, `App/OctagonPadView.swift`: the Play tab.
- `App/AudioBridge.h/.mm`: audio session, output wiring, and the bridge to the
  shared C++ engine in `../src`.
