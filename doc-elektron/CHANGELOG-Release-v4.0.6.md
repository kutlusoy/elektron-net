# Changelog: Release v4.0.6

**Status:** Built from branch `4.0.6`. Not yet tagged/shipped as of this writing.
**Related:** `fix-report-snapshot-restart-deadlock.md` (snapshot self-heal detail),
`fix-report-powlimit-retarget-overflow.md` (powLimit overflow fix detail),
`WHITEPAPER.md`, `BITCOIN_CORE_DIFF.md`.

This file is a release-level summary of everything shipped in v4.0.6, in
the spirit of `CHANGELOG-Release-v4.0.4.md`. See the related files above
for the full background, root-cause investigation, and rationale behind
each change.

---

## Node: snapshot-restart self-heal

A node that had bootstrapped via the automatic UTXO-snapshot mechanism
and then lost its usable local chain data (either from an interrupted
live snapshot replacement, or -- the more common trigger -- a routine,
*expected* rejection of its own stale local snapshot file immediately
followed by a real replacement) would previously hang permanently at
startup with no error message, RPC never coming up, indistinguishable
from "still starting" to any operator or monitoring system.

- **Root cause**, precisely isolated through live reproduction on
  regtest: not "how many checkpoints behind," as first suspected, but a
  specific sequence -- a node's own now-stale local snapshot file gets
  correctly rejected, and a real replacement begins immediately after.
  `ActivateSnapshot()` wipes the currently-active snapshot chainstate's
  on-disk data *before* validating whether the new candidate will even
  be accepted, so any rejected candidate -- not just a failed one --
  destroys the previously-valid data as a side effect.
- **Fix**: rather than trying to patch the resulting broken state
  forward (two attempts at this were made, both reverted after
  surfacing deeper chainstate-loading invariants not worth the risk to
  touch), the node now detects "no chainstate has a usable local tip,
  and the network has advanced far enough past it (`MandatoryPruneDepth`
  blocks) that local recovery is architecturally impossible" and
  restarts itself with `-reindex` automatically (`execv`, same PID),
  reusing the already-verified-reliable full-reindex recovery path
  instead of inventing a new one.
- **One-shot guard**: a marker file records that an automatic restart
  was already attempted; if the broken state somehow recurs before the
  marker is cleared, the node fails loudly instead of restart-looping.
  The marker clears itself on the next boot that finds a healthy tip, so
  a genuinely new future occurrence still gets its own single automatic
  attempt. Confirmed the guard does not fire on any of the ordinary
  paths (fresh node, short offline gap, multi-checkpoint fresh bootstrap)
  and does correctly block a second automatic attempt when the same
  broken state is forced to recur.
- **Wallets and the config file are untouched**: confirmed both by
  reading `-reindex`'s own documented behavior and empirically (`stat`
  showed unchanged mtimes across the crash-and-recover cycle) -- this is
  block-index/chainstate rebuilding only, same as a manual `-reindex`.
- **Live-tested and bit-for-bit verified, at two different height
  regions** (the original ~900-1200 checkpoints, and a retest at
  ~2000-2400): reproduced the crash, confirmed the automatic
  restart-with-`-reindex` message, watched the re-exec'd process
  re-bootstrap via a fresh snapshot with no manual intervention, and
  confirmed `bestblockhash`/`gettxoutsetinfo muhash` matched the source
  node exactly afterward. A subsequent plain restart (no special flags)
  came up cleanly in ~1-2 seconds. Also ran the full scenario matrix
  (fresh node; short-offline-gap reconnect; fresh node after several
  snapshot generations; the actual bug trigger) to confirm the fix
  changes nothing about the already-reliable ordinary paths.

See `fix-report-snapshot-restart-deadlock.md` for the full investigation,
including the two reverted attempts and why each didn't work.

## Node: powLimit retargeting overflow fix

`CalculateNextWorkRequired()` (`src/pow.cpp`) could silently overflow
`arith_uint256` during difficulty retargeting on Mainnet, Testnet, and
Testnet4, because `powLimit * 4 * nPowTargetTimespan` exceeds `2^256` by
a factor of ~945x for the parameters those networks use -- a real
consensus-safety bug (found via a failing chainparams sanity test, not
cosmetic), not previously fixed since being reported August 5, 2026.

- **Fix**: `powLimit` lowered by the minimum amount that restores a real
  safety margin (right-shifted 12 bits / divided by 4096, ~4.3x margin
  over the bare mathematical minimum), `nPowTargetSpacing`/
  `nPowTargetTimespan` left untouched. Height-gated (new
  `PowLimitFixActivationHeight`/`powLimitPostFix` consensus params,
  same pattern as `MuhashAttestationActivationHeight` etc.), so existing
  chain history retargets byte-for-byte identically to before this
  change; only blocks at or after the activation height use the
  corrected value.
- **No existing mainnet activation height was touched.** This is a
  purely additive change: `StoicAwakeningEndHeight`,
  `MuhashAttestationActivationHeight`,
  `IntraBlockAttestationFixActivationHeight`, `MandatoryPruneDepth`, and
  every other previously-live consensus parameter are unchanged on every
  network.
- **Activation heights**: Mainnet **500000** (tip was ~190000 at the
  time this was set, giving real operator lead time -- adjustable before
  release if needed). Testnet/Testnet4/Regtest: **1** (immediately
  active, no operator-coordination need on those networks; a no-op at
  runtime on Regtest specifically, since `fPowNoRetargeting` bypasses
  retargeting entirely there).
- **Verified**: `ChainParams_MAIN_sanity`, `ChainParams_TESTNET_sanity`,
  and `ChainParams_TESTNET4_sanity` all pass (they previously failed,
  catching the real invariant violation). A separate, lower-severity,
  pre-existing test-fixture issue (`get_next_work*`, 4 tests using stale
  hardcoded Bitcoin-mainnet fixtures) remains, unrelated to and
  unaffected by this fix -- explicitly out of scope for this pass.

See `fix-report-powlimit-retarget-overflow.md` for the full analysis,
including the safety-invariant math and deployment reasoning.

## Version

`CLIENT_VERSION_BUILD` bumped from 5 to 6 in `CMakeLists.txt`
(`v4.0.5` -> `v4.0.6`); `CLIENT_VERSION_MAJOR`/`MINOR` unchanged.
