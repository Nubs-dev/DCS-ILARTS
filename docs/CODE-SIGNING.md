# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

## What is signed

The release files built from this repository by the [Build workflow](../.github/workflows/build.yml) on GitHub
Actions:

- `ILARTS.exe` (the viewer), `ILARTS.dll` (the module DCS loads) and `ILARTSCam.dll` (the virtual cameras), in every
  package;
- the installer, `DCS-ILARTS-Setup-<version>.exe`.

Only files built by that workflow from a tagged commit of this repository are signed. Nothing built elsewhere is
submitted for signing.

## Team roles

- Committers and reviewers: [Nubs-dev](https://github.com/Nubs-dev)
- Approvers: [Nubs-dev](https://github.com/Nubs-dev)

Every signing request is approved by hand.

## Privacy

This program will not transfer any information to other networked systems unless specifically requested by the
user or the person installing or operating it. The only network connection DCS ILARTS makes is the RTMP stream
the user sets up, to the server address the user enters. Future versions may include an optional automatic
check for updates.
