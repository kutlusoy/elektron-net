# Fix Report: Per-Block UTXO Attestation Cost Scales With UTXO Set Size

**Status:** Confirmed via code review. Complexity re-assessed and re-verified directly against this codebase (see §7).
**Severity:** Not urgent to *activate*, but the implementation itself should start now; see §1.1 for concrete cost/threshold data showing this becomes a hard chain-halt problem well below Bitcoin's current scale.
**Affected files:** `src/validation.cpp`, `src/node/miner.cpp`, `src/kernel/coinstats.h`, `src/kernel/coinstats.cpp`, `src/index/coinstatsindex.cpp` (reference implementation), `src/crypto/muhash.h`
**Related:** `doc-elektron/AUDIT_PRUNING_SNAPSHOT.md`, `BITCOIN_CORE_DIFF.md` §2.2, §2.3, §9.4, `doc-elektron/fix-report-snapshot-bootstrap-trust.md`

---

## 1. Problem

`ValidateUTXOCheckpoint` runs on **every block**, not just at checkpoint heights, and calls `ComputeBlockUTXOAttestationHash`, which internally uses:

```cpp
const auto stats = kernel::ComputeUTXOStats(
    kernel::CoinStatsHashType::HASH_SERIALIZED,
    &view, blockman);
```

`HASH_SERIALIZED` is a full-set hash: it requires iterating and hashing the entire UTXO set from scratch, not an incrementally-updatable commitment. In upstream Bitcoin Core, this hash type is only computed on demand via the `gettxoutsetinfo` RPC — it is never part of consensus-critical per-block validation, precisely because of this cost profile.

In Elektron Net, this same full-set computation happens as a **consensus-critical step on every block**, i.e. every 60 seconds, versus an equivalent cadence of every 10 minutes if the same call existed in Bitcoin. The cost of this step grows with the size of the UTXO set, not with block content — per-block validation cost increases over time independent of transaction volume, purely as the UTXO set grows.

Confirmed directly in the current codebase (`src/validation.cpp`):
- `ComputeBlockUTXOAttestationHash()` — simulates connecting the block on a throwaway `CCoinsViewCache`, then calls the full-set `HASH_SERIALIZED` scan.
- `ValidateUTXOCheckpoint()` — calls the above on every block with `nHeight > 0`, and rejects the block (`bad-utxo-attestation`) on mismatch.
- `src/node/miner.cpp` — `CreateNewBlock()` calls the same function while building a block template, so the full-set scan currently happens **twice per block**: once during mining, once during validation.

### 1.1 Concrete Cost Estimates (based on real Bitcoin Core benchmarks)

Reference points for `gettxoutsetinfo` (default `hash_serialized_2`, no index — the equivalent of what Elektron Net does per block) on real Bitcoin nodes:

| Hardware | UTXO count | Measured time |
|---|---|---|
| Odroid HC2 (~Raspberry Pi 4 class) | ~68.7M | 7 min 50 sec |
| Typical modern node (various reports) | ~173M (current) | "up to 10 minutes" / "several minutes even on high-powered hardware" |

Derived throughput: **~150,000 UTXOs/sec** (weak hardware), **~500,000–700,000 UTXOs/sec** (solid server hardware, estimated).

**The threshold that actually matters** — the UTXO count at which per-block attestation alone exceeds the 60-second block interval:

```
Threshold = 60s × throughput
Weak hardware:  60 × 150,000  ≈  9 million UTXOs
Solid hardware: 60 × 650,000  ≈ 39 million UTXOs
```

Past this point the node cannot keep pace with the chain by definition — every block takes longer to validate than the interval between blocks, and the backlog grows without bound. This is roughly **20–75× smaller** than Bitcoin's current UTXO set (173M), meaning the problem doesn't require Elektron Net to ever reach Bitcoin's scale to become binding.

For context: Bitcoin's own UTXO set crossed the ~9–39M range roughly 5–8 years after genesis (a 2017 academic analysis recorded 52.5M UTXOs at that point; earlier figures aren't precisely sourced here). Elektron Net's 10× block frequency means that under comparable per-block transaction density, the same threshold could be reached roughly an order of magnitude faster in calendar time if adoption ever approaches that density — though at today's actual usage, the threshold is nowhere close and there is no imminent deadline from usage alone.

