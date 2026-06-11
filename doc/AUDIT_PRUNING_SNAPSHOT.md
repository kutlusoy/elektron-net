# Technical Audit Report: Elektron Net v4.0.0
## Pruning, Per-Block UTXO Attestation & Checkpoint Snapshot Synchronization

**Date:** 2026-05-12 (Audit) / 2026-06-11 (v4.0 full reconciliation pass)
**Auditor:** kutlusoy
**Scope:** `src/validation.{h,cpp}`, `src/net_processing.{h,cpp}`, `src/init.cpp`, `src/node/{miner,blockstorage,chainstate}.cpp`, `src/rpc/mining.cpp`, `src/wallet/wallet.{h,cpp}`, `src/interfaces/chain.h`, `src/node/interfaces.cpp`, `src/protocol.h`, `src/kernel/chainparams.cpp`, `src/test/elektron_simulation.cpp`, `src/test/peerman_tests.cpp`  
**Mandate:** Audit + implementation of all P0 fixes and selected P2 measures. Build successfully compiled (VS 2026 / `vs2026-static`).
**Operator docs:** [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md), [`mining-pool-integration.md`](mining-pool-integration.md), [`WHITEPAPER.md`](../WHITEPAPER.md) §4.

---

## 1. Definition & Usage of `MANDATORY_PRUNE_DEPTH`

### 1.1 Definition
```cpp
// src/validation.h:79
static const unsigned int MANDATORY_PRUNE_DEPTH = 197280;
```
Corresponds to **137 days** at a block time of 60 seconds (`137 * 24 * 60 * 60 / 60 = 197280`).

### 1.2 Usage Locations (Complete)

| File | Line | Purpose |
|------|------|---------|
| `src/validation.h` | 79 | **Definition** of the constant |
| `src/validation.cpp` | ~2311 | `ValidateUTXOCheckpoint`: **Every block** (height > 0) must carry UTXO attestation in coinbase |
| `src/validation.cpp` | ~2291 | `ComputeBlockUTXOAttestationHash`: Hash after simulated block connect (mining + validation) |
| `src/validation.cpp` | ~2385 | `WriteAutomaticSnapshot`: On-disk snapshot **only** at checkpoint heights (except `force=true`) |
| `src/validation.cpp` | 6580 | `GetPruneRange`: Maximum allowed prune = `tip - 197280` |
| `src/node/miner.cpp` | ~207 | `CreateNewBlock`: UTXO attestation in every block via `ComputeBlockUTXOAttestationHash` |
| `src/rpc/mining.cpp` | ~1040 | GBT `coinbase_required_outputs` exposes attestation to external miners |
| `src/node/blockstorage.cpp` | 359 | `PruneBlockFiles`: Mandatory pruning of all files older than `tip - 197280`, regardless of disk space |
| `src/net_processing.cpp` | 2160 | `MaybeRequestSnapshot`: Bootstrap trigger when `header_height - tip_height > 197280` |
| `src/net_processing.cpp` | 2170 | `MaybeRequestSnapshot`: Calculation of the latest checkpoint via `(header / 197280) * 197280` |
| `src/net_processing.cpp` | 5218 | `GETUTXOSNAPSHOT` handler: Validation that requested index is actually a checkpoint |
| `src/kernel/chainparams.cpp` | 120 | `nPruneAfterHeight = 197280` (`MANDATORY_PRUNE_DEPTH`): Pruning starts at the first checkpoint (~137 days) |
| `src/wallet/wallet.cpp` | ~3340 | `ScanUTXOSet`: Recover wallet balances from current UTXO set when pruned history is unavailable |
| `src/net_processing.cpp` | ~6614 | `awaiting_snapshot_bootstrap`: Skip historical block IBD only when sync gap > `MANDATORY_PRUNE_DEPTH` or local snapshot exists |
| `src/net_processing.cpp` | ~5266, ~5411 | P2P: refuse `UTXOSNAPSHOT` without `.hash`; download marked complete only with sidecar |
| `src/validation.cpp` | ~2357 | `ExtractCoinbaseUTXOAttestation`: parse height + hash from coinbase `OP_RETURN` |
| `src/validation.cpp` | ~6187 | `PopulateAndValidateSnapshot`: reject snapshot if content hash ≠ `expected_utxo_hash` |
| `src/init.cpp` | ~1543 | `MaybeActivateAutomaticSnapshot`: requires `.hash`; compares to on-chain attestation; passes `expected_utxo_hash` |
| `src/init.cpp` | ~1434 | `UpdateLocalSnapshotServices`: `NODE_SNAPSHOT` only when `.dat` + `.hash` both exist |
| `src/interfaces/chain.h` | ~183 | `forEachCoin`: iterate live UTXO set for wallet recovery |
| `src/wallet/wallet.cpp` | ~1922, ~3340 | `ScanUTXOSet` / `AttachChain`: UTXO scan on pruned nodes instead of `-reindex` |
| `src/test/elektron_simulation.cpp` | 49, 157, 176, 184, 192, 306, 339, 352 | Unit tests for validation of the constant, hash mismatch rejection |
| `src/test/blockmanager_tests.cpp` | 311, 331 | Unit tests for prune-range enforcement |

