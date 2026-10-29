# AmbiBIN Orthobilateral VST3 routing

The main VST3 incorporates the multi-bus implementation validated in the
earlier REAPER Bus Test. It is no longer a separate test product. VST2 retains
its existing input layout; no mixer or decoder algorithm was changed by this
format integration.

## Input buses

The four Ambisonic input buses are Left, Right, Front, and Back, in that order.
Each contains one microphone's ACN-ordered Ambisonic channels, using the
normalization selected in the plug-in. The output bus is stereo binaural.

| Decoding order | Channels per microphone | Four active microphones | Two active microphones |
|---|---:|---:|---:|
| First | 4 | 16 | 8 |
| Second (default) | 9 | 36 | 18 |
| Third | 16 | 64 | 32 |
| Fourth | 25 | 100 | 50 |

These totals assume the corresponding buses are enabled in the host. Selecting
bilateral mode uses only Left and Right; it does not itself disable the other
host buses. Front and Back can be disabled in the host when unused. A required
input bus that is disabled causes silence rather than reassignment of another
microphone's channels.

## REAPER and order changes

Copy the complete `AmbiBIN Orthobilateral.vst3` directory to a VST3 search
location, preserving its `Contents` directory and the included MKL runtime.
Rescan if necessary and select the main **VST3: AmbiBIN Orthobilateral** entry,
not the earlier Bus Test or the VST2 entry.

Stop playback before changing decoding order. The VST3 requests the matching
input-bus widths from REAPER. Check the track's channel count and plug-in pin
routing after changing order. REAPER testing confirmed that the track resizes
and the decoding order survives saving and reopening the project.

Changing order does **not** rearrange the channels of a recording. For example,
a four-microphone second-order file has groups at track channels 1–9, 10–18,
19–27, and 28–36. To use its first-order components, route channels 1–4,
10–13, 19–22, and 28–31 to the four new four-channel buses. Simply routing
the first 16 file channels would mix channels belonging to different microphones.

VST3 hosts differ in handling plug-in-initiated I/O changes. The processor
notifies the host and handles rejected layouts coherently; automatic resizing
in other hosts has not been established. Buffers with a stale channel count
are silenced rather than interpreted as a different microphone layout.

## Identity and existing sessions

The main product uses the retained `ABBI` product code. The former Bus Test
used `ABBT`, so a project containing Bus Test instances does not automatically
switch to this main VST3. Replace those instances manually and transfer their
settings, retaining the old binary until the migrated project is verified.
VST3 automatic replacement of VST2 is disabled. Both formats can coexist;
do not install two differently named copies of the same legacy VST2 identifier.

## Native-wrapper regression check

The normal build and CTest run produce both format-specific regressions when
the VST2 SDK is present, or VST3 only when it is absent. For additional tests
through the actual JUCE VST3 wrapper:

```powershell
cmake -S tests/vst3-host -B build/vst3-host-tests -G "Visual Studio 18 2026" -A x64
cmake --build build/vst3-host-tests --config Release --parallel 4
# Run only after the normal CTest run has finished writing the reference file:
& './build/vst3-host-tests/Release/vst3_routing_check.exe' `
  './build/windows-vs2026/AmbiBINOrthobilateralVST3_artefacts/Release/VST3/AmbiBIN Orthobilateral.vst3' `
  './build/windows-vs2026/test-artifacts-vst3/vst3-routing-reference.bin'
```

The harness checks all 36 second-order input lanes at yaw 0° and 90° against
direct-processor references, orders 1–4, disabled auxiliary buses, and fresh
instance state restoration with host layout applied before or after state.
It supplements, rather than replaces, listening tests and saved-project checks
in REAPER. No system plug-in directory is modified by the build or tests.
