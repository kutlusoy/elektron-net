# Changelog: Stoic Awakening Retirement (`StoicAwakeningEndHeight`)

**Status:** Implemented 2026-07-13, shipped in **v4.0.3**. Stoic Awakening (the mainnet min-difficulty
escape active from block 1, `MinDifficultyActivationHeight = 1`) stops
applying to mainnet blocks at and after height **150000**. Blocks below that
height keep validating exactly as before — this is a forward-only retirement,
not a retroactive rewrite.
**Related:** `doc-elektron/hardfork-v3.0.1-stoic-awakening.md` and
`doc-elektron/DESIGN_RATIONALE.md` §"120 Seconds" (original design intent),
`WHITEPAPER.md` §3.3 (original rationale), `BITCOIN_CORE_DIFF.md` §2.4
(consensus rule summary), `doc-elektron/CHANGELOG-muhash-attestation.md`
(the changelog this file's format follows).

This file records *why* and *how* Stoic Awakening was retired, in the spirit
of `CHANGELOG-muhash-attestation.md`.

---

## 2026-07-13

### The problem

Stoic Awakening was designed as an emergency relief valve: if the gap since
the last block exceeds 120s (2× the 60s target spacing), the *next* block may
be mined at `powLimit` (minimum difficulty), then the chain immediately
returns to normal difficulty. The design docs assumed this would fire rarely
(`WHITEPAPER.md` originally estimated "~95% of blocks appear within 120s",
i.e. a ~5% trigger rate) and only during genuine hashrate shocks (a large
miner disconnecting).

Two issues surfaced from a user report of blocks being found "in
one-second intervals" with no change in miner participation, confirmed
against a live mempool explorer screenshot of the actual mainnet difficulty-
adjustment history (heights ~80638–86688):

1. **The trigger fires far more often than assumed, even at perfectly stable
   hashrate.** Block discovery is a Poisson process; inter-block gaps are
   exponentially distributed. At a 60s mean with a 120s (2×) threshold:
   `P(gap > 120s) = e^(-120/60) = e^-2 ≈ 13.5%` — roughly 1 in 7–8 blocks,
   not 1 in 20. This is pure statistical variance, not a hashrate change, and
   is inherent to the mechanism at any constant hashrate.

2. **A single escape block landing on the last block of a retarget period
   corrupts the entire next 2016-block epoch.** `CalculateNextWorkRequired()`
   (`src/pow.cpp`) uses `pindexLast->nBits` — the bits of the *last* block of
   the outgoing period — as the baseline for the next epoch's difficulty. If
   that last block happens to be a Stoic Awakening escape block (nBits ==
   `powLimit`), the entire next epoch is computed from a near-zero baseline
   instead of the real prevailing difficulty. Since each retarget period can
   only recover by at most ×4 (+300%, the existing
   `nActualTimespan`/4..×4 clamp), it takes multiple consecutive
   max-capped periods (days, at 2016 blocks/period) to climb back — during
   which the chain runs at anomalously low, easily-attackable difficulty and
   produces far more than the intended 1,440 blocks/day.

   This reproduced exactly in the reported live data: height 80638 (real
   difficulty 12.71k) → 80639 (Stoic Awakening escape, -100% to ~powLimit,
   not a retarget-interval height) → **80640** (a genuine retarget height,
   `2016 × 40`) inherits the crashed baseline from 80639, so despite a
   nominal "+29.99k%" the absolute difficulty stays near zero → 82656, 84672,
   86688 (each exactly +2016 blocks later) each hit the +300% recovery cap in
   succession, still not fully recovered by the last of the three.

   A live diagnostic taken mid-crash-cascade (tip 82858, epoch 82656–84671)
   showed this compounding worse than the pure ×4-per-period math above
   suggests: the previous epoch (80640–82655) averaged **2.96s/block**
   against a 60s target; the *current* epoch, despite already having jumped
   difficulty ×4 (0.00003587 → 0.00014348) relative to that previous epoch,
   was averaging **1.48s/block** — faster, not slower, than the epoch before
   the difficulty increase. The raw adjustment factor needed to fully correct
   (40.535×) far exceeds what a single ×4-capped period can deliver.
   The likely reason block times get *faster* even as difficulty rises: the
   crashed-difficulty window is trivially profitable, so opportunistic
   miners (solo/pool operators harvesting near-free block rewards — see the
   "Solo Pool Miner" entries appearing in the mempool explorer during this
   period) pile onto the chain for as long as it stays cheap, adding real
   hashrate that keeps outpacing each capped recovery step. This makes the
   recovery self-prolonging rather than a fixed multi-day event, and is an
   additional argument for retiring the mechanism outright rather than
   tuning its threshold.

   This same failure mode is a well-known wart of Bitcoin's testnet3
   difficulty history — Stoic Awakening imported it onto mainnet by design
   (with a height gate instead of a permanent flag), without the mitigation
   Bitcoin later added for testnet4 (BIP94's "use the *first* block of the
   period as the baseline instead" — already present in this codebase as
   `enforce_BIP94`, but not enabled on mainnet).

### Decision

Rather than recalibrate the trigger threshold or backport the BIP94
first-block-baseline fix indefinitely, the decision (made directly with the
user, 2026-07-13) was to **retire Stoic Awakening on mainnet entirely** at a
future height, once real-world experience showed the "barbell" trade-off
(occasional solo-miner wins vs. multi-day difficulty distortions) wasn't
landing as designed. Bitcoin's original testnet-only min-difficulty rule
remains available via `fPowAllowMinDifficultyBlocks` for testnet/testnet4/
regtest, untouched by this change.

### Implementation

**New consensus parameter.** `Consensus::Params::StoicAwakeningEndHeight`
(`src/consensus/params.h`), same `-1`-disabled sentinel convention as
`MinDifficultyActivationHeight`/`MuhashAttestationActivationHeight`. `-1`
means the escape never ends (the historical default, and the value for
every network except mainnet).

**`src/pow.cpp`:** both `GetNextWorkRequired()` and
`PermittedDifficultyTransition()` now additionally require
`StoicAwakeningEndHeight == -1 || height < StoicAwakeningEndHeight` before
allowing the min-difficulty escape, alongside the existing
`fPowAllowMinDifficultyBlocks` / `MinDifficultyActivationHeight` check. Only
the Stoic Awakening (`MinDifficultyActivationHeight`) path is end-height
gated — `fPowAllowMinDifficultyBlocks` (testnet/testnet4/regtest) is
untouched.

**`src/kernel/chainparams.cpp` (mainnet only):**
`consensus.StoicAwakeningEndHeight = 150000;`. Chosen with the user directly:
mainnet tip was ~86829 when decided (2026-07-13), giving ~4-5 weeks of real
lead time at the then-current (bug-inflated) ~2150 blocks/day rate for pool
operators, node operators, and wallets to update before the cutover —
consistent with the precedent set by `MuhashAttestationActivationHeight`
(`CHANGELOG-muhash-attestation.md`), including the same caveat: **re-verify
against the live tip before relying on this height if significant time has
passed**, since the actual rate may itself normalize once earlier crash
cascades finish recovering.

**Tests** (`src/test/pow_tests.cpp`): `stoic_awakening_end_height` (escape
still fires just below the end height, stops firing at and above it, for
both `GetNextWorkRequired` and `PermittedDifficultyTransition`) and
`stoic_awakening_no_end_height_by_default` (confirms `-1` — the default,
and the value used by every other network — never retires the escape,
protecting testnet/testnet4/regtest and any future network that doesn't set
this field).

### What does *not* change

- Blocks mined below height 150000 validate exactly as before — this is not
  retroactive. A node re-syncing from genesis still needs to accept Stoic
  Awakening escape blocks in that range.
- Testnet, Testnet4, and Regtest are unaffected — they use
  `fPowAllowMinDifficultyBlocks`, a separate, unconditional flag that this
  change does not touch.
- The `enforce_BIP94` first-block-baseline mitigation was **not** enabled on
  mainnet as part of this change. It remains a possible defense-in-depth
  addition for the pre-150000 window, but since it would itself need its own
  height gate to avoid retroactively invalidating already-mined
  retarget-boundary blocks, and the escape is being removed outright rather
  than kept and hardened, it was left out of scope here.

### Operator impact

Mining pools and node operators must upgrade before height 150000. After
that height:
- `nBits` on mainnet changes **only** at 2016-block retarget boundaries,
  exactly like Bitcoin mainnet — never mid-period.
- Block templates (GBT) will no longer see `bits` change in response to a
  >120s gap; the "no pool patch needed" guidance in
  `doc-elektron/mining-pool-integration.md` for Stoic Awakening becomes
  moot from that height onward (pools already following the documented GBT
  contract need no further changes — there is simply no more mid-period
  `bits` change to react to).