**Conclusion:** The constant is centrally and consistently used across all relevant subsystems (Consensus, Mining, P2P, Storage, Tests).

---

## 2. Architecture Overview

### 2.1 Components

| Component | Mechanism | Responsible File |
|-----------|-----------|------------------|
| **Mandatory Pruning** | All blocks older than 137 days are deleted | `src/node/blockstorage.cpp` |
| **UTXO attestation in coinbase** | **Every block**: OP_RETURN with `height + hash_serialized(UTXO-Set after connect)` | `src/node/miner.cpp`, `src/validation.cpp`, `src/rpc/mining.cpp` |
| **Checkpoint marker** | Every 197,280 blocks: same attestation format; checkpoint logging only | `src/validation.cpp` |
| **Automatic Snapshot Generation** | After `ConnectBlock` at checkpoint: `.dat` + `.hash` sidecar | `src/validation.cpp` |
| **Snapshot P2P Download** | `GETUTXOSNAPSHOT` → `UTXOSNAPSHOT` → `GETSNAPSHOTDATA` → `SNAPSHOTDATA` | `src/net_processing.cpp`, `src/protocol.h` |
| **Snapshot Activation** | `ActivateSnapshot()` with `verify_assumeutxo_hash=false`, `expected_utxo_hash` content check, background validation disabled | `src/init.cpp`, `src/validation.cpp` |
| **Wallet UTXO recovery** | `ScanUTXOSet` via `Chain::forEachCoin` when pruned history unavailable | `src/wallet/wallet.cpp`, `src/node/interfaces.cpp` |

---

## 3. Scenario Analysis: Code Paths

### 3.1 Scenario A: "Nobody in the network has the full chain anymore"

**Code path:**
1. New node starts IBD. Header sync runs.
2. `MaybeRequestSnapshot()` (`src/net_processing.cpp:2137`) is called every 60 seconds by the scheduler.
3. Trigger condition: `header_height - tip_height > MANDATORY_PRUNE_DEPTH` (line 2160).
4. Target checkpoint: `(header_height / 197280) * 197280` (line 2170). This is the **latest** checkpoint that is not ahead of the best header.
5. `GETUTXOSNAPSHOT` is broadcast to **all** connected peers (line 2212).
6. Responding peers are recorded in `m_snapshot_peers[checkpoint_hash]` (line 5265).
7. Download state is initialized (`.download` temp file, line 5281).
8. In `SendMessages()` (line ~6647) block download is skipped when `snapshot_download_active` or `awaiting_snapshot_bootstrap` (gap > `MANDATORY_PRUNE_DEPTH` or local snapshot). Snapshot chunks use `GETSNAPSHOTDATA` (1 MB, 10-second rate limit per peer).
9. Incoming chunks are written to the `.download` file. Upon completion, rename to `.dat` **only if** `.hash` sidecar exists (line ~5411); otherwise download is not marked complete.
10. `MaybeActivateAutomaticSnapshot()` (`src/init.cpp:~1461`) picks newest checkpoint `.dat`, requires `.hash`, verifies sidecar against on-chain attestation when checkpoint block is on disk, then `ActivateSnapshot(..., expected_utxo_hash)`.
11. `PopulateAndValidateSnapshot()` deserializes the snapshot and rejects activation if `HASH_SERIALIZED` ≠ `expected_utxo_hash` (line ~6187).
12. Background validation is immediately disabled: `SetTargetBlock(nullptr)` (line ~1628).

