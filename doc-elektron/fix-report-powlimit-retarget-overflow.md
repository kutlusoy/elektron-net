# Elektron Net: `pow.cpp` Retargeting Overflow Fix Report

- **Version:** 0.1 (draft)
- **Date:** August 05, 2026
- **Audience:** Core consensus maintainers, anyone reviewing chainparams changes
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) - `src/kernel/chainparams.cpp`, `src/pow.cpp`, `src/test/pow_tests.cpp` - treat as ground truth for anything this doc references
- **See also:** `WHITEPAPER.md`, `doc-elektron/ELEKTRON_NET_AI_AGENT_GUIDE.md`

---

## 1. Summary

`test_elektron --run_test=pow_tests` fails 7 of 18 test cases on current `main`. Two of those failures (`ChainParams_MAIN_sanity`, and the equivalent for `TESTNET`/`TESTNET4`) are not test artifacts. They catch a real violated safety invariant in the difficulty retargeting math: `consensus.powLimit` was raised for Mainnet/Testnet/Testnet4 without a matching reduction in `consensus.nPowTargetTimespan`, so the intermediate multiplication in `CalculateNextWorkRequired()` can silently wrap around `uint256`.

The remaining 4 failures (`get_next_work`, `get_next_work_pow_limit`, `get_next_work_lower_limit_actual`, `get_next_work_upper_limit_actual`) are a separate, lower-severity issue: stale upstream Bitcoin Core test fixtures that were never adapted to Elektron Net's parameters.

## 2. The overflow bug (MUST fix)

### 2.1 Where it lives

`src/pow.cpp`, `CalculateNextWorkRequired()`:

```cpp
bnNew.SetCompact(pindexLast->nBits);
bnNew *= nActualTimespan;      // <- intermediate product, no overflow check
bnNew /= params.nPowTargetTimespan;
```

`nActualTimespan` is clamped to at most `params.nPowTargetTimespan * 4`. `bnNew` starts at the current difficulty target, which is permitted by consensus to be as large as `params.powLimit`. `arith_uint256` performs fixed-width 256-bit multiplication with silent wraparound on overflow, there is no exception, no clamp, no assertion at that line.

Bitcoin Core's own inherited sanity check (`sanity_check_chainparams()` in `src/test/pow_tests.cpp`) exists specifically to guard the invariant this relies on:

```cpp
if (!consensus.fPowNoRetargeting) {
    arith_uint256 targ_max{UintToArith256(uint256{"ffff...ffff"})};
    targ_max /= consensus.nPowTargetTimespan * 4;
    BOOST_CHECK(UintToArith256(consensus.powLimit) < targ_max);
}
```

i.e. it requires `powLimit * 4 * nPowTargetTimespan < 2^256`.

### 2.2 Current parameter values and the resulting margin

| Network | `powLimit` (leading bytes) | `nPowTargetTimespan` | Invariant holds? | Overshoot factor |
|---|---|---|---|---|
| Mainnet | `0x7fffff00...` | `2016 * 60` = 120,960s | No | ~945x |
| Testnet | `0x7fffff00...` | `2016 * 60` = 120,960s | No | ~945x |
| Testnet4 | `0x7fffff00...` | `2016 * 60` = 120,960s | No | ~945x |
| Signet | `0x00000377ae...` | `2016 * 60` = 120,960s | Yes | n/a (well within bound) |
| Regtest | `0x7fffffff...` | `2016 * 60` = 120,960s | No (extreme), but `fPowNoRetargeting = true` skips the check at runtime | n/a, not reachable |

Worked example (Mainnet): with `bnNew` near `powLimit` and `nActualTimespan` clamped to its 4x maximum (483,840s), `bnNew * nActualTimespan` exceeds `2^256 - 1` by a factor of about 945. The subsequent division by `nPowTargetTimespan` does not undo this, the multiplication has already wrapped before the division executes.

### 2.3 Practical trigger conditions

This requires two things to coincide:

1. The chain's current difficulty target is close to `powLimit` (plausible on a young, low-hashrate network, or immediately after a difficulty crash).
2. A retargeting period where `nActualTimespan` is clamped to its upper bound (`nPowTargetTimespan * 4`), i.e. blocks over that period took roughly 4x longer than the 60s target on average.

Both conditions are realistic for Elektron Net specifically because of its stated design goal (permissive minimum difficulty for smaller miners) combined with its short, 60-second target spacing making timespan clamps easier to reach than on a slower chain.

