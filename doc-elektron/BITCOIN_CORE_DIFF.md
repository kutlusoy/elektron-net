# Elektron Net vs Bitcoin Core — Technical Diff Reference

**Version:** 4.0.0  
**Base:** Bitcoin Core (C++20 fork)  
**Last updated:** 2026-07-01  
**Companion docs:** [`WHITEPAPER.md`](../WHITEPAPER.md), [`mining-pool-integration.md`](mining-pool-integration.md), [`AUDIT_PRUNING_SNAPSHOT.md`](AUDIT_PRUNING_SNAPSHOT.md), [`hardfork-v3.0.1-stoic-awakening.md`](hardfork-v3.0.1-stoic-awakening.md), [`fix-report-utxo-attestation-scalability.md`](fix-report-utxo-attestation-scalability.md), [`CHANGELOG-muhash-attestation.md`](CHANGELOG-muhash-attestation.md)

This document lists **every deliberate divergence** from upstream Bitcoin Core, organized from protocol fundamentals outward to tooling and tests. Files with only branding or string changes are grouped separately.

---

## 1. Executive Summary

| Area | Bitcoin Core | Elektron Net |
|------|--------------|--------------|
| Block time | 600 s (10 min) | **60 s** |
| Blocks / day | 144 | **1,440** |
| Halving interval | 210,000 blocks | **2,102,400 blocks** (same ~4 calendar years) |
| Genesis reward | 50 BTC | **5 ELEK** |
| Smallest unit | satoshi | **lepton** (1 ELEK = 10⁸ leptons) |
| Pruning | Optional, user `-prune` GB target | **Mandatory**, depth-based (197,280 blocks) |
| UTXO attestation | None | **Every block** (coinbase `OP_RETURN`) |
| UTXO snapshot files | Manual AssumeUTXO (hardcoded hashes) | **Automatic** at checkpoints + P2P download |
| Snapshot trust model | Hardcoded `assumeutxo` hash in chainparams | **On-chain attestation** + `.hash` sidecar; content verified at activation |
| Wallet rescan on pruned node | Fails; suggests `-reindex` | **UTXO-set scan** recovers balances (Pocket philosophy) |
| `rescanblockchain` beyond prune height | Fails | Still fails; reload wallet for UTXO scan at startup |
| Min-difficulty recovery | Testnet only | **Stoic Awakening** on mainnet (after >120 s delay, from block 1) |
| Network magic (mainnet) | `0xf9beb4d9` | **`0xe1ec7a6e`** |
| DNS seed (mainnet) | `seed.bitcoin.sipa.be` etc. | **`seed.elektron-net.org`** |
| User agent | `Bitcoin` | **`Elektron`** |
| Currency symbol (RPC/GUI) | BTC | **ELEK** |
| P2P port / RPC port | 8333 / 8332 | **8333 / 8332** (unchanged) |
| `PROTOCOL_VERSION` | 70016 (typical) | **70017** |
| `MIN_PEER_PROTO_VERSION` | Lower (compat) | **70017** from genesis |

**v4.0 genesis restart:** new chain from block 0; old datadirs are incompatible.

---

## 2. Consensus & Protocol Rules (New Behaviour)

### 2.1 Mandatory 137-day pruning

- Constant: `MANDATORY_PRUNE_DEPTH = 197280` (`src/validation.h`) — this is now specifically the **mainnet value and default** that `Consensus::Params::MandatoryPruneDepth` (`src/consensus/params.h`) initializes to; testnet/testnet4/regtest override that field to a much lower value so the checkpoint cycle in §2.3 is actually observable without mining 197,280 blocks (see §2.3 and `doc-elektron/CHANGELOG-muhash-attestation.md`). All consensus/checkpoint code reads `Consensus::Params::MandatoryPruneDepth`, not the bare constant.
- All nodes prune block files older than the network's checkpoint interval from the tip, **regardless of disk space** (`src/node/blockstorage.cpp`).
- User `-prune=<GB>` is **ignored**; nodes always use manual pruning mode with depth-based retention (`src/node/blockmanager_args.cpp`, `src/init.cpp`).
- `-txindex` is incompatible (same as Bitcoin, but pruning is always on).
- `MIN_BLOCKS_TO_KEEP = 2880` (~2 days at 60 s) — safety buffer before mandatory depth (`src/validation.h`).
- `nPruneAfterHeight = 197280` on mainnet (`src/kernel/chainparams.cpp`) — pruning **starts at the first checkpoint**, not after a 274-day grace period. There is no `2 × MandatoryPruneDepth` delay. (`nPruneAfterHeight` is a separate, disk-pruning-only mechanism and was not changed for testnet/testnet4/regtest as part of the checkpoint-interval work below.)

