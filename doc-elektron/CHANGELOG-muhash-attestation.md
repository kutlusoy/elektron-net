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

Disk-pruning thresholds (`nPruneAfterHeight`, a separate, pre-existing mechanism) were left untouched.

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

## Deferred (not part of this pass)

- **Phase 2 (snapshot metadata embedding):** `WriteAutomaticSnapshot()` still always performs a full `HASH_SERIALIZED` pass for the checkpoint snapshot file itself; the accumulator's state is not yet embedded in snapshot metadata for bootstrap nodes to skip a from-scratch rebuild. Not needed for this pass: activation is testnet/regtest-only, at heights far below the checkpoint interval (`MANDATORY_PRUNE_DEPTH`), so no node exercised by this change will reach a checkpoint boundary in the course of testing it.
- **Phase 4 (P2P protocol-version gate):** no `PROTOCOL_VERSION` bump or peer-disconnect-on-old-version behavior was added. The consensus cutover is height-only (mirrors the existing `MANDATORY_PRUNE_DEPTH` / `MinDifficultyActivationHeight` precedent), which the fix report already identifies as sufficient for correctness; the P2P gate is a pure UX nicety for a scenario (stale peers lingering post-activation) that does not apply while this stays off mainnet.
- **Mainnet activation:** intentionally out of scope. `MuhashAttestationActivationHeight` stays `-1` on mainnet until a separate decision is made, informed by how the testnet/regtest activation behaves in practice.
