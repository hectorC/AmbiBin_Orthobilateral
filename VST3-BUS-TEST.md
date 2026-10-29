# Historical prototype notes

The tested multi-bus implementation is now integrated into the main
**AmbiBIN Orthobilateral VST3**. The names and build commands below describe
the earlier Bus Test. Use [VST3-ROUTING.md](VST3-ROUTING.md) and the README
for the current main build. Keep the old binary for existing Bus Test sessions
until their instances have been manually replaced.

## REAPER VST3 four-bus prototype

This separate test variant is named **AmbiBIN Orthobilateral Bus Test**. It
uses plug-in code `ABBT`, distinct from the existing `ABBI` product. It does
not replace an installed VST2 or the earlier single-input-bus VST3.

The purpose is to test VST3 bus negotiation and routing in REAPER without
changing the existing binaural decoders or orientation-dependent mixer.
**Bus Test 3** enables orders **1–4** and fixes decoding-order recall on project
load. Selecting an order requests matching
Ambisonic widths for every microphone bus and updates the SAF decoders. The
default remains second order. This version replaces the first Bus Test under
the same test plug-in ID; it does not replace the regular VST2 product.

## Build

```powershell
cmake --preset windows-vs2026 -B build/windows-vst3-bus-test -DAMBIBIN_BUILD_VST2=OFF -DAMBIBIN_BUILD_VST3=ON -DAMBIBIN_VST3_MULTIBUS_TEST=ON
cmake --build build/windows-vst3-bus-test --config Release --parallel 4
ctest --test-dir build/windows-vst3-bus-test -C Release --output-on-failure
```

The default build options still select the existing product. The new prototype
is opt-in and refuses a configuration that also builds VST2. Installation is
disabled. The existing VST2 DLL has not been overwritten.

## REAPER setup

1. Extract the test package and copy the **entire**
   `AmbiBIN Orthobilateral Bus Test.vst3` directory to a VST3 directory scanned
   by REAPER, normally `C:/Program Files/Common Files/VST3/` on Windows.
   Do not copy just its inner binary. The matching MKL runtime is included
   beside that binary under `Contents/x86_64-win/`.
2. Rescan as necessary and insert **VST3: AmbiBIN Orthobilateral Bus Test**,
   not either of the regular products. The editor's version line says `Bus Test 3`.
   Close REAPER before replacing the previously installed test bundle.
3. Use a track with at least **36 channels** for four second-order feeds. Keep
   the existing sample-aligned interleaved recording on that track if desired.
4. Open the FX pin connector. Verify that all four nine-channel microphone
   inputs are active and map them as follows:

| Track channels (one-based) | Plug-in input bus | Channels within that bus |
| --- | --- | --- |
| 1–9 | Left microphone | ACN 0–8 |
| 10–18 | Right microphone | ACN 0–8 |
| 19–27 | Front microphone | ACN 0–8 |
| 28–36 | Back microphone | ACN 0–8 |

5. Map the **Binaural** output to track channels 1–2. Monitor/send only that
   stereo pair; unused input channels on a multichannel track must not be
   mixed into the headphone output downstream.
6. Select **4 mics** and match the normalization setting to the recording.
   The bus labels use ACN order. The decoding-order selector now enables orders
   1–4 and requests matching microphone bus widths from REAPER.
7. For bilateral testing, select **2 mics** and supply the first 18 channels.
   Front and Back buses can remain active with silent inputs, or be disabled
   by the host. Left and Right must match the currently selected order.

Do not request one fifth-order Ambisonic bus to carry all four microphones.
The prototype explicitly negotiates **four second-order buses** instead.

If REAPER exposes fewer than four nine-channel groups or rejects the layout,
record its version and capture the FX pin connector/input-bus configuration.
That is a host-negotiation result to investigate, not a reason to judge the
acoustic output yet. Additional buses may require host activation/routing.

## Order-change test

### Bus Test 3: saved-order recall

Bus Test 2 deferred order restoration until after the state-loading call
returned. A newly created VST3 controller could therefore read and cache the
constructor's second-order value before the saved order had been applied.
That was reproduced with a fourth-order state loaded into a fresh VST3.

Bus Test 3 restores bus widths and decoder order synchronously before returning
state. It cancels stale pending order requests, saves the negotiated bus order,
and reconciles decoder order with the final bus layout before codec setup.
Any required host I/O notification is deferred until after the loading call.

Regression tests now destroy/recreate the actual VST3 instance, restore a
fourth-order state with the host layout supplied both before and after state,
verify immediate order 4 and 100-in/2-out after preparation, and render a probe
through the Right microphone's fourth-order channel offset. These checks
failed on Bus Test 2 and pass on Bus Test 3. REAPER session recall still needs
confirmation on your side.