### 2.2 Per-block UTXO attestation

- **Every block** at height > 0 must include in coinbase:  
  `OP_RETURN <height> <attestation hash (32 bytes)>`.
- Genesis (height 0) has no attestation.
- Validation errors: `missing-utxo-attestation`, `bad-utxo-attestation`, `bad-utxo-attestation-compute`.
- **`ConnectBlock`**: attestation failure returns `false` (invalid block) — not `FatalError` (node keeps running).
- **`CreateNewBlock` / GBT**: if attestation hash cannot be computed, template creation returns `nullptr` / RPC error (no silent omission).
- Miners receive the required output via GBT `coinbase_required_outputs` (`src/rpc/mining.cpp`). The 32-byte hash format is unchanged by the dual algorithm below — pools following this contract need no changes.
- **Dual algorithm, height-gated** (`Consensus::Params::MuhashAttestationActivationHeight`, `src/consensus/params.h`; see `doc-elektron/fix-report-utxo-attestation-scalability.md` and `doc-elektron/CHANGELOG-muhash-attestation.md` for the full design/implementation history):
  - Below the activation height (or when the height is `-1`, disabled): `HASH_SERIALIZED` — a full rescan of the UTXO set after connecting the block (`kernel::ComputeUTXOStats`, `src/kernel/coinstats.cpp`). This is the only algorithm ever used on **mainnet** (`MuhashAttestationActivationHeight = -1`, permanently, until a separate decision is made).
  - At/after the activation height: an **incrementally-maintained MuHash accumulator** (`kernel::UTXOMuHashState`, `src/kernel/utxo_muhash.h`), updated per-block from `ConnectBlock`/`DisconnectBlock` and persisted alongside the chainstate (`CCoinsViewDB::WriteUTXOMuHashState`, `src/txdb.cpp`). Cost is bounded by the coins touched in the block being processed, not by total UTXO set size — this removes a per-block cost that otherwise scales with the UTXO set (see the fix report for the concrete threshold math).
  - Current activation heights (`src/kernel/chainparams.cpp`): **mainnet `-1` (disabled)**, testnet/testnet4 `5000` (verify against the live tip before deploying if time has passed), regtest `50` (fixed, exercised by every regtest run, high enough to leave a visible pre-activation window for manual testing).
  - `ComputeBlockUTXOAttestationHash()` picks the algorithm transparently based on height; callers (`ValidateUTXOCheckpoint`, `CreateNewBlock`) are unaffected either way.

### 2.3 Checkpoint snapshot files (every `Consensus::Params::MandatoryPruneDepth` blocks — 197,280 on mainnet)

- Checkpoint interval is now **per-network** (see §2.1): mainnet `197280`, testnet/testnet4 `7000`, regtest `100` — so the automatic snapshot cycle below can actually be observed while testing on testnet/regtest, not just after 197,280 blocks.
- On-disk `.dat` + `.hash` sidecar written **only** at checkpoint heights (`WriteAutomaticSnapshot`).
- If `.hash` sidecar write fails, the `.dat` file is **removed** (snapshot unusable without hash).
- Uses AssumeUTXO serialization; **no** hardcoded `assumeutxo` entries in `chainparams` for automatic snapshots.
- **Activation security** (`MaybeActivateAutomaticSnapshot`, `PopulateAndValidateSnapshot`):
  - Requires valid `.hash` sidecar; refuses activation without it.
  - When checkpoint block is on disk: verifies `.hash` against on-chain coinbase attestation (`ExtractCoinbaseUTXOAttestation`).
  - Always verifies deserialized snapshot content against `expected_utxo_hash` (from sidecar).
  - Picks **newest** checkpoint file deterministically; deletes obsolete snapshots after success.
- P2P bootstrap via new messages (see §5).
- **`NODE_SNAPSHOT`**: advertised only when node has both `.dat` and `.hash` for a checkpoint.

### 2.4 Stoic Awakening (mainnet min-difficulty recovery)

- `MinDifficultyActivationHeight = 1` on mainnet (`src/kernel/chainparams.cpp`).
- If time since last block **> 120 s** (2× target spacing), next block may use `powLimit` difficulty.
- Implemented in `src/pow.cpp`, `src/node/miner.cpp` (`GetNextWorkRequired` / template `nBits` logic).

### 2.5 Economic parameters (unchanged cap, rescaled schedule)

