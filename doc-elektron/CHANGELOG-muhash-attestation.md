# Changelog: MuHash UTXO Attestation (Phase 1 + Activation Gate)

**Status:** Implemented on branch `MuHash`. Active on **testnet/testnet4/regtest only** (low, explicit activation heights); permanently disabled on **mainnet** (`MuhashAttestationActivationHeight = -1`) until a separate, deliberate decision is made to activate it there.
**Related:** `doc-elektron/fix-report-utxo-attestation-scalability.md` (design + rationale), `BITCOIN_CORE_DIFF.md` §2.2 (consensus rule summary).
**Scope of this pass:** Fix-report Phase 1 (incremental accumulator, consensus hooks, persistence, mining path) + Phase 3 (per-network activation height gate). Phase 2 (embedding the accumulator in checkpoint snapshot metadata) and the optional Phase 4 (P2P protocol-version peer gate) are **not** included here — see "Deferred" below.

This file is a dated, chronological log of the changes made in this pass, in the spirit of `BITCOIN_CORE_DIFF.md` (which records the *current-state* diff against upstream Bitcoin Core; this file records *how* the MuHash attestation change was built, one step at a time, so the history stays reviewable after the fact).

---

## 2026-07-01

**New consensus parameter.** `Consensus::Params::MuhashAttestationActivationHeight` (`src/consensus/params.h`), same `-1`-disabled sentinel convention as the existing `MinDifficultyActivationHeight`. Set per network in `src/kernel/chainparams.cpp`:
- Mainnet: `-1` (disabled — explicit, will not change as a side effect of this pass).
- Testnet / Testnet4: `5000` (revised same-day from an initial `50000` placeholder — the network is confirmed still near height 0, so `5000` gives a real testing window without an unnecessarily long wait; **still verify against the live tip before deployment** if time has passed).
- Regtest: `50` (revised same-day from an initial `10` — high enough to leave a visible pre-activation window for manual testing, still fast for CI).

