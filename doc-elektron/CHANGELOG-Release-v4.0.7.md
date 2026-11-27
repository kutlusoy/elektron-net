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

## Node: use-after-free in script checks (CVE-2024-52911)

`ConnectBlock()` in `src/validation.cpp` declared the `CCheckQueueControl` before the vector of `PrecomputedTransactionData`. The vector was therefore destroyed first, and on an early return from an invalid block a background script check thread could still read the freed data. Exploiting it needs a block with valid proof of work at the tip. Severity: High.

- **Fix**: `txsdata` is declared before `control`, so the control (which waits for the checks) is destroyed first, as in Bitcoin Core 29.0. No consensus change.

## Node: backported upstream hardening

Small fixes from Bitcoin Core master (merged between May and November 2026) that do not touch consensus rules, the UTXO attestation, the MuHash state or the pruning depth. None of them changes the validity of a block, the attestation hash or the retained block window.

- **Corrupt coin database no longer invalidates blocks** (bitcoin/bitcoin#34931): `CCoinsViewDB::GetCoin()` used to treat a coin record that cannot be deserialized as a missing coin. A node with a damaged chainstate then marked valid blocks as `bad-txns-inputs-missingorspent` and forked itself off permanently. A deserialization or database error is now thrown as `dbwrapper_error`, which makes `CCoinsViewErrorCatcher` shut the node down cleanly. `CDBWrapper::TryRead()` is the new building block, `Read()` keeps its behavior.
- **Duplicate entries in `m_blocks_unlinked`** (#35070, #35168): the same block could be added twice and then re-added to `setBlockIndexCandidates` with a changed `nSequenceId`, which breaks the ordering of that set (undefined behavior). `BlockManager::AddUnlinkedBlock()` rejects duplicates, and pruned blocks without data are no longer added at startup. This matters for Elektron Net because every node is pruned.
- **`FetchBlock()` takes `cs_main` before looking up the peer** (#35498), which closes an assertion failure when a peer disconnects during the `getblockfrompeer` RPC.
- **Compact blocks**: an invalid compact block in the optimistic reconstruction path is now punished as misbehavior (#35321), and a `sendcmpct` with an announce field above 1 is rejected (#35550).
- **Integer overflows**: saturating `nScore` for local addresses (#34028), `int` cast in `ProcessGetCFCheckPt` (#36321), `int64_t` accumulator for the package weight (#36535).
- **Mempool memory accounting** includes the unbroadcast set (#35923).

## Node: private broadcast keeps the proxy on v2 to v1 reconnection

With `-privatebroadcast`, a failed BIP324 v2 handshake made the node retry the connection as v1 directly, without the proxy, which revealed the sender's IP address to the recipient (bitcoin/bitcoin#35410). The proxy override of the first attempt is now kept for the v1 retry, and a private broadcast connection is never opened directly. The option is off by default.

## Connected repositories

The node version is raised on branch `4.0.7` in the repositories that bundle or build `elektrond`:

- `elektron-net-startos`: `VERSION` build argument and the package version (`4.0.7:0`).
- `elektron-net-photonpool`: `NODE` in `src/version.rs`.
- `elektron-net-pool`: `version` in `package.json`.
- `elektron-net-mobile`: README references node `v4.0.7`. The node version of the app comes from the release tag (`v4.0.7-m<wallet version>`), the tag `v4.0.7` must exist in this repository before the app and the StartOS package are built.

## Open items

- **Wallet rebroadcast can reveal the node behind an address** (disclosed on Delving Bitcoin, November 25, 2026): a tiny payment with the minimum fee to a previously used address is rebroadcast after about a day only by the node whose wallet watches that address, which identifies its IP. `CWallet::ResubmitWalletTransactions()` with `MEMPOOL_AND_BROADCAST_TO_ALL` has the same behavior as upstream. Watch bitcoin/bitcoin#36482 (rebroadcast only newly added mempool transactions), which is not merged yet. Until then `-walletbroadcast=0` is the workaround, at the price that the wallet no longer rebroadcasts its own transactions. Check whether a transaction can stay unconfirmed for a day on a 60 second chain with the current miner fee floor.
- Check the Bitcoin Core security advisories and the upstream merges after November 21, 2026 again before the release.

## Version

`CLIENT_VERSION_BUILD` bumped from 6 to 7 in `CMakeLists.txt`
(`v4.0.6` -> `v4.0.7`); `CLIENT_VERSION_MAJOR`/`MINOR` unchanged.