- `MAX_MONEY = 21_000_000 * COIN` (`src/consensus/amount.h`).
- `nSubsidyHalvingInterval = 2_102_400`.
- `nPowTargetSpacing = 60`, `nPowTargetTimespan = 2016 * 60`.
- BIP34/65/66/CSV/Segwit active from height **1** on mainnet.

### 2.6 Wallet: Pocket recovery without block history

When a wallet's last-synced height is older than available pruned blocks, Elektron Net does **not** require `-reindex` or a full chain download:

1. **`interfaces::Chain::forEachCoin()`** — iterates the live chain UTXO set (`src/node/interfaces.cpp`).
2. **`CWallet::ScanUTXOSet()`** — credits outputs matching wallet keys (`src/wallet/wallet.cpp`).
3. **`CWallet::CreditUTXOFromChain()`** — builds partial `CWalletTx` entries keyed by real txid.

Triggered automatically in `CWallet::AttachChain()` when `chain.havePruned()` and block rescan is impossible. User sees a warning: *"Wallet balances recovered from the current UTXO set. Pruned transaction history is unavailable."*

**Implication for wallet vendors:** seed + UTXO scan restores spendable balance; **transaction history before the pruning window is not recoverable** from the network (by design). See [`WHITEPAPER.md`](../WHITEPAPER.md) §4.3 (Pocket philosophy).

---

## 3. New Global Symbols & Functions

### 3.1 New constants

| Symbol | Location | Value / meaning |
|--------|----------|-----------------|
| `MANDATORY_PRUNE_DEPTH` | `src/validation.h` | `197280` |
| `ELEKTRON_MANDATORY_PRUNE_WINDOW_GB` | `src/qt/guiconstants.h` | `10` (planning estimate only) |
| `NODE_SNAPSHOT` | `src/protocol.h` | Service bit `1 << 12` |
| `CURRENCY_UNIT` | `src/policy/feerate.h` | `"ELEK"` |
| `COIN` | `src/consensus/amount.h` | 10⁸ leptons per ELEK |
| `Consensus::Params::MuhashAttestationActivationHeight` | `src/consensus/params.h` | `-1` disabled (mainnet); per-network heights in `src/kernel/chainparams.cpp` (see §2.2) |
| `Consensus::Params::MandatoryPruneDepth` | `src/consensus/params.h` | Defaults to `197280` (mainnet); `7000` testnet/testnet4, `100` regtest (see §2.1/§2.3) |
| `DB_UTXO_MUHASH` | `src/txdb.cpp` | Coins-DB key (`'U'`) for the persisted MuHash accumulator |

### 3.2 New P2P message types

| Message | Payload (summary) |
|---------|-------------------|
| `getutxosnapshot` | `uint256` checkpoint block hash |
| `utxosnapshot` | height, block hash, UTXO hash, file size |
| `getsnapshotdata` | block hash, offset, length |
| `snapshotdata` | block hash, offset, data chunk |

Defined in `src/protocol.h`; handlers in `src/net_processing.cpp`.

### 3.3 New functions (Elektron-specific)

| Function | File | Purpose |
|----------|------|---------|
| `ComputeBlockUTXOAttestationHash()` | `src/validation.cpp` | Return UTXO attestation hash: full rescan pre-activation, incremental MuHash clone post-activation |
| `ExtractCoinbaseUTXOAttestation()` | `src/validation.cpp` | Parse `OP_RETURN` attestation from coinbase at height |
| `ValidateUTXOCheckpoint()` | `src/validation.cpp` | Consensus: verify coinbase attestation every block |
| `Chainstate::EnsureUTXOMuHashLoaded()` / `Chainstate::UTXOMuHash()` | `src/validation.h/.cpp` | Lazy-load (or one-time rebuild) and expose the persistent MuHash accumulator |
| `kernel::UTXOMuHashState` | `src/kernel/utxo_muhash.h` | Incrementally-maintained MuHash commitment to the full UTXO set |
| `WriteAutomaticSnapshot()` | `src/validation.cpp` | Write `.dat` + `.hash` at checkpoint heights; abort if hash write fails |
| `PopulateAndValidateSnapshot(..., expected_utxo_hash)` | `src/validation.cpp` | Load snapshot; verify content hash against sidecar / attestation |
| `DirectorySize()` | `src/util/fs_helpers.cpp` | Recursive directory size (GUI disk display) |
| `MaybeRequestSnapshot()` | `src/net_processing.cpp` | IBD: request snapshot when `header - tip >= MANDATORY_PRUNE_DEPTH` |
| `UpdateLocalSnapshotServices()` | `src/init.cpp` | Set `NODE_SNAPSHOT` only when `.dat` + `.hash` exist |
| `MaybeActivateAutomaticSnapshot()` | `src/init.cpp` | Activate downloaded snapshot during IBD (hash-verified) |
| `Chain::forEachCoin()` | `src/interfaces/chain.h`, `src/node/interfaces.cpp` | Iterate live UTXO set (wallet scan) |
| `CWallet::ScanUTXOSet()` | `src/wallet/wallet.cpp` | Recover wallet balances without block history |
| `CWallet::CreditUTXOFromChain()` | `src/wallet/wallet.cpp` | Credit single UTXO into wallet map |