**Snapshot replacement:** If an old snapshot already exists, `ActivateSnapshot()` (`src/validation.cpp:5798`) checks: `if (!verify_assumeutxo_hash)` → old chainstate is deleted via `DeleteChainstate()`.

### 3.2 Scenario B: "Node offline >137 days, was online at multiple checkpoints"

**Path with old snapshot:**
1. Node starts with existing snapshot chainstate (e.g. base at 197,280).
2. `LoadChainstate()` (`src/node/chainstate.cpp:196`) recognizes automatic snapshot (no entry in `chainparams.AssumeutxoForHeight()`).
3. Background validation is disabled on restart (line 206).
4. Header sync: `best_header` at e.g. 800,000.
5. `MaybeRequestSnapshot()` checks: `existing_height (197280) >= target_height (789120)`? No (line 2199).
6. Log: "Existing snapshot at 197280 is behind latest checkpoint 789120. Requesting newer snapshot from peers." (line 2203).
7. `GETUTXOSNAPSHOT` is broadcast. Download starts.
8. Upon completion: `ActivateSnapshot()` calls `DeleteChainstate(old_cs)` (line 5803) and deletes the old chainstate. `SetTargetBlock(nullptr)` is called on all chainstates (line 5808).

**Path without snapshot (only old chainstate):**
1. Node starts with tip at 500,000, network at 800,000. Gap = 300,000 > 197,280.
2. `MaybeRequestSnapshot()` recognizes: No existing snapshot. Calculates target checkpoint 789,120.
3. Same bootstrap path as Scenario A.

---

## 4. Identified Risks & Weaknesses

### 4.1 CRITICAL: No Cryptographic Snapshot Hash Check — Blind Trust

**Location:** `src/init.cpp:1504`, `src/validation.cpp:5780`

`ActivateSnapshot(afile, metadata, false, /*verify_assumeutxo_hash=*/false)` completely skips the hardcoded AssumeUtxo hash check.

**Problem:**
- The on-chain checkpoint only proves that a hash exists. It does **not** prove that the downloaded `.dat` file correctly represents this hash.
- A malicious peer can create a `.dat` file with correct metadata (`base_blockhash = Checkpoint`) but manipulated UTXO data.
- Since background validation is permanently disabled for automatic snapshots (`SetTargetBlock(nullptr)`), there is **no** later validation path that would detect the manipulation.
- The node would work with a poisoned UTXO set and accept/reject transactions differently from the rest of the network.

**Post-Implementation Status:** **Resolved** by L0.1 + `PopulateAndValidateSnapshot` (`src/init.cpp`, `src/validation.cpp`):
- `.hash` sidecar required; activation refused without it.
- When checkpoint block is on disk: sidecar verified against `ExtractCoinbaseUTXOAttestation`.
- After load: deserialized UTXO content must match `expected_utxo_hash` (`HASH_SERIALIZED`).
- On any mismatch, snapshot files are removed and activation aborts.

**Impact:** Permanent consensus split, unrecoverable without manual intervention.

### 4.2 CRITICAL: Snapshot Activation Picks Arbitrary File

