# AmbiBIN Orthobilateral

AmbiBIN Orthobilateral is an audio plug-in for rendering recordings from
multiple Ambisonic microphones as a head-tracked binaural listening experience.
It combines binaural decoding of each microphone's sound field with
orientation-dependent mixing of the decoded signals.

The project supports two microphone configurations:

| Mode | Microphone positions | Input feed order |
| --- | --- | --- |
| Bilateral | Two microphones placed side by side | Left, Right |
| Orthobilateral | Four microphones arranged horizontally around the listening reference | Left, Right, Front, Back |

Each feed is a complete Ambisonic recording, not a single audio channel. The
input consists of consecutive channel groups, one group per microphone, using
the same Ambisonic order, channel ordering, and normalization. Recordings must
be sample-aligned across microphones.

## Processing and controls

Each microphone feed is rotated and decoded to a binaural pair. A
multi-microphone mixer then calculates separate contributions to the left and
right outputs from the listener's yaw, pitch, and roll. Microphone positions
remain fixed relative to the rig as the virtual head rotates. The left output
uses the decoded left-ear signals; the right output uses the decoded right-ear
signals.

The mixer supports both the four-position orthobilateral geometry and the
two-position bilateral geometry. Its controls include microphone-count
selection, per-feed gain trims, Blend Focus for adjusting transition breadth,
and diagnostic outputs for inspecting individual feeds. Gain smoothing avoids
abrupt changes as the orientation or microphone configuration changes.

The microphones occupy a horizontal plane. Tilting the ears out of that plane
therefore requires a mixing fallback rather than access to recordings from
above or below the listener. The system does not reconstruct recordings at
missing microphone positions. Combining spaced recordings can also introduce
coloration; normalized mixing weights do not remove differences in arrival
time or recorded sound fields.

Rotation can be set manually or received from a head tracker over OSC. Use the
individual control tooltips for configuration details.

## Foundations and attribution