### 3.4 New struct / class members (`PeerManagerImpl`)

In `src/net_processing.cpp` (snapshot download state):

- `SnapshotDownload`: `checkpoint_hash`, `file_size`, `received_ranges`, `temp_path`, `final_path`, `completed`, `utxo_hash`
- `m_snapshot_peers`, `m_snapshot_downloads`
- `m_snapshot_bootstrap_target`, `m_last_snapshot_request`
- Download completes only when `.hash` sidecar exists; chunk stall timeout 30 min

### 3.5 New test-only hooks

| Hook | File |
|------|------|
| `PeerManager::MaybeRequestSnapshot()` virtual | `src/net_processing.h` |
| `elektron_simulation_tests` suite | `src/test/elektron_simulation.cpp` |

### 3.6 New repository files (not in Bitcoin Core)

| Path | Purpose |
|------|---------|
| `mining/miner.py` | Reference CPU miner (GBT + `coinbase_required_outputs`) |
| `mining/miner.cpp` | C++ reference miner |
| `mining/mine_genesis.py` | Genesis block mining helper |
| `mining/generate_address.py` | Address helper |
| `mining/GENESIS.md`, `mining/README.md` | Mining docs |
| `src/test/elektron_simulation.cpp` | Pruning/snapshot/attestation tests |
| `doc-elektron/AUDIT_PRUNING_SNAPSHOT.md` | Internal audit |
| `doc-elektron/BITCOIN_CORE_DIFF.md` | This document |
| `doc-elektron/mining-pool-integration.md` | **Pool / Stratum / GBT integrator guide** |
| `doc-elektron/hardfork-v3.0.1-stoic-awakening.md` | Stoic Awakening spec |
| `WHITEPAPER.md`, `right-to-be-forgotten.md` | Project documentation |

---

## 4. Removed or Disabled Behaviour

| Bitcoin Core behaviour | Elektron Net |
|------------------------|--------------|
| Optional pruning (`-prune=0` full node) | **Removed** — always pruned |
| User-chosen prune target (GB) | **Ignored** — `PRUNE_TARGET_MANUAL` always |
| GUI prune checkbox / size spinbox | **Disabled** — shows measured usage |
| `-txindex` with pruning | Still **disallowed** |
| Checkpoint-only UTXO hash (if ever planned) | Replaced by **per-block** attestation |
| Hardcoded `m_assumeutxo_data` for auto snapshots | **Empty** on mainnet; snapshots validated via on-chain hash |
| AssumeUTXO background validation for auto snapshots | **Disabled** (`SetTargetBlock(nullptr)`) |
| Bitcoin mainnet chain / genesis | **Replaced** (v4.0 restart) |
| Fixed seed nodes in `chainparamsseeds.h` | **Placeholder only** — `vFixedSeeds.clear()`; DNS seed used |
| Blind snapshot activation (`verify_assumeutxo_hash=false` without hash check) | **Removed** — content + attestation verification required |
| 274-day pruning grace (`nPruneAfterHeight = 2 × depth`) | **Removed** — prune starts at 197,280 blocks |

**No Bitcoin Core functions were deleted wholesale.** Changes are additive or conditional branches on Elektron rules.

---

## 5. File-by-File Reference

Legend: **●** = functional change · **○** = branding/docs only · **＋** = new file

### 5.1 Consensus, validation, storage

#### `src/validation.h` ●

- Added `MANDATORY_PRUNE_DEPTH`, adjusted `MIN_BLOCKS_TO_KEEP` comment (2880 @ 60 s).
- Declares `ComputeBlockUTXOAttestationHash`, `ExtractCoinbaseUTXOAttestation`, `ValidateUTXOCheckpoint`, `WriteAutomaticSnapshot`.
- `ActivateSnapshot` / `PopulateAndValidateSnapshot`: optional `expected_utxo_hash` for automatic snapshot hash verification.