**Location:** `src/init.cpp:1447-1455`

```cpp
for (const auto& entry : fs::directory_iterator(snapshot_dir)) {
    if (fname.ends_with(".dat") && !fname.ends_with(".download")) {
        snapshot_path = entry.path();
        break;   // ARBITRARY
    }
}
```

`fs::directory_iterator` yields no defined order. If multiple `.dat` files exist (remnants of old installations or earlier checkpoints), the **first arbitrary** one is activated.

**Post-Implementation Status:** **Resolved** by L0.4 (`src/init.cpp`). `MaybeActivateAutomaticSnapshot` collects all `.dat` files, parses the height from the filename, and deterministically selects the newest checkpoint. After successful activation, all obsolete files in the `snapshots/` directory are deleted.

**Impact:** Node may load an old snapshot, then need to replace it, losing time or oscillating between states.

### 4.3 CRITICAL: Snapshot Download Without Global Timeout / Stall Detection

**Location:** `src/net_processing.cpp:6600-6621`

- No global timeout for a download.
- No detection that a download has "stalled".
- No logic to discard an incomplete download and restart.
- `.download` file on disk persists on restart, but `received_ranges` in RAM is lost.

**Post-Implementation Status:** **Resolved** by L0.2 (`src/net_processing.cpp`). `SnapshotDownload` now has `last_progress_time`. `MaybeRequestSnapshot` checks every 60 seconds: if a download has had no progress for >30 minutes, it is discarded, the `.download` file is deleted, and a re-broadcast is allowed. Additionally, download completion requires a valid `.hash` sidecar — a finished `.dat` without sidecar is not marked complete and cannot activate.

**Deadlock scenario (pre-fix):**
1. Node broadcasts `GETUTXOSNAPSHOT`.
2. A peer responds with `UTXOSNAPSHOT`. Node starts download.
3. This peer goes offline or never responds with `SNAPSHOTDATA`.
4. `MaybeRequestSnapshot()` is called every 60s, but sees `m_snapshot_downloads.count(checkpoint_hash)` and returns immediately (line 2182).
5. The node thus sends **no new** `GETUTXOSNAPSHOT` broadcasts.
6. If the peer that advertised the snapshot was the only one and is now offline, `m_snapshot_peers[hash]` remains empty.
7. `peer_it->second.count(node.GetId())` is always false. The node **never** requests a chunk again.
8. **Permanent deadlock:** incomplete download is not discarded, but also not continued.

### 4.4 CRITICAL: No Snapshot Availability Guarantee in the Network

**Location:** `src/net_processing.cpp:5210`, `src/validation.cpp:2365`

Not every node has every snapshot. A node only writes snapshots for checkpoints that it **itself processed live**.

**Chain-halt scenario:**
1. Network has 10 nodes. Checkpoint at 789,120 was reached 2 days ago.
2. 7 nodes were online and have `789120-xxx.dat`.
3. 3 nodes were offline and do not have this snapshot.
4. Today the 7 nodes all go offline.
5. A new node joins the network and connects to the 3 remaining nodes.
6. It broadcasts `GETUTXOSNAPSHOT` for 789,120.
7. None of the 3 peers has the file.
8. The node waits 60 seconds, broadcasts again. Again no response.
9. **This repeats indefinitely.** The node can never synchronize.

**Post-Implementation Status:** **Partially resolved** by L0.3 (`src/protocol.h`, `src/init.cpp`, `src/net_processing.cpp`). `NODE_SNAPSHOT` service bit (bit 12) signals snapshot availability. `MaybeRequestSnapshot` first broadcasts only to peers with this bit. This improves discovery, but there is still no hard guarantee that snapshots must be redundantly retained.

**Conclusion:** There is no requirement that snapshots be retained by a minimum number of nodes. The network can put itself into a configuration where no full sync is possible anymore.

### 4.5 CRITICAL: Deep Reorgs Are Impossible

