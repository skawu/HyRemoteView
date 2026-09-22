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
| `hyremote_fixture/` | `POC_ONLY` driver that compiles HyRemote's **unmodified** `rfb_transport.cpp` / `vnc_auth.cpp` and drives it with the shared deterministic content through a control channel. | The real HyRemote runtime RFB transport is correctly driven by this client, including resize, damage and input semantics, and its VNC authentication agrees with the client's DES. |
| `src/harness/scripted_server.*` | `POC_ONLY` test double that implements the server side of the wire contract directly. | Negative/bounds, malformed input, truncation, overflow and mid-message disconnect, which a conformant server never produces. |

The test double exists because negative tests need a server that intentionally violates the
protocol. It is never used as product code and never replaces the runtime fixture.

## Deterministic content

Frame verification never uses "looks correct". The scripted frame source paints
known colour blocks plus a deterministic pixel pattern, then performs at least one geometry resize and
several local damage updates. The client asserts expected pixels and a deterministic framebuffer
hash, and the probe prints both so the evidence can be re-derived.

## Build

The client core and the test double need no third-party dependency, so this build needs nothing but a
C++17 compiler:

```text
cmake -S poc/core-remote -B build/core-remote -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/core-remote
ctest --test-dir build/core-remote --output-on-failure
```

The HyRemote runtime fixture is built by a second configuration that points at an unmodified HyRemote
checkout. Qt is only needed here, because the HyRemote runtime itself needs it:

```text
cmake -S poc/core-remote -B build/hyremote-fixture -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<Qt6 prefix> \
      -DOPENSSL_ROOT_DIR=<OpenSSL root, when POC_HYREMOTE_WITH_SECURITY=ON> \
      -DPOC_CORE_REMOTE_BUILD_TESTS=OFF \
      -DPOC_HYREMOTE_SOURCE_DIR=<unmodified HyRemote checkout> \
      -DPOC_HYREMOTE_WITH_SECURITY=ON
cmake --build build/hyremote-fixture
```

`POC_HYREMOTE_WITH_SECURITY=ON` mirrors HyRemote's own `HYREMOTE_WITH_TRANSPORT_SECURITY` and is what
makes VNC authentication (security type 2) reachable.

## Findings recorded by this harness

* **HyRemote accepts a `RFB 003.007` client version but always frames the security offer in the 3.8
  form.** `src/runtime/src/transport/rfb_transport.cpp` accepts `version.mid(8, 3) == "007"` and then
  writes the 3.8 `{count, type...}` offer unconditionally. An RFC 6143-conformant 3.7 peer reads a
  4-byte security type instead, so it desynchronises and is closed by the transport's handshake bound
  (`ProtocolFailure` at 3000 ms). Reproduced by `--scenario handshake --version 37` against the
  unmodified runtime; the same run with `--version lenient37` completes. Impact is narrow, because the
  runtime announces `RFB 003.008` and a peer that follows that announcement negotiates 3.8 anyway.
  Reported upstream rather than patched here: this repository must not carry a runtime change.

## Running the probe

Start one fixture, read the ports it prints, run one scenario, and let the scenario stop the fixture
again. A fresh fixture per scenario keeps every run reproducible: the fixture always starts holding
deterministic step 0 and an empty input log.

```text
build/hyremote-fixture/hyremote-runtime-fixture --security none
#   READY transport_port=<T> control_port=<C>

build/hyremote-fixture/rfb-probe --port <T> --control-port <C> --scenario handshake
build/hyremote-fixture/rfb-probe --port <T> --control-port <C> --scenario determinism
build/hyremote-fixture/rfb-probe --port <T> --control-port <C> --scenario input-semantics
build/hyremote-fixture/rfb-probe --port <T> --control-port <C> --scenario reconnect
build/hyremote-fixture/rfb-probe --port <T> --control-port <C> --scenario clipboard
build/hyremote-fixture/rfb-probe --port <T> --control-port <C> --scenario determinism --set-pixel-format 565be
```

VNC authentication needs a credential, and a credential never comes from a command line:

```text
# set POC_S3CRET in the environment of both processes
build/hyremote-fixture/hyremote-runtime-fixture --security vnc --password-env POC_S3CRET
build/hyremote-fixture/rfb-probe --port <T> --control-port <C> --scenario handshake \
                                 --security vnc --password-env POC_S3CRET

# the client's DES against HyRemote's own OpenSSL-based implementation, same challenge, same password
build/hyremote-fixture/rfb-probe --port <T> --control-port <C> --scenario des-crosscheck \
                                 --password-env POC_S3CRET
```

A scenario prints its findings and ends with `SCENARIO_RESULT=PASS` or `SCENARIO_RESULT=FAIL`, and the
process exit code matches. A scenario that is *expected* to be refused (a wrong credential, a strict
3.7 negotiation) correctly reports `SCENARIO_RESULT=FAIL` with the deterministic error code in it.