#### `src/validation.cpp` ●

- **`ComputeBlockUTXOAttestationHash`**: `CCoinsViewCache` + `UpdateCoins` over block txs → `HASH_SERIALIZED`.
- **`ExtractCoinbaseUTXOAttestation`**: parses height + 32-byte hash from coinbase `OP_RETURN`.
- **`ValidateUTXOCheckpoint`**: required every height > 0; full hash check; checkpoint-only log level.
- **`WriteAutomaticSnapshot`**: at `height % 197280 == 0`; writes `.dat`, `.hash`; removes `.dat` if hash write fails; prunes old snapshot files.
- **`ConnectBlock`**: attestation validation (returns `false` on failure); then `WriteAutomaticSnapshot` at checkpoints.
- **`GetPruneRange`**: caps prune at `tip - MANDATORY_PRUNE_DEPTH`.
- **`ActivateSnapshot`**: allows replacing older automatic snapshot chainstate when `verify_assumeutxo_hash=false`; accepts `expected_utxo_hash`.
- **`PopulateAndValidateSnapshot`**: rejects snapshot if `expected_utxo_hash` mismatches `HASH_SERIALIZED` after load.

#### `src/node/miner.cpp` ●

- **`CreateNewBlock`**: always adds UTXO attestation via `ComputeBlockUTXOAttestationHash`; **`return nullptr`** on compute failure (logged error).
- **`GetNextWorkRequired` / template bits**: Stoic Awakening min-difficulty when delay > 120 s.

#### `src/node/blockstorage.cpp` ●

- **`PruneBlockFiles`**: mandatory prune to `tip - MANDATORY_PRUNE_DEPTH` regardless of `nPruneTarget`.

#### `src/node/blockmanager_args.cpp` ●

- Forces `PRUNE_TARGET_MANUAL`; ignores user `-prune` GB value.

#### `src/node/chainstate.cpp` ●

- Load path: automatic snapshots without `chainparams.AssumeutxoForHeight` entry; disables background validation on restart.

#### `src/pow.cpp` ●

- `GetNextWorkRequired`, `PermittedDifficultyTransition`: honour `MinDifficultyActivationHeight` on mainnet (not only testnet).

#### `src/kernel/chainparams.cpp` ●

- New genesis, magic bytes, ports, halving, spacing, BIP activation heights.
- `MinDifficultyActivationHeight = 1` (mainnet).
- `m_assumeutxo_data = {}`, `nMinimumChainWork` / `defaultAssumeValid` cleared.
- DNS: `seed.elektron-net.org`.
- `m_headers_sync_params` tuned for shorter chain.
- `nPruneAfterHeight = 197280` (`MANDATORY_PRUNE_DEPTH`, 137 days at 60s blocks).

#### `src/chainparamsseeds.h` ●

- Fixed seeds **cleared** (`vFixedSeeds.clear()` in chainparams); placeholder byte for MSVC; DNS seed `seed.elektron-net.org`.

#### `src/consensus/amount.h` ○●

- Comments: lepton / ELEK naming; `MAX_MONEY` text references Elektron.

#### `src/consensus/params.h` ●

- Uses existing `MinDifficultyActivationHeight` field (set in chainparams).

---

### 5.2 P2P & networking

#### `src/protocol.h` ●

- New net messages: `GETUTXOSNAPSHOT`, `UTXOSNAPSHOT`, `GETSNAPSHOTDATA`, `SNAPSHOTDATA`.
- New service flag: `NODE_SNAPSHOT`.

#### `src/net_processing.h` ●

- Virtual `MaybeRequestSnapshot()` for tests.

#### `src/net_processing.cpp` ●

- `MaybeRequestSnapshot()`: scheduler every 60 s; trigger when `header - tip >= MANDATORY_PRUNE_DEPTH`.
- **`awaiting_snapshot_bootstrap`**: skips historical block download only when sync gap to checkpoint **> `MANDATORY_PRUNE_DEPTH`** or local snapshot exists — does **not** block IBD while history is still within retention.
- Snapshot download: writes `.hash` on `UTXOSNAPSHOT`; completes rename only if sidecar present.
- Snapshot download state machine, stall detection (30 min), chunk requests in `SendMessages`.
- Handlers for four new message types; `GETUTXOSNAPSHOT` only serves checkpoints with valid `.hash`.
- `NODE_NETWORK_LIMITED` peer logic adjusted for pruned network (small chain, snapshot-pending skip block download).
- `MAX_BLOCKS_IN_TRANSIT_PER_PEER` increased 1 → 5 (60 s blocks).