### 2.4 Consequence if triggered

The wraparound produces a `bnNew.GetCompact()` result that is arithmetically wrong but not distinguishable from a "valid" retarget by any check currently in the code path, `ConnectBlock`/header validation only checks that `nBits` matches `CalculateNextWorkRequired()`'s output, not that the underlying math stayed in range. Depending on the exact wrapped value, next difficulty could come out anywhere from far too low (long stretch of trivially minable blocks) to a value inconsistent with what other nodes compute if intermediate rounding differs, an actual chain-split risk if any two implementations disagree on wraparound semantics. This is a consensus-critical parameter combination, not a cosmetic one.

## 3. Recommended fix

Restore the invariant `powLimit * 4 * nPowTargetTimespan < 2^256` for Mainnet, Testnet, and Testnet4. Two independent levers, either is sufficient on its own, both together give more headroom:

- **Lower `powLimit`.** Moving it back toward the `0x1d00ffff`-style magnitude used by Bitcoin (or any value at least ~3 orders of magnitude below the current `0x7fffff...`) restores the margin without touching timing parameters.
- **Lower `nPowTargetTimespan`.** Since blocks already arrive 10x faster than Bitcoin (60s vs 600s), the retargeting window could reasonably shrink proportionally, which independently restores the same margin.

Whichever lever is chosen, the fix MUST be applied to Mainnet, Testnet, and Testnet4 (Signet already satisfies the invariant; Regtest is unaffected at runtime due to `fPowNoRetargeting`, but consider tightening it anyway for consistency and to avoid the sanity test needing a network-specific exception).

After the parameter change, `ChainParams_MAIN_sanity`, `ChainParams_TESTNET_sanity`, and `ChainParams_TESTNET4_sanity` MUST pass without modification, they encode the correct invariant already; only the chainparams values are wrong.

## 4. Deployment strategy for a live network (MUST follow, not optional)

The network is already live with real miners and real chain history. A direct edit of `powLimit`/`nPowTargetTimespan` in `chainparams.cpp`, shipped and rolled out without coordination, would itself cause the exact consensus split this fix is meant to prevent: nodes running old binaries would keep validating blocks against the old values, nodes running new binaries would validate against the new values, and the two groups would disagree on which blocks are valid from the moment any node hits a retarget boundary. That is a self-inflicted hard fork, just an unplanned one instead of a planned one.

This MUST instead follow the same activation-height pattern already used elsewhere in this codebase for consensus changes (`MuhashAttestationActivationHeight`, `StoicAwakeningEndHeight`, `IntraBlockAttestationFixActivationHeight` in `src/consensus/params.h`). The fixed values only take effect at and after a pre-announced future block height; every block before that height is validated exactly as today, so existing chain history is untouched.

### 4.1 Shape of the change

- **New params field**, e.g. `int PowLimitFixActivationHeight = -1;` in `Consensus::Params` (`src/consensus/params.h`), following the same `-1` sentinel convention ("never active") used by the existing activation heights.
- **New post-fix values**, e.g. `uint256 powLimitPostFix` and, if that lever is used, an adjusted `nPowTargetTimespanPostFix`, set per network in `chainparams.cpp` alongside the existing pre-fix values.
- **Height-gated selection** inside `CalculateNextWorkRequired()` (and anywhere else `params.powLimit` / `params.nPowTargetTimespan` is read for consensus purposes, `GetNextWorkRequired()` and the sanity check included), selecting old vs. new values based on `pindexLast->nHeight + 1` relative to `PowLimitFixActivationHeight`, the same shape already used for the Muhash/attestation activation checks elsewhere in `validation.cpp`.
- **Sanity test update**: `sanity_check_chainparams()` in `src/test/pow_tests.cpp` MUST check the invariant against whichever value set is active at the height it is validating, so the test keeps meaning something both before and after the activation height, not just after.

### 4.2 Rollout sequence