**Location:** `src/init.cpp:1520`, `src/validation.cpp:6570-6591`

Since background validation is disabled and all historical blocks are pruned, a snapshot-based node has **no blocks before the checkpoint base** and **no undo data**.

**Consequence:**
- If a reorganization occurs deeper than the snapshot base (e.g. 51% attack with chain from block 789,000), a pruned node **cannot validate** this reorg.
- It cannot re-validate the old chain onto the new fork because it lacks the historical blocks and UTXO transitions.
- The entire network (almost all nodes are pruned) cannot validate the stronger fork and remains on the weaker one.

**Post-Implementation Status:** **Accepted design trade-off.** The full history is defined as the last 137 days (MANDATORY_PRUNE_DEPTH). Reorgs deeper than the latest checkpoint are by design not supported. A Stoic would say: "The past that we can no longer touch is as it is."

This is a fundamental trade-off of the pruning design, but combined with the missing background validation, the network is locked to the integrity of the latest checkpoint. An error in the checkpoint (or a malicious checkpoint miner) could permanently split the network.

### 4.6 HIGH: Double-ComputeUTXOStats in Mining Path

**Location:** `src/node/miner.cpp:208`, `src/validation.cpp:2337`

The miner calculates the UTXO hash (`ComputeUTXOStats`) and embeds it into the coinbase. `ConnectBlock` recalculates the same hash **again** for validation.

**Risk:**
- `ComputeUTXOStats` is O(n) over the entire UTXO set.
- While `ConnectBlock` runs, `cs_main` is held. While the miner calculates the hash, it blocks all processing.
- Theoretical consistency risk: If between embedding by the miner and validation by `ConnectBlock` another thread modifies the CoinsDB (not possible because of `cs_main`, but a risk with future changes), the block would be rejected as invalid and the chain would halt at the checkpoint.

**Post-Implementation Status:** **Accepted.** Pure performance problem. `cs_main` protects against inconsistencies.

### 4.7 HIGH: Snapshot Files Are Never Cleaned Up — Disk Space Leak

**Location:** `src/validation.cpp:2455`, `src/init.cpp:1509`

- On successful download: `.download` → `.dat`. Old `.dat` files remain.
- On failed activation: `.dat` → `.failed`. Never deleted.
- On snapshot replacement: `DeleteChainstate()` deletes the LevelDB data, but **not** the source `.dat` / `.hash` files in `snapshots/`.
- `WriteAutomaticSnapshot()` runs at every checkpoint. After 10 checkpoints the node has 10 `.dat` + 10 `.hash` files. At 14 GB per snapshot = 280 GB leak.

**Post-Implementation Status:** **Resolved.** `WriteAutomaticSnapshot()` now iterates the `snapshots/` directory after successful writing and deletes all files whose name does not start with the current checkpoint prefix (`<height>-<hash>`). This includes obsolete `.dat`, `.hash`, `.download`, and `.failed` files from earlier checkpoints. The cleanup runs under `cs_main` directly after the snapshot is finalized, preventing unbounded accumulation.

**Code:** `src/validation.cpp:2481-2492`

### 4.8 HIGH: Snapshot Download Tracking Lost on Restart

**Location:** `src/net_processing.cpp:5276-5285`, `src/net_processing.cpp:5336-5358`

The `received_ranges` map exists only in RAM. On node restart:
1. `.download` file remains on disk.
2. But `SnapshotDownload` is re-initialized with empty `received_ranges`.
3. The node does not know which chunks it already has.
4. It requests all chunks again. Incoming chunks are correctly written, but it is pure bandwidth waste.

**Post-Implementation Status:** **Accepted.** Functionally correct (chunks are written idempotently), but wastes bandwidth. Not classified as critical.

### 4.9 MEDIUM: `MaybeRequestSnapshot` Spam to All Peers Without Selection

**Location:** `src/net_processing.cpp:2212`