#### `src/node/protocol_version.h` ●

- `PROTOCOL_VERSION = 70017`, `MIN_PEER_PROTO_VERSION = 70017`.

---

### 5.3 Node init & RPC

#### `src/init.cpp` ●

- `-prune` help text: mandatory, ignored.
- Always runs prune incompatibility checks.
- `UpdateLocalSnapshotServices()`, `MaybeActivateAutomaticSnapshot()`.
- Scheduler: snapshot activation + service bit updates.
- On-chain attestation verification via `ExtractCoinbaseUTXOAttestation` when checkpoint block on disk.
- `PopulateAndValidateSnapshot` called with `expected_utxo_hash` from `.hash` sidecar.
- `NODE_SNAPSHOT` set only when serving snapshots with `.hash`.

#### `src/common/init.cpp` ●

- Default `bitcoin.conf` template: Elektron branding, mandatory pruning notice, no `-txindex`.

#### `src/rpc/mining.cpp` ●

- GBT: `coinbase_required_outputs` (witness + UTXO attestation).
- `generate` / `generateblock` / `getblocktemplate`: explicit RPC error if `createNewBlock` fails (attestation error).
- Allows mining with zero peers on fresh network (warning only).

#### `src/rpc/blockchain.cpp` ○

- String references: "Elektron Net address" in RPC help.

#### `src/rpc/*.cpp` (multiple) ○

- `rawtransaction.cpp`, `util.cpp`, `net.cpp`, `node.cpp`, `signmessage.cpp`, `output_script.cpp`, `external_signer.cpp`, `request.cpp` — ELEK / Elektron address strings in help text.

#### `src/policy/feerate.h` / `feerate.cpp` ●○

- `CURRENCY_UNIT = "ELEK"`; fee format `ELEK_KVB`.

---

### 5.4 GUI (Qt)

#### `src/qt/guiconstants.h` ●

- `ELEKTRON_MANDATORY_PRUNE_WINDOW_GB = 10`.

#### `src/qt/intro.cpp` / `intro.h` ●

- Prune UI disabled; `getPruneMiB()` always returns `1` (manual mode).
- **`MeasureDataDirUsageGiB`**: scans `blocks/`, `chainstate/`, `snapshots/`; shows **current on-disk GB**.

#### `src/qt/optionsdialog.cpp` / `.h` ●

- Prune controls disabled; **30 s timer** refreshes measured disk usage.

#### `src/qt/optionsmodel.cpp` ●

- `Prune` always true; `PruneSize` fallback constant; cannot change retention from GUI.

#### `src/qt/bitcoingui.cpp`, `addressbookpage.cpp`, `editaddressdialog.cpp`, etc. ○

- User-visible "Elektron Net" strings.

#### `src/qt/bitcoinunits.h`, `bitcoinamountfield.*` ○

- Elektron copyright; unit enum labels may still say BTC/mBTC internally (display layer).

---

### 5.5 Utilities & binaries

#### `src/util/fs_helpers.h` / `fs_helpers.cpp` ●

- **`DirectorySize()`** for GUI and future tooling.
- Uses `std::filesystem::exists` with `error_code` for MSVC compatibility.

#### `src/clientversion.cpp` ○

- `UA_NAME("Elektron")`.

#### `src/bitcoind.cpp`, `src/bitcoin-cli.cpp` ○

- Help text / branding.

#### `src/common/license_info.cpp` ○

- Adds Elektron Net copyright line.

#### `src/addresstype.h` ○

- Comment strings only; address logic unchanged (Bech32 `be` HRP from chainparams).

---

### 5.6 Wallet ●

#### `src/interfaces/chain.h` / `src/node/interfaces.cpp` ●

- **`forEachCoin()`**: new interface to iterate chain UTXO set (cursor over `CoinsDB()`).

#### `src/wallet/wallet.h` / `wallet.cpp` ●

- **`ScanUTXOSet()`**, **`CreditUTXOFromChain()`**: Pocket recovery when pruned history unavailable.
- **`AttachChain()`**: on `havePruned()` + missing blocks, runs UTXO scan instead of `-reindex` error.
- Warning string: balances recovered; pruned transaction history unavailable.

#### `src/wallet/rpc/` ●○

- `transactions.cpp`, `backup.cpp`: updated prune/rescan error messages (UTXO scan guidance).
- Other RPC files: ELEK / Elektron address strings in help text.

#### `src/qt/bitcoinstrings.cpp` ○

- UTXO scan user-facing strings; removed obsolete `-reindex` prune wallet message.