After installing this build, select the desired order and save the project
again before repeating the close/reopen test. A project re-saved while the old
build was displaying the wrong order may already contain that unwanted value.

### Changing order

Stop playback, ensure the track has enough channels, then change the order in
the plug-in. Check the host's input count and pin routing before resuming:

| Decoding order | Channels per microphone | Four active buses | Two active buses |
| --- | --- | --- | --- |
| 1 | 4 | 16 | 8 |
| 2 | 9 | 36 | 18 |
| 3 | 16 | 64 | 32 |
| 4 | 25 | 100 | 50 |

Selecting bilateral mode does not itself disable Front/Back host buses, so
the host can still report the four-bus total even though only two feeds are
used. Orders above four are disabled in this test build to keep four active
buses within REAPER's 128-channel track limit.

The plug-in sends VST3 `kIoChanged` through JUCE's public client-extension hook.
Host automation is deferred to the message thread; buffers of the old width
are not processed with new microphone offsets. Host-driven layouts must use
the same supported order on all active microphone buses. Disabled auxiliary
buses retain the new width for later reactivation. Saved state includes order.

**Host cooperation is still under test.** The local JUCE host accepted explicit
host-driven changes to all four orders, but reasserted second order after a
plug-in-initiated first-order request. The decoder stayed synchronized with
the host's choice. REAPER's response to the dropdown-triggered restart remains
the main acceptance check. If the selection snaps back or the I/O count does
not change, report that behavior and capture the pin connector. Do not assume
the requested order was accepted. A fresh instance/layout request may be needed.

**Changing order does not rearrange the channels in a recorded file.** A
36-channel second-order recording still has nine-channel microphone groups
when you select first order. To test first-order decoding from that file,
retain 36 track channels and map just the first four ACN channels of each
original microphone group:

| Original track channels | New first-order input bus |
| --- | --- |
| 1–4 | Left, channels 1–4 |
| 10–13 | Right, channels 1–4 |
| 19–22 | Front, channels 1–4 |
| 28–31 | Back, channels 1–4 |

Do not route the first 16 file channels consecutively as four first-order
microphones: that would mix microphone groups. Higher orders require matching
source channels, or explicit zero padding of unavailable higher-order SH
components per microphone; padding does not add recorded spatial information.

## Suggested listening checks

- Confirm each feed using the individual diagnostic outputs before testing
  head tracking. Check that Front and Back are not interchanged.
- In four-mic mode at zero yaw/pitch/roll, the normal output selects Left for
  the left ear and Right for the right ear. At yaw +90 with the existing
  plug-in angle convention, it selects Front for the left ear and Back for
  the right ear. A tracker may use the opposite sign; start with manual angles.
- Check smooth transitions, then test bilateral mode.
- Save and reopen the session to verify retained routing and controls.

## Local automated validation

The processor test checks all microphone/SH input lanes at orders 1–4 against
the corresponding independent feed decode, input/output buffer alias safety,
raw diagnostics, bus-layout rejection, disabled auxiliary buses, and state
restoration. Missing required buses produce silence, not substituted feeds.
It also checks VST3 restart notification, rollback on an explicitly rejected
restart, deferred audio-thread requests, disabled-bus widths, and saved-order
restoration. Current processor result: **2,654,246 assertions passed**.

A separate JUCE-based host also loaded the **actual VST3 bundle** and accepted
four nine-channel Ambisonic inputs with stereo output. It compared all 36 input
lanes at yaw 0 and +90 against processor reference renders: **1,179,648 sample
comparisons, maximum difference 0**. Bilateral negotiation with Front/Back
disabled also succeeded, including a bilateral W-channel audio check. The
wrapper diagnostic source is retained in `tests/Vst3HostTests.cpp`, with a
separate host build project under `tests/vst3-host/`.

To repeat the actual-wrapper checks after the processor test has generated
its reference renders:

```powershell
cmake -S tests/vst3-host -B build/vst3-host-tests -G "Visual Studio 18 2026" -A x64
cmake --build build/vst3-host-tests --config Release --parallel 4
./build/vst3-host-tests/Release/vst3_routing_check.exe "$PWD/build/windows-vst3-bus-test/AmbiBINOrthobilateralBusTest_artefacts/Release/VST3/AmbiBIN Orthobilateral Bus Test.vst3" "$PWD/build/windows-vst3-bus-test/test-artifacts/vst3-routing-reference.bin"
```

These checks do not establish REAPER compatibility, tracking behavior in a
live session, or REAPER's response to automatic I/O restarts. Those remain acceptance
tests. No plug-in was installed automatically, no release was published, and
the original SPARTA checkout and official dependency submodules are unchanged.
