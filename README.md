# HyRemoteView

Viewer-side repository of the HyRemote remote-access product line.

> ## STATUS: `G3` verification PoC — **not** a product architecture baseline
>
> The first commit in this repository exists only to start the `G3-POC-1A` validation harness
> ([`skawu/HyRemoteView#9`](https://github.com/skawu/HyRemoteView/issues/9)).
>
> * It does **not** freeze the future `G4` repository layout.
> * Nothing here is a formal `V1` implementation commitment.
> * A PoC `PASS` must not be read as "these sources are the product".
>
> Server/runtime/target side lives in [`skawu/HyRemote`](https://github.com/skawu/HyRemote).

## What this repository currently contains

| Path | Meaning |
| --- | --- |
| `poc/core-remote/` | `POC_ONLY` harness: a HyRemoteView-owned, bounded, GPL-free minimal RFB client plus its deterministic test doubles. |

Everything under `poc/` is explicitly marked `POC_ONLY`. `POC_ONLY` means: built to answer one
verification question, allowed to be deleted, never treated as a shipped architecture.

## The one question `G3-POC-1A` answers

> Can a HyRemoteView-owned, bounded, GPL-free minimal RFB client connect correctly to the current
> HyRemote runtime on Windows/Linux and complete deterministic frame and input verification?

Explicitly **out of scope** for this PoC:

* a complete viewer UI, home page, or device list;
* Device Workspace, Identity, login (Feishu/DingTalk/…), OEM branding;
* Enhanced Channel / enhanced/encrypted transport capability;
* a frozen `G4` repository layout, protocol abstraction, or `src/app/core/device/identity/...` tree;
* TigerVNC / LibVNCClient / any GPL client dependency;
* GitHub Actions workflows (see the CI red line below).

## Confidentiality / licensing boundary

The PoC client must stay free of GPL client dependencies. Third-party code is added only with a
recorded license and only when it cannot be avoided; the default is to implement the needed
protocol primitive in-repo.

## CI red line

Per `skawu/HyRemoteView#10`, this repository must **not** create `.github/workflows/**`, must not add
Actions badges, and must not copy HyRemote's CI. Cross-platform verification is done by running the
documented commands locally and posting the raw evidence.

## Build and test

The harness is plain C++17 with no third-party dependency, so it builds with the standard system
toolchain. See [`poc/core-remote/README.md`](poc/core-remote/README.md) for the exact commands,
including the HyRemote-backed deterministic runtime fixture.

## Authority

Conclusions belong in the GitHub issue. Chat replies and agent console summaries are not project
facts. `G3-POC-1A` evidence is posted to
[`skawu/HyRemoteView#9`](https://github.com/skawu/HyRemoteView/issues/9).
