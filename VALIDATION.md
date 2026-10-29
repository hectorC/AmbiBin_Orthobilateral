# AmbiBIN Orthobilateral — validation

## Main VST3 integration

The main project is `standalone-ambibin`. Its default Windows Release build
now produces the multi-bus **AmbiBIN Orthobilateral VST3** and the preserved
legacy VST2 when its local SDK is available. Format-specific shared-code
targets keep VST3's four Ambisonic buses separate from VST2's existing
interleaved-input layout. This integration does not change acoustic DSP.

The main-format build and both regressions passed: 90,356 VST2 assertions and
2,654,246 VST3 assertions. The actual VST2 DLL passed load/open/close checks,
retaining its name, `ABBI` identifier, 15 parameters, and advertised 64/64 I/O.

The native-wrapper harness loaded the main VST3 by its production name, checked
orders 1–4, disabled auxiliary inputs, and compared all 36 second-order input
lanes at yaw 0° and 90° against direct-processor references: 1,179,648 samples,
maximum error zero. Fresh fourth-order state restored immediately as order 4
with 100 inputs and two outputs, whether the host applied the layout before
or after state restoration. REAPER acceptance of the preceding Bus Test's
order changes and state-recall fix was separately confirmed by the user.
The promoted main binary still merits a final REAPER listening/session check.

A separate clean build directory configured with `AMBIBIN_BUILD_VST2=ON`
but a nonexistent `AMBIBIN_VST2_SDK` successfully built only VST3 and its
regression runner. The single VST3 regression passed. The original SDK was
not moved or modified. Disabling VST3 in that same missing-SDK scenario
produced the expected clear error that no plug-in format was available.

Current build paths and host-testing commands are in the README and
[VST3-ROUTING.md](VST3-ROUTING.md). The following sections retain the original
extraction and legacy-format validation record.

Validated on Windows x64, 28 October 2026, using CMake 4.3.1 and Visual Studio
2026 (MSVC 14.51), Release configuration.

## Dependency provenance

- JUCE: official `juce-framework/JUCE`, tag 8.0.7,
  `10a589619b452c261b2940767eb253171eb5a823`.
- SAF: official `leomccormack/Spatial_Audio_Framework`,
  `a71005a2216da55ca4ff8587e3bea5840edba6fe`.
- Both repositories were fetched directly from their official HTTPS origins,
  registered as submodules, and have clean working trees. Their Git metadata
  is inside this project's own `.git/modules`, not the parent repository.
- The parent project's locally modified SAF checkout remains untouched and is
  not used by this build.

## Source and build checks

The extraction baseline was parent commit
`cf7d19e6c202f847c2ff8c8bc5d0285b4ff1e2ed`. File hashes confirmed that the
processor source/header, editor implementation, look-and-feel, and original
regression source were copied unchanged at extraction. Subsequent test-harness
changes ensure PNG snapshots are overwritten and allow editor-only checks.
The editor header
differs only in its look-and-feel include path. A subsequent UI-only change
removed the SPARTA header branding, left-aligned the remaining title, and
updated the descriptive tooltip. No mixer or decoder processing was rewritten.

The current product name is **AmbiBIN Orthobilateral**, in the editor and host
metadata. The CMake target is `AmbiBINOrthobilateral`; the Release VST2 output
is `AmbiBINOrthobilateral_artefacts/Release/VST/AmbiBIN Orthobilateral.dll`
within the preset build directory. The loader check verifies the new name
alongside the retained `ABBI` identifier and parameter count. Older output
directories may remain from earlier builds; use the current path in README.

The independent Release build completed successfully. Generated Visual Studio
projects were checked for references to the parent's `audio_plugins`, `SDKs`,
and original build directories; none were found. JUCE and SAF compile from
this directory's submodules. The VST2 SDK and MKL development inputs are local
copies inside this folder, intentionally ignored by this project's Git rules.

## Automated results

`ctest --preset windows-release`: **passed**, 90,356 assertions, approximately
49 seconds. Coverage includes:

- Defaults, automation, state round-trip, and legacy state compatibility.
- Diagnostic routing at orders 1–3 and handling of missing input channels.
- Bilateral routing, orthobilateral regression, focus, extra host channels,
  and continuity around sideways orientations.
- Mode changes without frozen tails and per-sample gain smoothing.
- Microphone selector and availability of front/back controls.

The two- and four-microphone editor snapshots were also visually inspected.

`scripts/Test-Vst2Load.ps1`: **passed**. The actual Release DLL loaded,
instantiated, opened and closed. VST identity `ABBI` and 15 parameters were
verified. Its loaded MKL DLL resolved to the copy beside the plug-in, rather
than the existing system-wide copy. This minimal loader does not negotiate
a DAW bus layout or render audio; processor audio is covered by the regression
suite above.

## Remaining acceptance checks

Actual DAW scanning, saved-session recall, head tracking, and listening tests
remain manual checks. In particular, official SAF is intentionally used here;
these results do not establish acoustic equivalence to the parent's modified
SAF. The regression comparisons use this extraction's official decoder as
their reference. Optional VST3 and non-Windows builds have not been validated.

The parent repository was clean after validation, with this entire folder
locally excluded. No plug-in/system directory was written. These build and
test checks do not require installing the plug-in or modifying the parent
SPARTA repository.