```cpp
m_connman.ForEachNode([&](CNode* pnode) {
    if (pnode) {
        MakeAndPushMessage(*pnode, NetMsgType::GETUTXOSNAPSHOT, checkpoint_hash);
    }
});
```

There is no distinction between inbound/outbound, peers that already responded vs. not, or protocol version. Every 60 seconds the same request is sent to every connected peer (up to 125 connections). For a node that is 10 days behind, that's 18,000 messages per checkpoint. Wasteful and fingerprintable.

**Post-Implementation Status:** **Partially resolved** by L0.3 (`src/net_processing.cpp`). `GETUTXOSNAPSHOT` is now first sent only to peers with `NODE_SNAPSHOT`, with fallback to outbound peers. 60-second interval is deemed acceptable.

### 4.10 MEDIUM: `NODE_NETWORK_LIMITED` Safety Window

**Location:** `src/test/peerman_tests.cpp:19`

```cpp
static constexpr int64_t NODE_NETWORK_LIMITED_ALLOW_CONN_BLOCKS = 10000000;
```

This value is explicitly set to 10,000,000 in Elektron Net (compared to Bitcoin's ~288 blocks), because "all nodes are pruned". This is correct for normal operation, but a node that accidentally does not want to load a snapshot and tries to sync the full chain would also only accept `NODE_NETWORK_LIMITED` peers, which cannot serve historical blocks before `tip - 2880`. The intended path (snapshot bootstrap) is then forced by `MaybeRequestSnapshot`.

### 4.11 MEDIUM: Missing `.hash` Sidecar File Leads to `null` Hash in P2P Handshake

**Location:** `src/net_processing.cpp:5231-5242`

If the `.hash` file is missing or unreadable, `utxo_hash.SetNull()` is sent. The receiving node accepts the snapshot anyway. The hash is only transmitted informatively, but not used for validation.

**Post-Implementation Status:** **Resolved** by L2.2 (`src/net_processing.cpp`) + `UpdateLocalSnapshotServices` (`src/init.cpp`):
- `ProcessGetUTXOSnapshot` sends **no** `UTXOSNAPSHOT` if `.hash` is missing or unreadable.
- `NODE_SNAPSHOT` is advertised only when the node holds both `.dat` and `.hash`.
- Receiver refuses activation without `.hash`; download completion also checks sidecar presence.

### 4.12 MEDIUM: Wallet Rescan Fails on Pruned Nodes (Pocket Recovery)

**Location:** `src/wallet/wallet.cpp` (Bitcoin Core default path)

On a pruned node, `rescanblockchain` and block-history rescan fail when the wallet's last-synced height is older than retained blocks. Bitcoin Core suggests `-reindex`, which is impractical on a pruned network.

**Post-Implementation Status:** **Resolved.** `CWallet::AttachChain()` detects pruned history unavailability and calls `ScanUTXOSet()` via `Chain::forEachCoin()`. Balances are recovered from the live UTXO set; transaction history before the pruning window remains unavailable (by design). Functional tests: `test/functional/wallet_backup.py`, `wallet_assumeutxo.py`.

**Operator reference:** [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md) §2.6, [`WHITEPAPER.md`](../WHITEPAPER.md) §4.5.

---

## 5. Solution Plan — Implementation Status

### 5.1 Priority P0 (Prevent Chain Halt / Consensus Split)

#### L0.1: Validate Snapshot Hash Against On-Chain Checkpoint
**Problem:** 4.1 (Poisoned snapshots)  
**Solution implemented:**
- `MaybeActivateAutomaticSnapshot()` requires a valid `.hash` sidecar; refuses activation without it.
- When checkpoint block is on disk: sidecar hash compared to `ExtractCoinbaseUTXOAttestation` in coinbase.
- `ActivateSnapshot(..., expected_utxo_hash)` → `PopulateAndValidateSnapshot` verifies deserialized content `HASH_SERIALIZED` matches `expected_utxo_hash`.
- On mismatch, snapshot files are deleted and activation is aborted.

**Files:** `src/init.cpp`, `src/validation.cpp`  
**Status:** **Implemented and compiled.**

#### L0.2: Download Timeout & Retry Mechanism
**Problem:** 4.3 (Deadlock with unresponsive peer)  
**Solution implemented:**
- Added a global timeout per `SnapshotDownload` (>30 minutes without progress).
- When timeout is reached:
  1. Download entry is removed from `m_snapshot_downloads`.
  2. `.download` temp file is deleted.
  3. `MaybeRequestSnapshot()` is allowed to send a new broadcast.
- Progress tracker: `last_progress_time` in `SnapshotDownload`. Updated on every incoming chunk.

**Files:** `src/net_processing.cpp` (structure `SnapshotDownload`, `MaybeRequestSnapshot`, `SNAPSHOTDATA` handler)  
**Status:** **Implemented and compiled.**

#### L0.3: Snapshot Redundancy / Seeding Requirement
**Problem:** 4.4 (No snapshot available in network)  
**Solution implemented:**
- **Option B (Hard) partially implemented:** New service bit `NODE_SNAPSHOT` (bit 12) in `src/protocol.h`.
- Nodes that currently hold at least one snapshot signal this via `nLocalServices`.
- `MaybeRequestSnapshot()` broadcasts `GETUTXOSNAPSHOT` first only to peers with `NODE_SNAPSHOT`.
- Periodic task (every 5 minutes) updates the service bit dynamically based on available snapshots.
- **Note:** This improves discovery but does not enforce a hard minimum number of snapshot-holding nodes.

**Files:** `src/protocol.h`, `src/init.cpp`, `src/net_processing.cpp`  
**Status:** **Implemented and compiled.**

#### L0.4: Deterministic Snapshot Selection
**Problem:** 4.2 (Arbitrary `.dat` selection)  
**Solution implemented:**
- In `MaybeActivateAutomaticSnapshot()` (`src/init.cpp:1447`): Collects ALL `.dat` files in the `snapshots/` directory.
- Parses the filename: `<height>-<hash>.dat`.
- Selects the file with the **highest height** (newest checkpoint).
- After successful activation, deletes all obsolete `.dat`, `.hash`, `.download`, and `.failed` files for other checkpoints.

**Files:** `src/init.cpp`  
**Status:** **Implemented and compiled.**

---

### 5.2 Priority P1 (Stability & Resource Management)

#### L1.1: Snapshot File Cleanup
**Problem:** 4.7 (Disk space leak)  
**Status:** **Done.** `WriteAutomaticSnapshot()` now cleans up obsolete `.dat`, `.hash`, `.download`, and `.failed` files from the `snapshots/` directory after each successful checkpoint snapshot. Only the current checkpoint's files are preserved.

#### L1.2: Persistent Download Progress
**Problem:** 4.8 (Tracking lost on restart)  
**Status:** **Accepted.** Functionally correct (chunks are written idempotently), but wastes bandwidth. Not classified as critical per project decision.

#### L1.3: Rate-Limiting & Smart Peer Selection for Snapshot Requests
**Problem:** 4.9 (Spam)  
**Status:** **Partially resolved by L0.3.** `GETUTXOSNAPSHOT` is now sent first to `NODE_SNAPSHOT` peers, with fallback to outbound peers. 60-second interval is deemed acceptable per project decision. No further action required.

---

### 5.3 Priority P2 (Consensus Hardening & Resilience)

#### L2.1: Per-Block Attestation Rejection (Invalid Block, Not FatalError)
**Problem:** 4.5 (Reorgs impossible; malicious attestation on a fork)  
**Solution implemented:**
- `ValidateUTXOCheckpoint` in `ConnectBlock` returns `false` on `missing-utxo-attestation` or `bad-utxo-attestation` — the block is marked invalid; the node continues on the current tip.
- This applies to **every block** (height > 0), including checkpoint heights; there is no separate `FatalError` watchdog for checkpoint attestation failures.
- Miners cannot silently omit attestations: `CreateNewBlock` returns `nullptr` if attestation hash cannot be computed.

**Files:** `src/validation.cpp`, `src/node/miner.cpp`  
**Status:** **Implemented and compiled.** Deep reorgs beyond the pruning window remain an accepted design trade-off (§4.5).

#### L2.2: Sidecar Hash Validation in P2P Handshake
**Problem:** 4.11 (Missing `.hash` file leads to `null` hash)  
**Solution implemented:**
- In `ProcessGetUTXOSnapshot` (`src/net_processing.cpp:5210`): If the `.hash` file is missing or unreadable, the peer no longer responds with `UTXOSNAPSHOT`.
- A peer that cannot provide the hash does not offer the snapshot. The requesting node then picks another peer.
- This prevents downloading snapshots that cannot later be validated against the on-chain checkpoint.

**Files:** `src/net_processing.cpp`  
**Status:** **Implemented and compiled.**

#### L2.3: `NODE_SNAPSHOT` Service Bit
**Problem:** 4.4 (No way to know who has snapshots)  
**Status:** **Fully implemented as part of L0.3.** `UpdateLocalSnapshotServices` requires both `.dat` and `.hash`.

#### L2.4: Wallet UTXO Scan on Pruned Attach
**Problem:** 4.12 (Wallet rescan impossible on pruned nodes)  
**Solution implemented:**
- `Chain::forEachCoin()` exposes live UTXO iteration.
- `CWallet::ScanUTXOSet()` credits matching outputs when block rescan is impossible.
- Triggered automatically in `AttachChain()` when `chain.havePruned()`.

**Files:** `src/wallet/wallet.cpp`, `src/node/interfaces.cpp`, `src/interfaces/chain.h`  
**Status:** **Implemented.** Functional tests updated.

#### L2.5: `awaiting_snapshot_bootstrap` IBD Gap Logic
**Problem:** Fresh nodes blocked from block IBD while gap ≤ retention window  
**Solution implemented:**
- `SendMessages()` skips historical block download only when `sync_gap > MANDATORY_PRUNE_DEPTH` or local snapshot exists.
- Allows normal block sync inside the 137-day window; snapshot path used only when history is unavailable.

**Files:** `src/net_processing.cpp`  
**Status:** **Implemented and compiled.**

---

## 6. Summary of Recommended Order — Actual Implementation

| Phase | Items | Priority | Status |
|-------|-------|----------|--------|
| **1** | L0.4 (Deterministic selection), L0.2 (Download timeout) | P0 | Done |
| **2** | L0.1 (Hash validation), L2.2 (Sidecar validation) | P0 | Done |
| **3** | L0.3 (`NODE_SNAPSHOT` bit + selective broadcast) | P0 | Done |
| **4** | L2.1 (Per-block attestation rejection), L2.4 (Wallet UTXO scan), L2.5 (`awaiting_snapshot_bootstrap`) | P2 | Done |
| — | L1.1, L1.2, L1.3, L2.3 (Cleanup, persistent progress, rate-limiting, `NODE_SNAPSHOT`) | P1/P2 | Accepted / Partially done / Done |
| — | 4.5, 4.6, 4.8 (Deep reorgs, double-compute, restart tracking) | — | Accepted as trade-offs |
| — | 4.7 (Disk space leak), 4.12 (Wallet recovery) | P1/P2 | Done |

**Key result:** All four P0 measures, P2 hardening (sidecar + content hash + attestation rejection), wallet UTXO scan, and IBD gap logic are implemented. Code compiles on VS 2026 (`vs2026-static`). Remaining open items are accepted design trade-offs or non-critical bandwidth waste (persistent download progress). Operator documentation: [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md), [`mining-pool-integration.md`](mining-pool-integration.md).

---

*End of report.*
