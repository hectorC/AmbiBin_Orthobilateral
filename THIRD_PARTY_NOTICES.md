# Third-party notices

## SPARTA AmbiBIN

Original plug-in source: copyright 2018 Leo McCormack, with subsequent authors
and contributors credited in the retained files and upstream SPARTA project.
The extracted processor/editor source retains its GPLv3-or-later notices.
The GPLv3 license text is provided in `LICENSE`.

The retained `SPARTALookAndFeel.h` is based on JUCE look-and-feel examples and
comes from SPARTA's shared resources; its existing comment is preserved.

## JUCE

Official repository: https://github.com/juce-framework/JUCE
Pinned version: 8.0.7, commit `10a589619b452c261b2940767eb253171eb5a823`.
The applicable JUCE framework and bundled dependency terms are in
`third_party/JUCE/LICENSE.md` and the individual module files. JUCE 8 provides
AGPLv3 and commercial licensing routes; select an appropriate route for any
distribution rather than assuming the original SPARTA license covers JUCE.

## Spatial Audio Framework

Official repository: https://github.com/leomccormack/Spatial_Audio_Framework
Pinned official commit: `a71005a2216da55ca4ff8587e3bea5840edba6fe`.
No project-specific SAF patches are applied. License notices are in
`third_party/SAF/LICENSE.md` and individual files, including the `ambi_bin`
example's ISC notices. SAF's optional HADES/tracker modules are not enabled.
The SOFA reader includes its upstream third-party notices.

## Locally supplied Windows dependencies

The VST2 SDK and Intel MKL headers, import library and custom runtime are local
development dependencies, ignored by this project's Git repository.
Preserve and review their supplied license notices before redistribution.
No SDK or Intel binary is downloaded by the build scripts, and no local system
installation is modified.