#### Functional tests ●

- `test/functional/wallet_backup.py`: pruned restore expects UTXO-scan success.
- `test/functional/wallet_assumeutxo.py`: `backup_w2.dat` restores via UTXO scan on pruned node.

---

### 5.7 Tests

#### `src/test/elektron_simulation.cpp` ＋●

- New suite: `MANDATORY_PRUNE_DEPTH`, `PruneAfterHeight()` alignment, attestation validation, snapshot files, hash mismatch rejection, bootstrap math, prune range.

#### `src/test/miner_tests.cpp` ●

- `CreateNewBlock_validity`: adds UTXO attestation to rebuilt coinbase; re-mines PoW.

#### `src/test/peerman_tests.cpp` ●

- `MaybeRequestSnapshot` smoke test; limited-peer behaviour for pruned network.

#### `src/test/blockmanager_tests.cpp` ●

- Assertions on `MANDATORY_PRUNE_DEPTH` prune cap.

#### `src/test/amount_tests.cpp` ○

- Elektron copyright.

---

### 5.8 External mining tools ＋

#### `mining/miner.py` ＋●

- Consumes `coinbase_required_outputs`; documents per-block attestation.

#### `mining/miner.cpp` ＋●

- Builds coinbase from `coinbasevalue` + `coinbase_required_outputs` (witness + UTXO attestation).
- Resolves payout `scriptPubKey` via RPC `validateaddress`.
- Recomputes Merkle root and assembles the full block (same flow as `miner.py`).

#### `mining/mine_genesis.py` ＋●

- Genesis mining for `chainparams.cpp`.

---

## 6. RPC & GBT Surface Changes

### `getblocktemplate` (additions)

```json
"coinbase_required_outputs": [
  { "value": 0, "scriptPubKey": "..." },  // witness commitment (if segwit)
  { "value": 0, "scriptPubKey": "..." }   // OP_RETURN UTXO attestation
]
```

Miners **must** append these outputs (order preserved with witness rules).

**Failure modes (integrators must handle):**

| Condition | Node behaviour |
|-----------|----------------|
| GBT / `createNewBlock` attestation compute fails | RPC error: *"Failed to create new block (UTXO attestation error)"* |
| Submitted block missing attestation | Rejected: `missing-utxo-attestation` |
| Wrong attestation hash | Rejected: `bad-utxo-attestation` |
| Stale GBT template (old hash) | Rejected after tip moved — **fetch fresh GBT every block** |

### Unchanged RPC ports

- Mainnet RPC: **8332**, P2P: **8333** (same as Bitcoin Core defaults).

### New P2P messages (pool / node operators)

Pools do not speak P2P directly, but **full nodes** bootstrap via:

| Message | Direction | Purpose |
|---------|-----------|---------|
| `getutxosnapshot` | request | Ask for checkpoint snapshot metadata |
| `utxosnapshot` | response | Height, hash, UTXO content hash, file size |
| `getsnapshotdata` | request | Request byte range of `.dat` file |
| `snapshotdata` | response | Chunk payload |

Service bit **`NODE_SNAPSHOT` (1<<12)** indicates a peer can serve a verified checkpoint snapshot.

---

## 7. Network Parameters (Mainnet v4.0)

| Parameter | Value |
|-----------|-------|
| Genesis time | `1781164284` |
| Genesis nonce | `8892291` |
| Genesis hash | `00000006b054338443f1a5d5534df21eab0d13232028158ae198edbb169f9dad` |
| Merkle root | `0a7087d81dfb14868848c7e02da8408fe721540e63f6cab9a67d0dfc37b19b17` |
| Message start | `e1 ec 7a 6e` |
| `powLimit` | `007fffff0000...` |
| Bech32 HRP | `be` |
| First snapshot checkpoint | Block **197,280** |

---

## 8. Upgrade & Compatibility Notes

1. **Delete old datadir** before running v4.0 — genesis and magic bytes changed.
2. **Peers < protocol 70017** are disconnected.
3. **Blocks without UTXO attestation** are invalid at any height > 0.
4. **`-prune=<GB>`** has no effect; retention is always 197,280 blocks (~137 days at 60 s).
5. **`-txindex`** cannot be enabled.
6. **No full-node archival mode** — history older than 197,280 blocks is not retained network-wide.
7. For mining / pools, upgrade to software that reads **`coinbase_required_outputs`** (see §10).
8. For wallets on pruned nodes, expect **UTXO-scan recovery** at load — not full transaction history.

---