**Illustrative full-scenario estimates** (extrapolated from the throughput figures above, not directly measured):

| Scenario | UTXO count | Weak HW | Solid HW |
|---|---|---|---|
| Elektron Net today | ~thousands | < 10 ms | < 5 ms |
| Matches Bitcoin's total historical adoption | ~173M | ~19 min | ~4–5 min |
| Matches Bitcoin's per-block density at 10x frequency | ~1.7B | ~3.2 hours | ~40–45 min |

Both non-trivial scenarios are already far past the 9–39M point where the chain simply stalls — this isn't a "gets slower" problem, it's a "stops working" problem once crossed.

### 1.2 What the fix actually buys: real incremental-hashing evidence

Bitcoin Core already runs an optional `CoinStatsIndex` that maintains a MuHash accumulator incrementally as blocks connect, rather than recomputing it from the full set on demand. A real measurement against that index:

```
$ time bitcoin-cli gettxoutsetinfo muhash
{ "txouts": 83747992, ... }
0m00.02s real
```

**83.7 million UTXOs, 0.02 seconds** — because the value was already incrementally maintained, not recomputed. This is the concrete proof that incremental hashing removes the scaling problem rather than just slowing its onset: cost stops depending on total UTXO set size and depends only on the coins touched in the block being processed (bounded by max block weight, same bound Bitcoin already has). The speedup versus the full-scan numbers above is on the order of **3,000×–100,000×**, growing larger as the UTXO set grows, since the full-scan cost keeps climbing while the incremental cost stays flat.

*Caveat:* the 0.02s figure is an index **read** of an already-maintained value, not a measurement of the per-block **write/update** cost during `ConnectBlock`. That update cost (per-coin group multiplication in the MuHash accumulator) is expected to be in the low-millisecond range even for full blocks based on the underlying operation's known cheapness, but this needs to be benchmarked directly against Elektron Net's actual `ConnectBlock` path — see §6.

## 2. Proposed Fix — Conceptual

Switch from `HASH_SERIALIZED` (full re-scan every block) to an **incrementally-maintained MuHash accumulator** that is updated in place as coins are added/removed, rather than recomputed from scratch. See §7 for a codebase-verified assessment of what this requires in practice.

## 3. Implementation Plan

**Phase 1 — Core accumulator (highest priority, start immediately):**

1. Add a persistent MuHash3072 accumulator to chainstate, analogous in spirit to `CCoinsViewCache` but tracking only the running multiset commitment, not full coin data:
   ```cpp
   // src/kernel/coinstats.h or a new src/kernel/utxo_muhash.h
   // Elektron Net: persistent, incrementally-maintained UTXO set commitment.
   class UTXOMuHashState {
       MuHash3072 m_muhash;
   public:
       void AddCoin(const COutPoint& outpoint, const Coin& coin);    // Insert()
       void RemoveCoin(const COutPoint& outpoint, const Coin& coin); // Remove()
       uint256 GetHash() const;
       // Serialize/deserialize for persistence alongside chainstate.
   };
   ```
2. Hook into the existing coin add/remove path so this stays in lockstep with the chainstate automatically rather than needing a second pass:
   - In `UpdateCoins` (called from `ConnectBlock` for every transaction): call `AddCoin`/`RemoveCoin` on the same additions/removals already being applied to `CCoinsViewCache`.
   - In `DisconnectBlock` / `ApplyTxInUndo`: call the inverse operations using the existing undo data (`CTxUndo`) — MuHash's `Remove()` is the exact inverse of `Insert()`, so reorgs unwind cleanly using data that's already being tracked for other purposes.
3. Persist `UTXOMuHashState` to disk as part of the normal chainstate flush (`FlushStateToDisk`), so it survives restarts without needing to be rebuilt.
4. Change `ComputeBlockUTXOAttestationHash` to, post-activation, return `m_utxo_muhash.GetHash()` directly instead of calling `kernel::ComputeUTXOStats(HASH_SERIALIZED, ...)`. No per-block full-set pass remains once this path is active.

**Phase 2 — Bootstrap and snapshot integration:**

