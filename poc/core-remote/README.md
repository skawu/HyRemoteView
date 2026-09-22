# `poc/core-remote` — `POC_ONLY` G3 RFB correctness harness

> **`POC_ONLY`. Not a product baseline.**

This directory exists to answer exactly one question for
[`skawu/HyRemoteView#9`](https://github.com/skawu/HyRemoteView/issues/9) (`G3-POC-1A`):

> Can a HyRemoteView-owned, bounded, GPL-free minimal RFB client connect correctly to the current
> HyRemote runtime on Windows and Linux, and complete deterministic frame and input verification?

## Boundary

In scope here:

* a HyRemoteView-owned minimal RFB client implementation (`src/`);
* deterministic, `POC_ONLY` test doubles and the runtime fixture used as the server side;
* negative/bounds tests for the client-side parser.

Explicitly **not** here, and not to be added under this directory:

* viewer UI, device list, Device Workspace, Identity, OEM branding;
* enhanced/encrypted channel work (`skawu/HyRemote#246`);
* a frozen `G4` source layout or a final protocol abstraction;
* TigerVNC / LibVNCClient / any GPL client dependency;
* `.github/workflows/**`.

## Why the client does not use Qt

The PoC question is about the *protocol* and the *wire behaviour*, and the client has to be
HyRemoteView-owned and free of GPL client dependencies. The client core therefore uses only the C++
standard library plus a small blocking-socket layer (`src/rfb/socket.*`), which keeps the same source
buildable on Windows and Linux. Qt appears only where the HyRemote runtime fixture itself needs it.

## Server side: two layers

| Layer | What it is | What it proves |
| --- | --- | --- |
| `tools/hyremote_runtime_fixture` | `POC_ONLY` driver that compiles HyRemote's **unmodified** `rfb_transport.cpp` / `vnc_auth.cpp` and drives it with a scripted deterministic frame sequence. | The real HyRemote runtime RFB transport is correctly driven by this client, including resize, damage and input semantics. |
| `tests/scripted_server.*` | `POC_ONLY` test double that implements the server side of the wire contract directly. | Negative/bounds, malformed input, truncation, overflow and mid-message disconnect, which a conformant server never produces. |

The test double exists because negative tests need a server that intentionally violates the
protocol. It is never used as product code and never replaces the runtime fixture.

## Deterministic content

Frame verification never uses "looks correct". The scripted frame source paints
known colour blocks plus a deterministic pixel pattern, then performs at least one geometry resize and
several local damage updates. The client asserts expected pixels and a deterministic framebuffer
hash, and the probe prints both so the evidence can be re-derived.

## Build

```text
cmake -S poc/core-remote -B build/core-remote -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/core-remote
ctest --test-dir build/core-remote --output-on-failure
```

The client core and the test double need no third-party dependency. The HyRemote runtime fixture is
configured only when a HyRemote checkout is provided:

```text
cmake -S poc/core-remote -B build/core-remote -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPOC_HYREMOTE_SOURCE_DIR=<path-to-HyRemote-checkout>
```

## Running the probe

```text
build/core-remote/rfb-probe --scenario determinism --port <port> --json
build/core-remote/rfb-probe --scenario input-semantics --port <port> --json
build/core-remote/rfb-probe --scenario reconnect --port <port> --json
```
