# Elektron Net — SPV Header Difficulty Verification Guideline

- **Version:** 0.1 (draft)
- **Date:** July 18, 2026
- **Audience:** Any wallet vendor or library author implementing independent SPV (Simplified Payment Verification) header-chain validation for Elektron Net — i.e. any client that checks block header proof-of-work itself rather than trusting a server's word entirely
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) — `src/pow.cpp` (`GetNextWorkRequired`, `CalculateNextWorkRequired`, `PermittedDifficultyTransition`), `src/kernel/chainparams.cpp` (`CMainParams`) — treat these as ground truth for anything this doc references
- **Consumer:** [`elektron-net-electrum`](https://github.com/kutlusoy/elektron-net-electrum) — `electrum/blockchain.py` (`Blockchain.get_expected_target()`), a working reference port of the algorithm below, with tests in `tests/test_blockchain.py` (`TestStoicAwakeningDifficulty`)
- **See also:** [`guideline-wallet-integration.md`](./guideline-wallet-integration.md) §3.2 (where this was previously, incorrectly, described as "no adaptation needed"), [`hardfork-v3.0.1-stoic-awakening.md`](./hardfork-v3.0.1-stoic-awakening.md), [`CHANGELOG-stoic-awakening-retirement.md`](./CHANGELOG-stoic-awakening-retirement.md)

- never use this char "—" in your texts and comments. Use instead another notation.

---

## 1. Why this exists

Any SPV-style lightweight wallet (the Electrum family, but equally any bitcoinj-based, BIP157/Neutrino-based, or otherwise independently-verifying client) proves it's following the real chain by checking, for every header, that its hash meets the difficulty target its `bits` field claims, and that the sequence of `bits` values across the chain is itself the one the network's consensus rules would have produced. A client that skips this and just trusts whatever a server sends isn't doing SPV anymore, it's doing "trust the server" — a materially weaker security model, even if it happens to be a valid architectural choice for some wallets (see §5).

For a client that *does* do real SPV verification, this means: Elektron Net's actual difficulty-adjustment rules, not Bitcoin's, have to be implemented client-side. This was missed in the first wallet fork attempt (`elektron-net-electrum`, forked from `spesmilo/electrum`) — it carried over Bitcoin mainnet's `powLimit` and retarget timespan unchanged, and had no knowledge of the Stoic Awakening rule (§3) at all. The failure mode was total: header sync failed at height 1 against a real node, confirmed live (`unexpected bad header during binary` trying to validate the header right after genesis).

This is not a hypothetical edge case. As of this writing, the mainnet chain tip is still well inside the Stoic Awakening window (§3), meaning the rule below affects the *majority* of currently-existing header history, not a handful of early blocks that "don't matter anymore."

## 2. Baseline: adapting the classic Bitcoin retarget

Straightforward, but easy to get wrong by copy-pasting Bitcoin's constants instead of re-deriving them:

| Parameter | Bitcoin mainnet | Elektron Net mainnet |
|---|---|---|
| `powLimit` (max target / min difficulty) | compact `0x1d00ffff` | compact `0x1f7fffff` |
| `nPowTargetTimespan` (retarget window, real time) | 14 days | `2016 * 60` = 1.4 days |
| `nPowTargetSpacing` (target block time) | 600s | 60s |
| Retarget interval (`nPowTargetTimespan / nPowTargetSpacing`, in *blocks*) | 2016 | 2016 (same block count, since both the timespan and spacing shrink by the same 10x factor) |

The retarget interval in blocks is unchanged from Bitcoin, only the real-time window and the max/min difficulty bound differ. If your codebase (like Electrum's) has a separate "how many blocks per retarget chunk" constant from the "how many blocks per max/min difficulty" constant, only the latter two rows need changing; the chunk-size constant stays 2016.

`powLimit`'s exact value: `elektron-net/src/kernel/chainparams.cpp` (`CMainParams::CMainParams()`) has it as a hex `uint256` literal. If you're deriving the compact ("bits") form yourself, verify it against a *real* observed header rather than hand-computing it from the hex literal — the compact-bits encoding has enough edge cases (leading-zero-byte counting, sign bit) that a manual conversion is easy to get subtly wrong. A real Elektron Net height-1 header (2026-07-18, live network) had `bits = 528482303` (`0x1f7fffff`) — round-trip that through your own `bits <-> target` conversion functions and confirm it's stable, rather than trusting a from-scratch derivation.

This baseline alone is *necessary* but, per §3, not *sufficient* — implementing only this and nothing else still fails to validate most of the real chain's current header history.

## 3. "Stoic Awakening": the part that has no Bitcoin-mainnet equivalent

### 3.1 What it is

A temporary, mainnet-only minimum-difficulty escape, active for block heights in `[MinDifficultyActivationHeight, StoicAwakeningEndHeight)` — currently `[1, 150000)` (`elektron-net/src/kernel/chainparams.cpp`). It exists because, following a large miner's departure early in the network's life, the standard 2016-block retarget window was too slow to recover from a difficulty level the remaining hashrate couldn't sustain, causing multi-minute block delays. See [`hardfork-v3.0.1-stoic-awakening.md`](./hardfork-v3.0.1-stoic-awakening.md) for the original incident and rationale, and [`CHANGELOG-stoic-awakening-retirement.md`](./CHANGELOG-stoic-awakening-retirement.md) for why/how it's retired at height 150000.

Structurally, this is **the same rule as Bitcoin's own testnet `fPowAllowMinDifficultyBlocks`** — Elektron Net didn't invent a new algorithm, it applied a well-understood existing one (a real Bitcoin behavior, just never used on Bitcoin *mainnet*) to a height-bounded window on mainnet instead. If your SPV library already has testnet min-difficulty support, you likely already have most of the code you need; you just need to make it height-gated instead of "testnet-only."

### 3.2 The exact rule (ground truth: `elektron-net/src/pow.cpp`)

For a candidate block at height `h` (i.e. computing/verifying the difficulty required of `pindexLast->nHeight + 1` where `pindexLast` is the parent, height `h-1`):

1. **If `h` is a retarget boundary** (`h % 2016 == 0`): always the classic retarget computation (§2), completely unaffected by Stoic Awakening, regardless of whether `h` is inside `[1, 150000)`.
2. **Else, if `h` is inside `[1, 150000)`** ("stoic-active"):
   - If the candidate block's own timestamp is more than `2 * nPowTargetSpacing` (120s) after its parent's timestamp: required target = `powLimit` (easiest possible) for this one block.
   - Otherwise: required target = the bits of **the most recent ancestor that is either a retarget-boundary block, or a non-escape block** — i.e. walk backward from the parent, skipping any block whose own bits equal `powLimit`'s compact form, stopping as soon as you hit a retarget-boundary height (`height % 2016 == 0`, regardless of that block's own bits) or a block whose bits aren't `powLimit`. Use that block's bits directly.
3. **Else** (`h >= 150000`, non-boundary): required target = parent's bits, unchanged, no escape logic at all (this is also what step 2 reduces to in spirit for a mature, non-crashing network, since escape blocks stop occurring).

Note step 2's "walk backward" can, in principle, chain across *multiple* consecutive escape blocks (miner absence spanning several minute-plus gaps in a row) before finding real difficulty again — implementations must handle this, not just a single-hop lookback.

### 3.3 Implementation guidance for SPV clients specifically

Full nodes have the whole chain in a random-accessible index and can just walk `CBlockIndex::pprev` pointers as needed (§3.2 as literally written). An SPV client validating a batch/chunk of headers it just downloaded, not yet persisted to local storage, needs to make sure step 2's backward walk can see headers *within the batch being validated*, not only ones already saved from prior syncs — a naive implementation that only reads from persisted storage will incorrectly fail to validate valid headers whenever an escape chain spans across a sync-batch boundary. See `elektron-net-electrum`'s `Blockchain.get_expected_target()` (`electrum/blockchain.py`) for a worked implementation of this specific concern (the `chunk_headers` parameter).

### 3.4 What this doesn't affect

Cumulative chain-work computations (used to pick between two competing valid header chains during a reorg — not for header validation itself) are a separate concern from validating an individual header's `bits`. A client that assumes uniform per-block work within a whole 2016-block retarget chunk will under/overestimate work for chunks inside the Stoic Awakening window. This doesn't cause valid headers to be rejected or invalid ones accepted; it only matters if your client ever needs to resolve a fork while the chain tip is still inside `[1, 150000)`. Get header validation (§3.2) right first; this is a secondary refinement.

## 4. Testing recommendation

Don't trust a from-scratch reimplementation without cross-checking against real network data. At minimum:

- Confirm your `powLimit` compact-bits value round-trips correctly and matches a real observed header (§2).
- Construct synthetic header sequences (they don't need valid proof-of-work for testing the *target computation* logic in isolation, only for testing the full hash-vs-target check) exercising: a plain non-boundary carry-forward, an escape trigger (>120s gap), a multi-block escape chain followed by a real-difficulty block, the boundary-height exception (classic retarget always wins regardless of Stoic Awakening state), and the post-150000 "no escape logic at all" behavior.
- If practical, sync against a real Elektron Net node/electrs instance from genesis and confirm the full header chain validates without errors, not just a hand-picked few heights.

## 5. If you're not doing SPV verification at all

Not every wallet architecture needs this. A wallet that's really a thin client of a trusted backend API (no independent header/PoW checking, the server's word is simply trusted) never touches this problem, because it never validates difficulty client-side in the first place. That's a legitimate, simpler architecture with a different trust model, not a mistake, but it should be a deliberate choice, not an accidental one from not realizing SPV verification was supposed to be happening. If your wallet claims "SPV security" in its documentation or marketing, make sure it's actually implementing this, not silently skipping it.