5. A node bootstrapping from a checkpoint snapshot doesn't have the incremental history needed to have built up the accumulator itself — it needs the accumulator's state *as of that checkpoint* handed to it directly, not rederived block-by-block. Since `WriteAutomaticSnapshot` already performs one full `ComputeUTXOStats` pass at each checkpoint (for the existing `.hash` sidecar), compute the MuHash accumulator's state in that same pass and store it in the snapshot metadata. A bootstrapping node then loads this stored accumulator state alongside the snapshot content and continues incrementally forward from there — it never needs to run a full-set pass itself.
6. This means the only full-set passes that ever happen, before or after this fix, are the ones already happening at checkpoint heights for snapshot generation — nothing new is added, and the per-block cost problem is fully eliminated between checkpoints.

**Phase 3 — Activation gating:**

7. Gate the switch behind an activation height, per the deployment strategy below (§4) — `HASH_SERIALIZED` remains the validated algorithm before the height, `HASH_MUHASH` (via the incremental accumulator) after it.
8. Note the accumulator can be built up in the background *before* the activation height even on already-running nodes (it costs nothing extra once Phase 1 is deployed — it's just tracked from whatever height the new software starts running), so by the time the activation height arrives, nodes that updated any time before it already have a fully warmed-up accumulator ready to go, no catch-up work needed at the transition itself.

## 4. Deployment Strategy: Ship Now, Activate at a Future Height

This is a **consensus rule change** (the attestation algorithm is part of what every node must agree on to validate blocks) — a node running the old algorithm past the activation point computes a different attestation than the chain expects and rejects every subsequent block (`bad-utxo-attestation`). Unlike the soft-fork deployments already in the codebase (BIP34/65/66/CSV/Segwit, all `DeploymentActiveAt`-gated), old software doesn't just validate more loosely here — it forks off entirely. Treat this with hard-fork-level care, not as a silent patch.

```cpp
// src/kernel/chainparams.cpp (or validation.h, alongside MANDATORY_PRUNE_DEPTH)
// Elektron Net: MuHash attestation activation height — see §8 for the revised
// (earlier) recommendation vs. the originally proposed 5x MANDATORY_PRUNE_DEPTH.
static constexpr int MUHASH_ATTESTATION_ACTIVATION_HEIGHT = /* see §8 */;
```

```cpp
// src/validation.cpp — ComputeBlockUTXOAttestationHash / ValidateUTXOCheckpoint
if (nHeight >= MUHASH_ATTESTATION_ACTIVATION_HEIGHT) {
    return g_utxo_muhash_state.GetHash(); // incremental, Phase 1
}
const auto stats = kernel::ComputeUTXOStats(kernel::CoinStatsHashType::HASH_SERIALIZED, &view, blockman);
```

**Core recommendation: height-gate only, no protocol version bump required.** The consensus cutover is fully determined by `nHeight` in `ComputeBlockUTXOAttestationHash` (code above) — a node's advertised protocol version has no effect on which algorithm it validates with. Unlike Stoic Awakening (a mining/difficulty *behavior* change, where advertised version meaningfully signals what a peer expects from the network), this is a hard-fork-style cutover where old software simply starts rejecting blocks at the activation height regardless of what version it claims. A version bump adds a codepath, a chainparams field, and a test case for a benefit that's purely cosmetic (see below) — for the first implementation, skip it. This also keeps the change minimal and closer to the existing `MANDATORY_PRUNE_DEPTH` precedent, which is height-only.

**Optional Phase 4 (defer, revisit only if it becomes useful in practice): protocol version gate for a cleaner peer-disconnect experience.** `net_processing.cpp` already has a precedent for this shape of gate — the `post_fork`/`MinDifficultyActivationHeight` check in the `VERSION` handler for Stoic Awakening. The same pattern *could* be mirrored for MuHash (bump `PROTOCOL_VERSION` to 70018, add a `MIN_PEER_PROTO_VERSION_MUHASH` check gated on `MuhashAttestationActivationHeight`) so that outdated peers get disconnected cleanly at the handshake once the height is reached, instead of lingering as connections that are about to become useless. This is purely a P2P-hygiene nicety — it doesn't change correctness, doesn't gate the consensus rule itself, and can be added later, close to the actual activation height, if outdated peers turn out to be a real operational annoyance at that time. Not recommended as part of the initial fix.

**Impact on mining pools / integrators: none, provided the existing GBT contract is followed.** Per `mining-pool-integration.md` (§9.1 of `BITCOIN_CORE_DIFF.md`), pools are already required to copy the attestation `scriptPubKey` from `getblocktemplate`'s `coinbase_required_outputs` rather than compute the UTXO hash themselves — the computation is entirely node-side (`CreateNewBlock` → `ComputeBlockUTXOAttestationHash`). Since `HASH_MUHASH` produces the same 32-byte digest format as `HASH_SERIALIZED`, the GBT field shape, the `coinb1`/`coinb2` Stratum split, and `ExtractCoinbaseUTXOAttestation`'s 32-byte parsing all remain unchanged — pools following the documented contract need to do nothing. The only exposure is a pool that, contrary to the documented contract, independently recomputes or verifies the attestation locally; worth a one-line reminder to integrators ahead of the activation height, not a required code change on their end.

## 5. Testing Recommendations

- **Correctness:** unit tests comparing the incremental accumulator's result against a from-scratch `HASH_MUHASH` full computation on small synthetic sets, across many random add/remove sequences, to confirm bit-for-bit equivalence regardless of history.
- **Reorg correctness:** explicit tests that disconnect and reconnect blocks (including multi-block reorgs) and confirm the accumulator returns to and matches the exact prior state — this is the highest-risk correctness area, since it relies on `Remove()` being a true inverse of `Insert()` across the actual undo-data code path, not just in isolation.
- **Performance:** benchmark the real `ConnectBlock`/`UpdateCoins` path with the accumulator hooked in, at increasing synthetic UTXO set sizes (100k, 1M, 10M, 100M entries) and increasing per-block transaction counts (empty block up to a full block), to get real per-block update cost numbers rather than the extrapolated estimates in §1.1 — and to confirm the update cost stays flat with respect to total UTXO set size as expected.
- **Snapshot integration:** confirm a node bootstrapping from a checkpoint snapshot that includes the stored accumulator state produces identical results to a node that synced from genesis and built the accumulator incrementally the whole way.
- **Dormant-code / activation:** test the transition explicitly on regtest/testnet with an artificially low activation height (e.g. height 100) as a standing part of the test suite — this keeps the switchover logic continuously exercised in CI rather than only becoming a going concern on the day it activates for real.
- **Functional test across the activation boundary:** mine past a test-configured activation height and confirm blocks before it validate under `HASH_SERIALIZED`, blocks at/after it validate under the incremental accumulator, and a node still running pre-fix logic correctly rejects blocks past the activation height (confirms the intended fork behavior, so it can't ship silently broken).
- **(Optional, only if Phase 4 protocol version gate is implemented later — not needed for the initial fix):** functional test confirming a peer advertising a pre-70018 version is disconnected during the `VERSION` handshake once `m_best_height` reaches the (test-configured) `MuhashAttestationActivationHeight`, and remains connected below it.

## 6. Priority Recommendation (original)

While the actual activation height can safely stay well out, the *engineering work* (persistent incremental accumulator, reorg-safe undo handling, snapshot metadata integration, benchmark-backed test coverage — see §3/§5) is nontrivial and worth starting now, not closer to when it's needed. The threshold math in §1.1 shows this stops being a "slow" problem and becomes a "the chain halts" problem at a UTXO count roughly 20–75× smaller than Bitcoin's current set — and Elektron Net's 10× block frequency means that threshold could be reached faster in calendar time than Bitcoin's own history suggests, if adoption ever meaningfully picks up. Recommend starting Phase 1 (§3) in the current development cycle.

---

## 7. Addendum: Codebase-Verified Complexity Assessment

The sections above restate the original review findings largely as-is. This addendum documents a follow-up check against the actual `kutlusoy/elektron-net` code (not just the design proposal) to ground the complexity estimate in what already exists, plus a revised recommendation on activation timing (§8). No code was changed as part of this addendum — documentation only.

### 7.1 Key finding: most of Phase 1's "hard part" already exists in this repo

Because Elektron Net is a Bitcoin Core fork, Bitcoin Core's optional `CoinStatsIndex` — which does *exactly* what §3 Phase 1 proposes building, just as a local out-of-consensus index rather than a consensus rule — ships in this codebase unmodified:

| Building block | Status | Location |
|---|---|---|
| `MuHash3072` class (Insert/Remove/Finalize, 3072-bit arithmetic, serialization) | Present, unmodified from upstream Bitcoin Core | `src/crypto/muhash.h` (~lines 102–137) |
| `CoinStatsHashType::MUHASH` enum value + `ComputeUTXOStats(MUHASH, …)` | Present | `src/kernel/coinstats.h:26-30` |
| `ApplyCoinHash()` / `RemoveCoinHash()` — the exact per-coin functions Phase 1 describes writing from scratch | Present | `src/kernel/coinstats.cpp` |
| Incremental per-block Insert/Remove with reorg-safe rollback via `CBlockUndo::vtxundo` | Present, production-tested logic | `src/index/coinstatsindex.cpp` (`CustomAppend`, `CustomRemove`, `RevertBlock`) |
| Persistent accumulator state (not just on-demand computation) | Present (LevelDB, `DB_MUHASH` key) | `src/index/coinstatsindex.cpp` |
| BIP30 / unspendable-output special cases | Already handled correctly | `src/index/coinstatsindex.cpp` |
| Test coverage for MuHash (fuzzing) and the index | Present | `src/test/fuzz/muhash.cpp`, `src/test/coinstatsindex_tests.cpp` |
| Height-gated consensus-change precedent (the pattern §4 proposes) | Present as an established pattern | `src/deploymentstatus.h`, `src/kernel/chainparams.cpp:97` (`MinDifficultyActivationHeight`) |
| Current (expensive) per-block full scan, confirmed as described | Confirmed | `src/validation.cpp` `ComputeBlockUTXOAttestationHash()` (~line 2321) and `ValidateUTXOCheckpoint()` (~line 2392), called from `ConnectBlock` (~line 2921) and `CreateNewBlock` in `src/node/miner.cpp` |
| Undo data availability at the exact hook points needed | Confirmed | `ConnectBlock`'s `UpdateCoins()` call (~line 2887) and `DisconnectBlock`'s reverse-order undo loop (~line 2204) both already have `CTxUndo`/`CBlockUndo` in hand |

The parts §3 treats as the biggest unknowns — correct Insert/Remove inversion across reorgs, 3072-bit modular arithmetic, serialization, BIP30 handling — are **not open work**. They have been running correctly in Bitcoin Core's `CoinStatsIndex` in production for years, and that exact code is already sitting in this repository.

### 7.2 What genuinely still needs building

`CoinStatsIndex` is deliberately **outside the consensus path**: it updates asynchronously via `SyncWithValidationInterfaceQueue` / a background thread, is optional and can be disabled (`-coinstatsindex=0`), is allowed to lag behind the chain tip, and a failure there never blocks a block. Upstream Bitcoin Core has no precedent for making a UTXO-set-derived hash a consensus rule at all. §2's proposal is the opposite: a value every node must compute bit-identically to even agree a block is valid — a bug here is a chain split, not a wrong RPC answer. That raises the bar regardless of how much of the underlying math is already proven.

Concretely, still to be built:

1. **Synchronous, mandatory hook** into `ConnectBlock` (~line 2887, where `UpdateCoins` already runs) and `DisconnectBlock` (~line 2204, where `CBlockUndo` is already available) — not the async `BaseIndex`/`interfaces::Chain` mechanism. The existing index is a reference implementation, not the final wiring.
2. **Persistence tied to the chainstate flush itself** (not a separately deletable/disable-able index DB), so the accumulator can never fall out of sync with the real UTXO set.
3. **Cheap simulation for mining** (`CreateNewBlock` in `miner.cpp`): today the whole block is tentatively replayed on a copy of the full UTXO cache just to compute-and-discard a hash. With the accumulator, only the small MuHash object (a few hundred bytes) needs to be copied and this one block's coin changes applied — removing the current double-simulation per block, but requiring a real restructuring of `ComputeBlockUTXOAttestationHash()` and its callers.
4. **Snapshot bootstrap integration**: `WriteAutomaticSnapshot()` (~line 2437) already does a full scan per checkpoint — the accumulator state can be computed in that same pass and embedded in the snapshot metadata so a bootstrapping node never has to hash from scratch. Requires extending `node::SnapshotMetadata` and the load path in `PopulateAndValidateSnapshot` (~line 6217).
5. **Height gate with two parallel validation paths** (old `HASH_SERIALIZED` for historical blocks, new for blocks at/after the activation height) — straightforward given the existing `DeploymentActiveAt` pattern, but every edge case here is a hard-fork risk.
6. **Test/audit effort disproportionate to the code size**, because of the consensus stakes: reorg correctness, activation-boundary testing on regtest, snapshot-bootstrap equivalence (§5) — this is the actual time sink, not writing the lines.

### 7.3 Revised complexity estimate

**Overall: medium — materially lower than §1–§6 imply, but not trivial.**

- The algorithmically hard parts (MuHash math, Insert/Remove inversion, serialization, BIP30 handling) are already finished, tested, and running in this exact repository — the part §3 mis-categorizes as "still to be designed."
- The real new work is converting an **async, optional, local index into a synchronous, mandatory consensus rule**: `ConnectBlock`/`DisconnectBlock` wiring, chainstate persistence, mining-path restructuring, snapshot metadata extension, height gate. Modest in code volume (rough order of magnitude: single-digit to low-double-digit developer-days for the code itself), but reorg/bootstrap/activation test coverage needs materially more time, because a defect here forks the chain rather than producing a harmless bug.
- No new cryptographic primitives or external libraries are required. The largest residual risk is disciplined engineering (accumulator object lifecycle/locking, pruning/IBD/reindex edge cases), not cryptography.

## 8. Addendum: Activation Timing — Ship Earlier, Not at 5× MANDATORY_PRUNE_DEPTH

The original recommendation in §4 proposed an activation height of `5 × MANDATORY_PRUNE_DEPTH` (height 986,400, ~1.9 years out at 60s blocks), chosen mainly to land on a checkpoint boundary while giving ample time to build and test.

Given that the chain is currently still well before the *first* checkpoint at height 197,280 — meaning `WriteAutomaticSnapshot()` has never yet run on the live network, and no node has ever bootstrapped via automatic snapshot (see `doc-elektron/fix-report-snapshot-bootstrap-trust.md` §6) — there is a stronger case for activating **well before height 197,280** instead of waiting ~1.9 years:

1. **No legacy snapshot format ever needs to exist.** If the MuHash switch activates before the first automatic snapshot is ever written, `WriteAutomaticSnapshot()`'s very first execution already uses the new algorithm and embeds the accumulator state (§3, Phase 2) from day one. There is no live snapshot data anywhere that would need a migration path or dual-format support — this isn't deferred, it's eliminated.
2. **The legacy `HASH_SERIALIZED` validation path only needs to cover a small, bounded amount of history** (tens of thousands of blocks, not up to 986,400) — a dual-path branch that's trivial to keep correct and trivial to eventually delete, versus a much larger, longer-lived compatibility surface if activation happens after years of chain history exist under the old algorithm.
3. **This is the cheapest point in the network's life to do a hard-fork-style change**: single miner, minimal value at stake, no independent third-party nodes yet relying on the old algorithm's behavior. The coordination cost of a consensus cutover only grows as the network decentralizes — doing it now, rather than after the network matures, avoids ever having to coordinate this specific change against a live, valuable, decentralized network.

**Caveat, unchanged from §3/§5:** "earlier" must not mean "immediately." The synchronous consensus hook, chainstate persistence, mining-path rewrite, and snapshot-metadata extension described in §7.2 do not exist yet and must be built and tested (§5) before any activation height is safe — rushing this specific piece of code is exactly the scenario the original report's "ship now, activate later" framing was designed to avoid. The revised recommendation is not "activate at the next block," it is: **pick an activation height that is comfortably before 197,280, sized to leave enough runway for implementation and the testing described in §5, rather than defaulting to the originally proposed 986,400.**

This report documents the recommendation only; no activation height has been finalized or implemented, and no code has been changed as part of this pass.