**Checkpoint interval made per-network too.** The existing automatic-UTXO-snapshot / P2P-bootstrap checkpoint interval (previously the single hardcoded global `MANDATORY_PRUNE_DEPTH = 197280` in `src/validation.h`, used directly by `WriteAutomaticSnapshot`, `ValidateUTXOCheckpoint`'s log-level split, `Chainstate::GetPruneRange`, and several `net_processing.cpp` snapshot-bootstrap calculations) is now a new `Consensus::Params::MandatoryPruneDepth` field (default `197280`, matching the still-present `MANDATORY_PRUNE_DEPTH` constant, which now only serves as that default/mainnet reference value — not something requested by the original fix report, but needed so the checkpoint/snapshot cycle can actually be exercised while testing MuHash activation on testnet/regtest, rather than only after 197,280 blocks). All 9 real (non-comment) call sites already had `Consensus::Params`/`ChainstateManager` access in scope, so this was a mechanical read-from-params change, not a redesign. Per network:
- Mainnet: `197280` (unchanged).
- Testnet / Testnet4: `7000`.
- Regtest: `100`.

`nPruneAfterHeight` (`src/kernel/chainparams.cpp`) was initially left untouched, then corrected the same day (see the second dated entry below) to match `MandatoryPruneDepth` per network, mirroring the mainnet principle where the two are already equal (`197280` == `197280`).

**New incremental UTXO MuHash accumulator.** `src/kernel/utxo_muhash.h`: `kernel::UTXOMuHashState`, a thin wrapper around the existing `MuHash3072` type (`src/crypto/muhash.h`) that reuses the existing `ApplyCoinHash`/`RemoveCoinHash` helpers (`src/kernel/coinstats.cpp`) already used by `CoinStatsIndex` — no new per-coin serialization logic was written.

**Persistence.** `src/txdb.h`/`.cpp`: `CCoinsViewDB::WriteUTXOMuHashState`/`ReadUTXOMuHashState`, new DB key `DB_UTXO_MUHASH` (`'U'`). The accumulator is stored together with the chain tip hash it corresponds to, so a stale or missing entry (first run of this feature on an existing chainstate, or a crash between this write and the coins DB flush) is detected on load and triggers a one-time rebuild rather than silently using wrong data.

**Chainstate wiring.** `src/validation.h`/`.cpp`:
- `Chainstate::m_utxo_muhash` + `Chainstate::UTXOMuHash()` accessor, `Chainstate::EnsureUTXOMuHashLoaded()` (lazy load-or-cold-start-rebuild, called once per chainstate).
- `ConnectBlock()`: loads the accumulator at the top of the function (so it represents the state *before* the candidate block, matching what `ComputeBlockUTXOAttestationHash` expects as its base); commits this block's coin changes to it only after the `fJustCheck` early-return *and* after `WriteBlockUndo` succeeds — so speculative/dry-run connects (mining, `TestBlockValidity`) and disk-write failures never mutate the persistent accumulator, only blocks that are actually, successfully connected do.
- `DisconnectBlock()`: applies the exact inverse (`RemoveCoin` for the disconnected block's own outputs, `AddCoin` for restored inputs, read back from the view after `ApplyTxInUndo` rather than trusting the raw undo record, so it always matches what was actually restored).
- `FlushStateToDisk()`: persists the accumulator right after `CoinsTip().Flush()/Sync()`, keyed to the same best-block hash.
- The accumulator tracking itself is **unconditional** (runs on every block regardless of activation height), so it is already fully warmed up by the time the activation height is reached — only *consuming* it for the consensus attestation value is height-gated.

**Consensus switch.** `ComputeBlockUTXOAttestationHash()` (`src/validation.cpp`) now takes the consensus params and a pointer to the current accumulator. Post-activation, it clones the accumulator (cheap — a few hundred bytes) and applies only the candidate block's own coin changes, instead of a full `HASH_SERIALIZED` rescan of the entire UTXO set. Pre-activation (and on mainnet, always) behavior is byte-for-byte unchanged from before this pass.

**Mining path.** `src/node/miner.cpp` `CreateNewBlock()`: passes the new parameters through; post-activation this removes the double full-UTXO-set simulation that previously happened on every mined block (once for the template, once again for validation).

**Tests.**
- Unit (`src/test/elektron_simulation.cpp`): `muhash_accumulator_matches_full_scan` (incremental accumulator == from-scratch full `MUHASH` scan), `muhash_accumulator_survives_reorg` (disconnecting blocks returns the accumulator to the exact prior state). The pre-existing `checkpoint_and_snapshot_mechanism` test was adjusted to compute its expected attestation via the production `ComputeBlockUTXOAttestationHash()` function instead of hardcoding `HASH_SERIALIZED`, so it stays valid regardless of whether regtest's activation height has been reached.
- `src/test/miner_tests.cpp`: updated call site for the new function signature.
- Functional (`test/functional/feature_muhash_attestation_activation.py`, registered in `test_runner.py`): mines across the activation height, forces a reorg that straddles it, restarts the node (accumulator persistence / cold-start), and checks `getblocktemplate`'s `coinbase_required_outputs` keeps its existing 32-byte-hash shape throughout — a practical (not just code-inspection) confirmation that mining pools following the documented GBT contract (`mining-pool-integration.md`, `BITCOIN_CORE_DIFF.md` §9.1) need no changes.

**Miner/pool impact assessment.** Re-verified before implementation: `kutlusoy/elektron-net-pool` contains no code that computes or independently verifies the UTXO attestation hash (confirmed via repository code search for `attestation`/`muhash`/`utxo`/`hash_serialized` — zero matches). Pools only copy the `scriptPubKey` from `getblocktemplate`'s `coinbase_required_outputs`, and that field's shape (32-byte hash, same OP_RETURN encoding) is unchanged by this switch. No changes needed in the pool, pool-ui, or faucet repositories for this pass.

**Documentation.** `doc-elektron/fix-report-utxo-attestation-scalability.md` status field updated; `BITCOIN_CORE_DIFF.md` §2.2 updated to describe the dual-algorithm behavior and the new consensus parameter, "Last updated" bumped.

---

## 2026-07-01 (same day, follow-up: `nPruneAfterHeight` + live pruning verification)

Live-tested this branch end-to-end on a real regtest node (build + run `elektrond`/`elektron-cli`, mine across both boundaries, restart, `pruneblockchain`) rather than relying on unit/functional tests alone. Two findings:

1. **`nPruneAfterHeight` was inconsistent with the new `MandatoryPruneDepth`.** Mainnet already has them equal (`197280` == `197280`, "pruning starts at the first checkpoint" per §2.1). Testnet/testnet4 still had the old `nPruneAfterHeight = 1000` (vs. `MandatoryPruneDepth = 7000`) and regtest had `opts.fastprune ? 100 : 1000` (vs. `MandatoryPruneDepth = 100`) — meaning pruning eligibility wouldn't even be *considered* until height 1000, well past the point the checkpoint/attestation testing is meant to happen around. Fixed to match `MandatoryPruneDepth` on all three networks (testnet/testnet4: `7000`; regtest: fixed `100`, dropping the `fastprune` conditional since both branches now agree).
2. **Confirmed via live test that actual block-*file* deletion additionally requires `-fastprune`.** Height-based eligibility (`GetPruneRange`) is necessary but not sufficient: `FindFilesToPrune` only deletes whole `blk*.dat`/`rev*.dat` files, and a file only rolls over to the next number once it hits `MAX_BLOCKFILE_SIZE` (128 MiB) — `-fastprune` lowers that to 64 KiB (`src/node/blockstorage.cpp`), which is what actually makes multiple block files (and therefore observable pruning) reachable within a short regtest session with realistically tiny empty blocks. Without `-fastprune`, a regtest node would need ~400,000+ blocks before a second file is ever created, regardless of how low `MandatoryPruneDepth`/`nPruneAfterHeight` are. Not a bug — this is pre-existing, correct Bitcoin Core behavior — but worth recording since it wasn't obvious from the code alone and is essential to reproducing a working pruning test.

Verified live with the corrected `nPruneAfterHeight` values: fresh regtest node, `-fastprune`, mined to height 600. `pruneblockchain 500` (and automatic pruning during normal flushes) correctly capped the actual prune height at `tip - MandatoryPruneDepth` (pruned to height ~433, since pruning only removes whole files and the eligible cutoff of 500 didn't land exactly on a file boundary) and **physically deleted** the old `blk00000.dat`/`blk00001.dat`/`rev00000.dat`/`rev00001.dat` file pairs from disk — only the current file (`blk00002.dat`/`rev00002.dat`) remained. `getblockchaininfo` reported `pruneheight: 433` afterwards, confirming the RPC-visible state matches the on-disk reality.

---

## 2026-07-01 (same day, follow-up: faster testnet/testnet4 values for local iteration)

At the user's request: all local testing happens against testnet/testnet4 chainparams (not against the live public testnet), so there is no need for the earlier `5000`/`7000` values, which exist mainly to leave headroom on a network other operators might already be running. Lowered for quicker local turnaround:
- Testnet / testnet4 `MuhashAttestationActivationHeight`: `5000` → `250`.
- Testnet / testnet4 `MandatoryPruneDepth`: `7000` → `300`.
- Testnet / testnet4 `nPruneAfterHeight`: `7000` → `300` (kept aligned with `MandatoryPruneDepth`, same principle as the earlier fix).

Regtest values (`50` / `100` / `100`) and mainnet (`-1` / `197280` / `197280`, all untouched) are unaffected. If these testnet/testnet4 chainparams are ever pointed at the actual public testnet (multiple independent operators, not just local testing), the activation height must be re-verified against the live tip first — a value already passed by the real chain would fork any node still on old software the moment it upgrades.

---

## 2026-07-01 (same day, critical fix: P2P snapshot-bootstrap messages were wire-broken on every network)

**Not part of the MuHash change itself, but found while verifying it end-to-end**, and severe enough to fix immediately per the user's request. Two-node live test: node A mined to height 1100 (`-fastprune`, `MandatoryPruneDepth=100`) and pruned (`pruneheight=865`, matching the user's own independently-reproduced regtest run). Node B connected fresh via real P2P (`-connect`), synced headers to 1100/1100 immediately, but **stayed stuck at `blocks: 0` indefinitely** — the exact symptom the user reported and asked about ("Funktioniert der sync ... nach dem pruning und snapshot wirklich?").

**Root cause**, found with `-debug=net` on both nodes:
```
Node B: [net] sending getutxosnapshot (32 bytes) peer=0
Node A: [net] received: getutxosnaps (32 bytes) peer=0
Node A: [net] Unknown message type "getutxosnaps" from peer=0
```
`NetMsgType::GETUTXOSNAPSHOT` (`src/protocol.h`) was defined as the string `"getutxosnapshot"` — **15 characters**, exceeding `CMessageHeader::MESSAGE_TYPE_SIZE` (12 bytes), a long-standing Bitcoin P2P wire-protocol constant every message type name must fit in (all of Bitcoin's own message names are ≤12 chars: `getheaders`, `sendheaders`, etc.). `NetMsgType::GETSNAPSHOTDATA` (`"getsnapshotdata"`, also 15 chars) had the same problem; `UTXOSNAPSHOT`/`SNAPSHOTDATA` (both exactly 12 chars) were fine.

Two different failure modes depending on transport, both traced in the source:
- **V2 (BIP324) transport — silent corruption, not just truncation.** `V2Transport::SetMessageToSend` (`src/net.cpp:1504-1506`) allocates a fixed 12-byte slot for the type string and `std::copy`s the full type string into it with **no length check** — for a 15-character name this overflows into the payload's first 3 bytes. The receiver reads back a mangled 12-byte type (`"getutxosnaps"`), fails to match any known handler, and silently drops the message. This is what happened in the live test (peer connected with `transport: v2`).
- **V1 (legacy) transport — hard crash.** `CMessageHeader::CMessageHeader` (`src/protocol.cpp:10-19`) has `assert(msg_type[i] == 0)` after copying up to `MESSAGE_TYPE_SIZE` bytes — a >12-char message type triggers an **assertion failure**, i.e. any node that ever tried to send `GETUTXOSNAPSHOT`/`GETSNAPSHOTDATA` over a V1 connection would have crashed outright.

Net effect: **the entire P2P UTXO-snapshot bootstrap mechanism (§2.3 of `BITCOIN_CORE_DIFF.md`) has never worked, on any network** (mainnet/testnet/testnet4/regtest — this is a wire-protocol constant, not chainparams-dependent) — a pruned node has had no way to serve a fresh peer past its prune point since this feature was written, and any V1 peer that tried would crash.

**Fix** (`src/protocol.h`): shortened both names to fit the 12-byte limit — `GETUTXOSNAPSHOT`: `"getutxosnapshot"` → `"getutxosnap"` (11 chars), `GETSNAPSHOTDATA`: `"getsnapshotdata"` → `"getsnapdata"` (11 chars). No other code changes needed: all call sites reference the `NetMsgType::` C++ constants, not the raw strings, so this is a self-contained, minimal fix. `UTXOSNAPSHOT`/`SNAPSHOTDATA` left unchanged (already valid).

**Re-verified live with the fix**, same two-node setup (node A pruned past height 865, tip 1100; fresh node B connecting via P2P) — the message now arrived intact, but this uncovered five more, previously entirely untested, bugs in the same P2P snapshot-bootstrap path (§2.3 `BITCOIN_CORE_DIFF.md`). Each was found, fixed, and re-verified live before moving to the next, per the user's "sofort beheben und protokollieren" instruction. All five are below, in the order they were hit.

### Bug 2: `GETUTXOSNAPSHOT` handler used `cs_main`-protected data without holding `cs_main`

After the bug-1 fix, node A crashed processing node B's (now correctly-named) request:
```
Assertion failed: lock cs_main not held in ./node/blockstorage.cpp:213
```
The `GETUTXOSNAPSHOT` handler in `src/net_processing.cpp` called `m_chainman.m_blockman.LookupBlockIndex(checkpoint_hash)` with no lock held. Fixed by wrapping the lookup in a scoped `LOCK(cs_main)` block, extracting just the two booleans/height needed (`checkpoint_height`, `is_checkpoint`) before releasing the lock — the same pattern already used by other message handlers in the same file (e.g. `GETBLOCKS`).

### Bug 3: `SNAPSHOTDATA` receiver left an `AutoFile` open across a scope its destructor can't tolerate

After the bug-2 fix, node B (the receiver of snapshot chunks) crashed:
```
./streams.h:401 AutoFile::~AutoFile(): Assertion 'IsNull()' failed.
```
`AutoFile`'s destructor asserts the file was explicitly closed if anything was written to it (`m_was_written`). Of the four `AutoFile` usages in `net_processing.cpp`'s snapshot code, only the one in the `SNAPSHOTDATA` handler (writing a downloaded chunk to the temp file) was missing an explicit `.fclose()` before the object went out of scope and before the subsequent `fs::rename()` of the completed file. Fixed by adding the `.fclose()` call (with a warning log if it fails) right after the write, before checking `AddRange`/renaming.

### Bug 4: `MaybeActivateAutomaticSnapshot` also used `cs_main`-protected data without holding `cs_main`

After the bug-3 fix, node B crashed again with the *same* assertion message as bug 2 (`lock cs_main not held ... blockstorage.cpp:213`), but from a different call site — the plain assertion text doesn't include a backtrace, so this needed `gdb -batch -x cmds.txt ./bin/elektrond` (`run <args>`; `bt`; `thread apply all bt`; `quit`) to identify. The backtrace showed `MaybeActivateAutomaticSnapshot` (`src/init.cpp`), called from a `CScheduler::Repeat` wrapper — i.e. this function runs every 30 seconds for as long as the node is in IBD, not just once at startup as an earlier grep-only read had assumed. It had the same unlocked `LookupBlockIndex` call as bug 2. Fixed the same way: `WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(...))`.

### Bug 5: automatic snapshot activation was not idempotent — retried every 30s, crashing on the second attempt

After the bug-4 fix, node B crashed a third time, differently:
```
Fatal LevelDB error: IO error: lock /tmp/node-b/regtest/chainstate_snapshot/LOCK: already held by process
```
`MaybeActivateAutomaticSnapshot` runs every 30 seconds while `IsInitialBlockDownload()` is true, which it can remain for a while even *after* a successful activation. The function had no guard against re-activating an already-activated snapshot, so the very next scheduled run tried to open a second LevelDB handle on the same `chainstate_snapshot/` directory the first (still-open) activation already held.

Two fix attempts were tried and **both failed live** before landing on the working one:
1. Checking `chainman.CurrentChainstate()->m_from_snapshot_blockhash` before attempting activation — rebuilt, retested, crashed identically on the next 30s cycle (the debug.log still showed a second "attempting activation" line).
2. Checking `chainman.ActiveChain().Tip()`'s height/hash against the snapshot filename — same result, still crashed.

Both approaches relied on introspecting `ChainstateManager`'s current chainstate selection, which (as bug 6 below makes clear in hindsight) was not behaving as expected at this point in the investigation. The working fix instead avoids chainstate introspection entirely: a function-local `static uint256 s_activated_snapshot_hash` (safe because this function has exactly one production call site and is never invoked from unit tests) is checked against the hash parsed out of the snapshot filename (`<height>-<hash>.dat`) before attempting activation, and set immediately after `ActivateSnapshot()` succeeds. Rebuilt, retested — held stable through 3+ scheduler cycles (~180s) with no re-activation attempt and no crash.

### Bug 6: successful activation still left the node reporting `blocks: 0` forever

With bugs 1–5 fixed, node B no longer crashed, and the snapshot activated exactly once — but `getblockchaininfo`/`getblockcount` stayed stuck at `blocks: 0`, `initialblockdownload: true`, `bestblockhash` = genesis, indefinitely, even though the debug.log clearly showed `"[snapshot] Successfully activated snapshot at height 1100"`. This is very likely the actual mechanism behind the user's original bug report (a second GUI peer "stuck, not syncing" after pruning/snapshots) — the message-truncation bug (bug 1) meant that scenario never even got this far before, so this bug was previously unreachable and untested.

First hypothesis (wrong, tried and reverted): that `ActivateSnapshot()` never called `ActivateBestChain()` on the new chainstate, so its tip was never actually moved to the snapshot block. Adding an explicit `chainman.ActivateBestChains()` call after activation was rebuilt and retested — **no change**, still stuck at `blocks: 0`. This ruled out that theory: `PopulateAndValidateSnapshot()` already calls `snapshot_chainstate.m_chain.SetTip(*snapshot_start_block)` directly, so the snapshot chainstate's own tip was correct all along.

**Actual root cause:** `MaybeActivateAutomaticSnapshot` (`src/init.cpp`), after a successful activation, called `chainman.HistoricalChainstate()->SetTargetBlock(nullptr)` to abandon the now-useless old (pre-snapshot) IBD chainstate — background validation can never complete for it since the historical blocks it would need are pruned on every peer. But `ChainstateManager::CurrentChainstate()` (`src/validation.h`) selects the *first* chainstate in `m_chainstates` that has no `m_target_blockhash` — and `m_chainstates` is ordered `[old IBD chainstate, new snapshot chainstate]` (`AddChainstate()` appends the new one to the end). Clearing the IBD chainstate's target left *both* chainstates target-less, so `CurrentChainstate()` — and therefore `ActiveChainstate()`, and therefore every RPC that reports chain height — kept resolving to the old, still-at-genesis IBD chainstate instead of the snapshot chainstate, forever.

Fix: instead of merely clearing the old chainstate's target, delete it outright via `chainman.DeleteChainstate()` — the same cleanup already used a few lines earlier in `ChainstateManager::ActivateSnapshot()` when replacing an old snapshot with a newer one (`src/validation.cpp` ~line 6003). Deleting it removes the ambiguity completely: only the snapshot chainstate remains, so `CurrentChainstate()` resolves to it unambiguously.

This surfaced a second, latent bug in `ChainstateManager::DeleteChainstate()` itself (`src/validation.cpp` ~line 6663): it unconditionally dereferenced `prev_chainstate->m_mempool->size()`, assuming the chainstate being deleted currently owns the mempool. That assumption holds when deleting an old *snapshot* chainstate (which does hold the swapped-in mempool at that point) but not for the old IBD chainstate here, whose mempool was already swapped away to the new snapshot chainstate inside `AddChainstate()` during activation — leaving it `nullptr`. In this Debug build (asserts active), this would have been a null-pointer dereference. Fixed by only performing the mempool hand-off when `prev_chainstate->m_mempool` is non-null; the pre-existing (snapshot-replacement) call site is unaffected since its mempool is always present.

**Final live verification** (all six fixes together): fresh two-node regtest run, node A at height 1100 (pruned to 865), node B connecting cold via real P2P. Node B: synced headers instantly, requested and downloaded the snapshot, activated it in a single attempt (`grep -c "attempting activation" debug.log` → `1`), logged `"[snapshot] Discarded old pre-snapshot chainstate"`, and `getblockchaininfo` correctly reported `"blocks": 1100` with `bestblockhash` identical to node A's tip. No crashes, no warnings, no errors in either node's `debug.log` or stderr across the full test.

---

## 2026-07-02 (follow-up: three more bugs found via user-reported Windows crash)

The user ran a fresh Windows GUI node (`elektron-qt.exe`) against this branch and hit two crashes back to back, reported via screenshots: an assertion failure (`validation.cpp:5656`, inside `ChainstateManager::CheckBlockIndex()`) after an automatic snapshot activated and new blocks kept arriving, followed by a restart producing a clean-looking but fatal error dialog: *"Für den angegebenen Blockhash '...' wurden keine Assumeutxo-Daten gefunden"* ("No assumeutxo data found for the given blockhash"). Reproduced and fixed live on Linux, per the user's "sofort beheben und protokollieren" instruction — this uncovered a further two bugs (8 and 9) while verifying the fix for the first.

### Bug 7: `m_chain_tx_count` was never set for automatic/dynamic snapshots, because they have no static chainparams entry

Root cause, common to both of the user's crashes: real (upstream) assumeutxo snapshots have a hardcoded `AssumeutxoData` entry per snapshot height in chainparams (`GetParams().AssumeutxoForHeight()`/`AssumeutxoForBlockhash()`). Elektron's automatic snapshots are *dynamic* — their heights are computed at runtime from `Consensus::Params::MandatoryPruneDepth`, never hardcoded — so that lookup always returns nothing (`au_data` is always `std::nullopt`) for one of ours. Two places assumed it would always be present:

- `ChainstateManager::PopulateAndValidateSnapshot()` (`src/validation.cpp`) only set `index->m_chain_tx_count` (the snapshot base block's transaction-count-from-genesis) `if (au_data)`. Left at its default `0`, `CBlockIndex::HaveNumChainTxs()` (`m_chain_tx_count != 0`) is `false` for the snapshot base — which violates an invariant `ChainstateManager::CheckBlockIndex()` asserts on every call: `(pindexFirstNeverProcessed == nullptr || pindex == snap_base) == pindex->HaveNumChainTxs()`. Regtest defaults `-checkblockindex` to run on *every* validation operation, and `CheckBlockIndex()` re-runs on every new header accepted — so the very next block header to arrive after activation reliably crashed the node. This is exactly the user's first screenshot.
- `BlockManager::LoadBlockIndex()` (`src/node/blockstorage.cpp`), called on every startup when a snapshot chainstate exists on disk, treated a missing `au_data` as fatal and refused to start at all: *"Assumeutxo data not found for the given blockhash"*. Since `au_data` is *always* missing for our automatic snapshots, this meant **every single restart of a node that had ever activated one** hit this fatal error — not an edge case. This is the user's second screenshot.

Fix: both call sites now branch on whether `au_data` is present. When it is (a real, static assumeutxo snapshot), behavior is unchanged. When it isn't (one of ours), `m_snapshot_height` is derived directly from the already-loaded block index (no static table needed for that), and `m_chain_tx_count` is set to a `base_height + 1` lower-bound estimate (one tx per block) — this value only feeds the `verificationprogress` display, never consensus validity, and the true historical count is unknowable at a pruned/snapshot-bootstrapped node anyway.

### Bug 8: automatic snapshots were silently captured one block too early (off-by-one), corrupting checkpoint data after MuHash activation

Found while re-verifying bug 7's fix with continued mining past a checkpoint (needed to actually re-trigger `CheckBlockIndex()` post-activation). Once bug 7 no longer crashed the node, mining 20 more blocks on the source node made the bootstrapped node reject the very next block after the checkpoint:
```
ConnectTip: ConnectBlock ... failed, bad-utxo-attestation, UTXO attestation mismatch at height 1101: expected 20505b7d..., got 792abd9f...
```
Root cause: `WriteAutomaticSnapshot()` used to be called from inside `Chainstate::ConnectBlock()`, which runs *before* `ConnectTip()`'s `view.Flush()` merges the connecting block's own coin changes out of the per-block `m_connect_block_view` cache and into `CoinsTip()`. `WriteAutomaticSnapshot()` reads the checkpoint's coin data from `CoinsDB()`, reached via its own internal `ForceFlushStateToDisk()` (which flushes `CoinsTip()` → `CoinsDB()`) — at that point in `ConnectBlock()`, `CoinsTip()` still only reflected the *parent* block's state, so the checkpoint block's own coinbase output was silently missing from every snapshot file. Confirmed directly from node A's own logs: before the fix, the height-1000 checkpoint reported `coins=999`; the height-1100 checkpoint reported `coins=1099`. After the fix, height-1100 correctly reports `coins=1100`.

Every bootstrapping peer rebuilds its MuHash accumulator from a cold start via a full scan of the received snapshot coins (`Chainstate::EnsureUTXOMuHashLoaded()`) — a mathematically exact substitute for the incrementally-tracked value *only if the underlying coin set is actually complete*. A snapshot missing one coin produces an accumulator that's wrong by exactly that coin, which is invisible until the very next block's attestation is checked against it. This is likely the deeper reason behind the user's original report of a peer stuck "not syncing" — the message-truncation bug (bug 1, see above) meant this interaction between the checkpoint mechanism and MuHash activation had never actually been exercised until this session's activation-height tuning made both reachable together in regtest. It also sharpens the "Phase 2 deferred" note from the original fix report below: the gap wasn't just that the accumulator state isn't embedded in snapshot metadata for a faster bootstrap — the underlying *coin data itself* was silently wrong at every checkpoint once MuHash activation and the checkpoint interval overlapped.

Fix: moved the `WriteAutomaticSnapshot(...)` call from inside `ConnectBlock()` to `Chainstate::ConnectTip()`, placed right after `view.Flush()` (using `pindexNew`) so `CoinsTip()` — and therefore what `WriteAutomaticSnapshot()`'s own flush pushes into `CoinsDB()` — already includes the checkpoint block's own changes. `src/validation.cpp` only.

### Bug 9: deleting the old chainstate (bug 6's fix) broke `ChainstateManager::ValidatedChainstate()`, which unconditionally requires one to exist

Found while re-verifying bug 7+8's fixes across a node restart (the exact scenario from the user's second screenshot). The node no longer hit the "Assumeutxo data not found" fatal error, but crashed differently, deeper into startup:
```
Thread "b-initload" received signal SIGABRT
#5 ChainstateManager::ValidatedChainstate() at validation.h:1215
#6 StartIndexBackgroundSync() at init.cpp:2631
```
`ValidatedChainstate()` (`src/validation.h`) — used by `StartIndexBackgroundSync()` (`init.cpp`) and `BaseIndex::Init()` (`index/base.cpp`) — loops over `{CurrentChainstate(), HistoricalChainstate()}` looking for one with `m_assumeutxo == Assumeutxo::VALIDATED`, and calls `abort()` if it finds none. Normally that's always the primary (non-snapshot) chainstate, since `VALIDATED` is every `Chainstate`'s default value. Bug 6's fix for the `CurrentChainstate()` ambiguity (see above) deletes that exact chainstate once an automatic snapshot activates — which is correct for `CurrentChainstate()`, but leaves *nothing* `VALIDATED` behind, since the surviving snapshot chainstate defaults to `UNVALIDATED`. This is only reachable at startup (both call sites run once, during init), which is why it wasn't caught verifying bug 6 at runtime — indexes are already initialized by the time a live snapshot activation happens in the same process; it only bites on the *next* restart, exactly matching the user's second screenshot.

Fix: once the old chainstate is deleted because there's no static assumeutxo data for it (i.e., an Elektron automatic snapshot), explicitly mark the surviving snapshot chainstate `m_assumeutxo = Assumeutxo::VALIDATED`. This is a deliberate, narrow exception to upstream's assumeutxo trust model, justified by a difference in how the two mechanisms establish trust: real assumeutxo snapshots are trusted *only* because their hash matches a hardcoded chainparams value, and remain `UNVALIDATED` until independent background validation catches up. Elektron's automatic snapshots are cross-checked against the receiving node's own on-chain MuHash coinbase attestation *before* activation (see `MaybeActivateAutomaticSnapshot()`, `init.cpp`) — they are already independently confirmed valid by this node, not merely assumed so, so treating them as `VALIDATED` immediately is accurate, not a shortcut. Applied at both the code path that first triggers the chainstate deletion (`node/chainstate.cpp`'s `LoadChainstate()`, the startup case that actually crashed) and the equivalent live-activation path (`init.cpp`'s `MaybeActivateAutomaticSnapshot()`, for consistency in case indexes are ever started dynamically later in the same run).

**Final live verification** (bugs 7–9 together): node A mined to height 1100 (checkpoint) then pruned; node B bootstrapped via P2P/automatic snapshot (single activation attempt, no crash); node A mined 20 more blocks — node B followed to height 1120 with **no** `bad-utxo-attestation` and **no** crash; node B killed and restarted against the same on-disk (already-activated) state — came back up cleanly at height 1120 with no fatal error, no assertion failure; node A mined 5 more blocks — node B, post-restart, followed to height 1125 without issue. Full unit test suite re-run (`elektron_simulation_tests`): only the already-known, unrelated `prune_depth_calculation` failure remains.

---

## Deferred (not part of this pass)

- **Phase 2 (snapshot metadata embedding):** `WriteAutomaticSnapshot()` still always performs a full `HASH_SERIALIZED` pass for the checkpoint snapshot file itself; the accumulator's state is not yet embedded in snapshot metadata for bootstrap nodes to skip a from-scratch rebuild. Not needed for this pass: activation is testnet/regtest-only, at heights far below the checkpoint interval (`MANDATORY_PRUNE_DEPTH`), so no node exercised by this change will reach a checkpoint boundary in the course of testing it.
- **Phase 4 (P2P protocol-version gate):** no `PROTOCOL_VERSION` bump or peer-disconnect-on-old-version behavior was added. The consensus cutover is height-only (mirrors the existing `MANDATORY_PRUNE_DEPTH` / `MinDifficultyActivationHeight` precedent), which the fix report already identifies as sufficient for correctness; the P2P gate is a pure UX nicety for a scenario (stale peers lingering post-activation) that does not apply while this stays off mainnet.
- **Mainnet activation:** intentionally out of scope. `MuhashAttestationActivationHeight` stays `-1` on mainnet until a separate decision is made, informed by how the testnet/regtest activation behaves in practice.