This project builds on the open-source **SPARTA AmbiBIN** plug-in from
[SPARTA](https://github.com/leomccormack/SPARTA), by **Leo McCormack and
contributors**. Its Ambisonic rotation and binaural decoding
functionality provide the foundation; this project's focus is the
multi-microphone input handling and orientation-dependent binaural mixing for
bilateral and orthobilateral rigs. These additions do not imply authorship of
the underlying SPARTA or Spatial Audio Framework algorithms.

Original copyright notices are retained. The inherited plug-in source is
GPLv3-or-later; see `LICENSE` and `THIRD_PARTY_NOTICES.md`.

## Dependencies

The build contains only this plug-in and its required libraries; it does not
configure or build the SPARTA suite. It is a plug-in project, not a standalone
audio application.

- **JUCE** is a submodule fetched from the official
  [JUCE repository](https://github.com/juce-framework/JUCE), pinned to **8.0.7**.
- **Spatial Audio Framework (SAF)** is a submodule fetched from its
  [official repository](https://github.com/leomccormack/Spatial_Audio_Framework),
  pinned to commit `a71005a2216da55ca4ff8587e3bea5840edba6fe`.
  This is unmodified upstream SAF; no project-specific patches are applied.
  Only its `saf` and `saf_example_ambi_bin` libraries are configured.
- Windows builds require the **Intel MKL** headers, import library, and runtime
  described below. VST2 additionally requires a locally supplied **VST2 SDK**;
  without it, the build automatically skips VST2 and still builds VST3.

Exact dependency pins are recorded in `DEPENDENCIES.json` and the submodule
gitlinks. Clone this repository with its official dependencies:

```powershell
git clone --recurse-submodules https://github.com/hectorC/AmbiBin_Orthobilateral.git
cd AmbiBin_Orthobilateral
```

For an existing checkout, initialize the submodules with:

```powershell
git submodule update --init --recursive
```

### Windows prerequisites

Install CMake and the Visual Studio C++ desktop toolchain. The tested preset
uses Visual Studio 2026 x64; a Visual Studio 2022 preset is also supplied.
CMake must be recent enough to recognize the selected Visual Studio generator.

Supply these local dependencies, which are excluded from version control:

```
third_party/VST2_SDK/             optional, locally supplied VST2 SDK
third_party/mkl/include/          Intel MKL headers
third_party/mkl/lib/              saf_mkl_custom_lp64.lib import library
third_party/mkl/bin/              saf_mkl_custom_lp64.dll runtime
```

These inputs are already provisioned in the current development folder.
CMake options `AMBIBIN_VST2_SDK`, `AMBIBIN_MKL_ROOT`, `INTEL_MKL_HEADER_PATH`,
`INTEL_MKL_LIB`, and `AMBIBIN_MKL_RUNTIME` allow overriding their locations.
No surrounding SPARTA checkout or prebuilt SPARTA library is required.

The word "custom" in the MKL runtime name refers to a selected set of Intel
MKL numerical routines, not to changes in SAF's acoustic processing.

## Build

From the project directory:

```powershell
cmake --preset windows-vs2026
cmake --build --preset windows-release --parallel 4
```

Alternatively, the helper also locates CMake in its normal Windows installation
directory when it is not on PATH:

```powershell
./scripts/Build-Windows.ps1
# Alternate compiler:
./scripts/Build-Windows.ps1 -Preset windows-vs2022
```

The default build produces the main multi-bus VST3 and, when the VST2 SDK is
available, the existing VST2. Automatic installation is disabled:

```
build/windows-vs2026/AmbiBINOrthobilateral_artefacts/Release/VST/AmbiBIN Orthobilateral.dll
build/windows-vs2026/AmbiBINOrthobilateralVST3_artefacts/Release/VST3/AmbiBIN Orthobilateral.vst3
```

The matching MKL runtime is copied beside each plug-in binary.
The DAW's DLL loader must be able to locate that runtime. This build does not
install files into Windows or plug-in directories.

The repository build produces plug-ins only. Local regression sources, test
scripts, and validation reports are excluded from Git and are not required
to configure or build either format.

Both formats are enabled by default. Use `-DAMBIBIN_BUILD_VST2=OFF` or
`-DAMBIBIN_BUILD_VST3=OFF` at configuration time to explicitly disable one.
`AMBIBIN_VST2_SDK` selects the local SDK directory; both
`pluginterfaces/vst2.x/aeffect.h` and `aeffectx.h` must exist there for VST2
to be built. A missing SDK leaves the requested option enabled but omits the
VST2 targets, so supplying the SDK on a later configuration enables them again.
Configuration fails clearly if no requested format is available.

VST3 uses four named Ambisonic input buses (Left, Right, Front, Back) and a
stereo output. It supports first- through fourth-order decoding; second order
is the default. VST2 retains its original interleaved input layout and order
controls, subject to the host's available channel count. The acoustic mixer
and decoder processing are shared and unchanged. See
[VST3-ROUTING.md](VST3-ROUTING.md) for REAPER routing, order changes, and session
recall.

The macOS/Linux build paths have not been validated here; non-Windows builds
use SAF's corresponding numerical backends.

## Compatibility and distribution

The displayed and host-reported name is **AmbiBIN Orthobilateral**. The legacy
manufacturer code `LEOM`, VST2 plug-in code `ABBI`, parameter order, and state
serialization are retained. Do not install this and the earlier
`sparta_ambiBIN_orthoLat` version simultaneously under the same identifier.
Actual session recall can depend on the host and its handling of renamed
plug-in files, so verify existing sessions before replacing an installation.

JUCE, SAF, the VST2 SDK, and Intel dependencies have their own licenses. Local
SDK/math copies are development inputs, not an assertion of redistribution
rights. Review the retained notices and applicable terms before publication
or binary distribution.

Build outputs, the VST2 SDK, and MKL development/runtime files are intentionally
excluded from this source repository. Provision the local dependencies above
before building a fresh checkout.

VST3 does not automatically replace VST2 instances. The earlier **Bus Test**
VST3 also has a different identifier from the main VST3: existing test sessions
still require that test plug-in until you manually replace their instances.
Keep the tested binary available while migrating those projects.