1. Pick a target activation height far enough in the future to give miners, pool operators, and node operators realistic time to update, this is a coordination and communication timeline, not a code question, and MUST be decided by the maintainers, not hardcoded arbitrarily here.
2. Ship a release containing the fix with that height baked in, and announce it clearly through the project's usual channels (see Section 6, Official project links) so operators know why and by when to update.
3. Give operators the full lead time to upgrade. Track adoption if possible (e.g. via node/user-agent visibility on `mempool.elektron-net.org` or similar), the fix only protects the network once a supermajority of hashrate is running height-aware software.
4. At the activation height, all updated nodes switch to the corrected `powLimit`/`nPowTargetTimespan` simultaneously, by construction, since it is a height check rather than a wall-clock check, so it cannot fire at slightly different times on different nodes the way a time-based flag day could.
5. Nodes that never updated will diverge from the new rules starting at that height, this is expected and is the same trade-off every other height-gated consensus change in this codebase already accepts.

### 4.3 Why this is safe to do now rather than urgent to rush

Per Section 2.3, the bug only triggers when the current difficulty sits close to `powLimit` and a retarget period gets clamped to its 4x upper bound. A network with currently stable miners is, by definition, not close to that trigger condition right now. That gives real lead time to run the rollout above in an orderly way instead of as an emergency patch, but it is not a reason to defer indefinitely: hashrate conditions are not guaranteed to stay stable, and the fix itself is cheap relative to the cost of needing it during an actual hashrate drop.

## 5. Secondary issue: stale retargeting test fixtures (SHOULD fix)

`get_next_work`, `get_next_work_pow_limit`, `get_next_work_lower_limit_actual`, and `get_next_work_upper_limit_actual` in `src/test/pow_tests.cpp` hardcode `expected_nbits` values computed from real historical Bitcoin mainnet block heights and timestamps (e.g. block #30240, #32255) under Bitcoin's original 10-minute spacing and 2016-block retarget interval. These fixtures do not reflect Elektron Net's actual `nPowTargetSpacing` (60s) or the corrected `nPowTargetTimespan` from Section 3, so they currently fail regardless of whether Section 3's fix is applied, and they do not verify anything meaningful about Elektron Net's own retargeting behavior.

Recommended: replace the four hardcoded fixtures with values derived from Elektron Net's own parameters (either computed by hand against the corrected `nPowTargetTimespan`, or generated once via a known-good reference run and pinned), so the tests assert against this project's consensus rules instead of inherited upstream ones.

## 6. Checklist

- [ ] Decide fix lever: lower `powLimit`, lower `nPowTargetTimespan`, or both, for Mainnet/Testnet/Testnet4
- [ ] Add `PowLimitFixActivationHeight` (and post-fix value fields) to `Consensus::Params`, following the existing activation-height convention
- [ ] Gate `CalculateNextWorkRequired()` (and any other consensus read of `powLimit`/`nPowTargetTimespan`) on that height
- [ ] Update `sanity_check_chainparams()` to validate the invariant against whichever value set is active at a given height
- [ ] Decide and announce the target activation height with adequate operator lead time
- [ ] Re-run `ChainParams_MAIN_sanity`, `ChainParams_TESTNET_sanity`, `ChainParams_TESTNET4_sanity` and confirm they pass
- [ ] Consider tightening Regtest's `powLimit` for consistency, even though `fPowNoRetargeting` makes it unreachable at runtime
- [ ] Replace the four stale `get_next_work*` fixtures with values derived from Elektron Net's actual parameters
- [ ] Re-run the full `pow_tests` suite and confirm 18/18 pass
- [ ] If any block template, wallet, or mining-pool repo (`elektron-net-pool`, `elektron-net-ppool`) hardcodes assumptions about the current `powLimit` magnitude, check those for consistency after the parameter change
- [ ] Track operator/node upgrade adoption ahead of the activation height

## 7. Open Questions

1. Was the current `powLimit` value chosen deliberately for a specific minimum-difficulty target, or is it a placeholder that was never tuned? This determines whether the fix should aim for a specific new numeric target or simply "safely below the overflow bound."
2. Has any Mainnet or Testnet block already been mined under the current parameters where `nActualTimespan` came close to its 4x clamp? If so, that history should be checked for any sign the wraparound already occurred, not just the fact that it now can.
3. What activation height / minimum lead time is realistic given current miner and pool update practices? This needs input from whoever tracks operator communication channels, not just the code.
4. Should `powLimitPostFix` preserve the "easy start for small miners" intent as closely as possible while satisfying the invariant, or is Mainnet's hashrate now high enough that a stricter, more Bitcoin-like `powLimit` is acceptable? This is a product decision, not purely a safety one, since Section 2 only requires the value be low enough, not any specific target.
