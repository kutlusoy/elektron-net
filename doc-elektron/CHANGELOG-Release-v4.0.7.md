# Changelog: Release v4.0.7

**Status:** In preparation (branch `4.0.7`).
**Related:** `CHANGELOG-Release-v4.0.6.md`, `BITCOIN_CORE_DIFF.md`.

This file is a release-level summary of everything shipped in v4.0.7, in
the spirit of `CHANGELOG-Release-v4.0.6.md`.

---

## Node: stall through duplicate orphan announcements (CVE-2026-117348)

Elektron Net is based on a Bitcoin Core version in the affected range (29.0 and later). Since 29.0, an `inv` that announces the wtxid of a transaction in the orphanage makes the node rebuild the list of the orphan's parents and check each of them against the mempool and the recent filters. A single `inv` message can carry up to 50,000 entries and duplicates were not skipped.

- **Impact**: a peer could send an orphan with thousands of missing parents and then an `inv` that repeats its wtxid 50,000 times. Each such message stalled the node for over a dozen seconds, during which it processed no messages from other peers, including blocks. Severity: Medium.
- **Fix**: the `INV` handler in `src/net_processing.cpp` now processes each transaction hash announced in one message only once, as upstream Bitcoin Core does in 30.5, 31.3 and 32.1 (disclosure: https://bitcoincore.org/en/2026/11/20/disclose-orphan-inv-stall/). No change to relay behavior for honest peers and no consensus change.

## Version

`CLIENT_VERSION_BUILD` bumped from 6 to 7 in `CMakeLists.txt`
(`v4.0.6` -> `v4.0.7`); `CLIENT_VERSION_MAJOR`/`MINOR` unchanged.