## 9. Integrator Guide (Pools, Wallets, GBT Software)

This section is the **entry point for external developers**. Detailed pool steps live in [`doc-elektron/mining-pool-integration.md`](mining-pool-integration.md).

### 9.1 Mining pools & Stratum backends — **must change**

| Requirement | Detail |
|-------------|--------|
| GBT field | Read **`coinbase_required_outputs`** on every new block template |
| Coinbase layout | `vout[0]` = payout; append all required outputs **in order** |
| Attestation | Copy `scriptPubKey` from GBT — **do not** compute UTXO hash locally |
| Stratum | Split `coinb1`/`coinb2` so extranonce only touches `scriptSig`; attestation stays in `coinb2` |
| Stoic Awakening | Respect `bits` from GBT (min-difficulty after >120 s since last block) |
| Reference code | [`mining/miner.py`](../mining/miner.py), [`mining/miner.cpp`](../mining/miner.cpp) |

**ASIC firmware:** unchanged (hashes headers only). **Pool software:** must change or all blocks are invalid.

### 9.2 GBT / standalone miners (cgminer, custom) — **must change**

Same as pools: include `coinbase_required_outputs`. Blocks without the per-block `OP_RETURN` attestation are rejected.

In-node mining (`generatetoaddress`, `generateblock`, reference miners) already handles attestation.

### 9.3 Wallet software — **should understand**

| Topic | Elektron behaviour |
|-------|-------------------|
| Balance recovery | Automatic **UTXO-set scan** when wallet load cannot rescan pruned blocks |
| Transaction history | Only ~137 days of blocks exist; older tx details are **not** on the network |
| `rescanblockchain` | Still requires blocks on disk; use wallet reload for UTXO scan |
| AssumeUTxo / snapshot wallets | Background-sync wallets unchanged; pruned-node path uses UTXO scan |
| User messaging | Warn that pruned history is unavailable; balance from current pocket (UTXO set) |

Wallet vendors integrating against `elektrond` RPC need no new RPC methods for UTXO scan — it runs inside `loadwallet` / `restorewallet` / startup.

### 9.4 Full-node / block explorer integrators

- Cannot rely on blocks older than **`tip - 197,280`** being served by pruned peers.
- Checkpoint **snapshots** (`.dat` + `.hash` in `datadir/snapshots/`) replace genesis-to-checkpoint download for IBD.
- **`gettxoutsetinfo`**, **`dumptxoutset`**, **`scantxoutset`** remain valid for UTXO-set queries.
- Block explorers need **their own index** if they require full history — the chain does not retain it.

### 9.5 Quick compatibility checklist

**Pool / miner**

- [ ] Parse `coinbase_required_outputs` from GBT
- [ ] Append witness commitment + UTXO attestation to every coinbase (height > 0)
- [ ] Fresh GBT per block (attestation hash changes every block)
- [ ] Handle Stoic Awakening `bits` from template

**Wallet**

- [ ] Do not assume `-reindex` is required on pruned Elektron nodes
- [ ] Document that old tx history may be unavailable
- [ ] Seed backup remains sufficient for balance recovery

**Node**

- [ ] Protocol version ≥ 70017
- [ ] Mandatory pruning always on
- [ ] Serve snapshot only with `.dat` + `.hash` if advertising `NODE_SNAPSHOT`

---

## 10. Document Map

| Document | Audience | Contents |
|----------|----------|----------|
| [`WHITEPAPER.md`](../WHITEPAPER.md) | Everyone | Protocol philosophy, Pocket model, 137-day pruning |
| [`doc-elektron/BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md) | C++ / Bitcoin devs | **This file** — full implementation diff |
| [`doc-elektron/mining-pool-integration.md`](mining-pool-integration.md) | **Pools, Stratum devs** | GBT fields, coinbase layout, Stratum splitting |
| [`doc-elektron/AUDIT_PRUNING_SNAPSHOT.md`](AUDIT_PRUNING_SNAPSHOT.md) | Auditors | Security paths, scenario analysis |
| [`doc-elektron/hardfork-v3.0.1-stoic-awakening.md`](hardfork-v3.0.1-stoic-awakening.md) | Miners | Stoic Awakening detail |
| [`mining/README.md`](../mining/README.md) | Miners | Reference miner setup |
| [`mining/GENESIS.md`](../mining/GENESIS.md) | Core devs | Genesis constants |

---

*When upstream Bitcoin Core is merged, reconcile this document against new base commits. Search the tree for `Elektron Net:` comment markers and `MANDATORY_PRUNE_DEPTH` references to find drift.*