# Elektron Net — `elektron-net-electrs` Fork Integration Guideline

- **Version:** 0.1 (draft)
- **Date:** July 12, 2026
- **Audience:** Rust developers, Electrum-protocol server operators, anyone forking `romanz/electrs` for Elektron Net
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) — `src/wallet/wallet.cpp` (`ScanUTXOSet()`/`CreditUTXOFromChain()`), `src/kernel/chainparams.cpp` — treat these as the ground truth for the bootstrap model and network parameters
- **Fork base:** [`romanz/electrs`](https://github.com/romanz/electrs)
- **Consumer:** [`elektron-net-mempool`](https://github.com/kutlusoy/elektron-net-mempool) — see its `docker-compose.yml` for the pre-wired, currently-disabled `elektron-electrs` service profile that this fork is meant to satisfy
- **See also:** [`Elektron Net — Wallet Integration Guideline`](./Elektron-Net_Wallet-Integration-Guideline.md) (this document covers §3.2/§3.5 of that guideline in full depth), [`BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](https://github.com/kutlusoy/elektron-net/blob/main/WHITEPAPER.md)

This document can be worked on independently of the wallet-client fork it feeds. Requirement levels follow common practice: **MUST** = mandatory for correct/safe operation, **SHOULD** = strongly recommended, **MAY** = optional.

---

## 1. Why `elektron-net-electrs` Is Needed

Elektron Net enforces mandatory pruning on every node (`MandatoryPruneDepth` / `nPruneAfterHeight` = 197,280 blocks ≈ 137 days at a 60-second block time, no archival mode exists) and bases consensus on a continuously attested UTXO set (MuHash, active from block 137,000) rather than on block history. `elektron-net-mempool` already anticipates an Electrum-protocol server for address lookups and pruning-independent transaction detail, and pre-wires a disabled `elektron-electrs` service profile for it — but the repository itself does not yet exist. This guideline defines what building it correctly requires.

Building this server is the single prerequisite for any mobile wallet that uses the Electrum protocol (see the companion Wallet Integration Guideline, §3.2/§4). Nothing else in that guideline can be tested end-to-end without it.

## 2. Why `romanz/electrs` Is the Recommended Base

Compared to the older Python ElectrumX/Fulcrum lineage, `romanz/electrs`:

- uses a single RocksDB store rather than a heavier multi-file database layout,
- has an indexing model whose divergence points from what Elektron Net needs are localized (startup/bootstrap and merkle-proof generation), not architectural,
- is actively maintained, with a documented schema (`doc/schema.md`) that makes it straightforward to reason about what a bootstrap record would need to look like.

This assessment is based on direct inspection of the upstream source (cloned at HEAD, July 2026), not just its documentation.

## 3. Required Adaptations (from source inspection)

### 3.1 Fork status: permanent, not upstreamable

The upstream README states plainly that altcoins are not supported and that forks of the Bitcoin codebase which relax consensus rules are also not supported, directing anyone needing that to maintain their own fork rather than file issues/PRs upstream. `elektron-net-electrs` MUST therefore be planned as an independent, permanently-diverged fork from day one. Rebasing onto upstream periodically (to pick up protocol/security fixes) is fine and encouraged; Elektron Net–specific changes will not be merged back.

### 3.2 The hardcoded pruning check MUST be replaced, not bypassed

`src/daemon.rs`, inside `rpc_poll()`, calls `getblockchaininfo` and hard-fails with:

```
"electrs requires non-pruned bitcoind node"
```

whenever `info.pruned` is true. Since every Elektron Net node enforces pruning unconditionally, this check will always trigger, and there is no non-pruned node anywhere on the network to point it at instead — this is not a configuration problem to work around, it is an assumption baked into upstream that does not hold for this chain.

**Required replacement logic:** on startup, when `info.pruned` is true (i.e. always, for Elektron Net):

1. Call an `elektrond` RPC equivalent to `scantxoutset` (or a purpose-built RPC exposing the same live UTXO-set iteration that `elektron-net`'s own `CWallet::ScanUTXOSet()` uses via `chain().forEachCoin()` → `ActiveChainstate().CoinsTip().ForEachUnspent()`) to build the initial scripthash → UTXO index in one pass.
2. Record the block height at which this bootstrap ran as the index's effective "genesis" — every scripthash's history before this height is, by design, unavailable, not corrupted.
3. Fall through to the existing block-by-block indexing loop for everything from that height forward, which needs no further modification (see §3.5).

### 3.3 Network identity: `rust-bitcoin`'s `Network` enum does not know Elektron Net

`src/config.rs` hardcodes RPC ports, Electrum-protocol ports, and monitoring ports per `Network` variant (e.g. `Network::Bitcoin` → RPC 8332 / Electrum 50001 / monitoring 4224; `Testnet` → 18332/60001/14224; etc.), and downstream address/HRP encoding is derived from the same enum via the `rust-bitcoin` dependency chain. There is no variant for a new chain, and none can be added without touching a third-party crate.

Two viable strategies, both used by comparable altcoin electrs forks:

1. **Vendor a patched `rust-bitcoin`** with an added `ElektronNet` variant. Correct long-term, but invasive, and needs re-patching on every `rust-bitcoin` upgrade — a real maintenance cost to budget for.
2. **Repurpose an existing variant** (commonly `Network::Bitcoin`) as an internal stand-in, overriding HRP (`be`), magic bytes (`0xe1 0xec 0x7a 0x6e`), and port constants at the `elektron-net-electrs` config layer.

Either is workable. What matters is that the choice is made **explicitly and documented in code comments** at the point of substitution — not left as an implicit side effect of copy-pasting the Bitcoin variant's config block, where a future contributor could easily mistake it for an actual Bitcoin-mainnet code path.

### 3.4 Merkle-proof generation MUST degrade gracefully past the pruning window

`src/merkle.rs` computes merkle branches from full block data fetched on demand via RPC (`getblock`/`get_block_txids` in `src/daemon.rs`). Once a block ages past the 197,280-block retention window, these RPC calls will fail against a pruned `elektrond` node — expected and correct, not a bug to route around.

**Required behavior:** `blockchain.transaction.get_merkle` (and any internal equivalent) MUST return a typed, documented "proof unavailable — block pruned" response for heights below the node's retention horizon, rather than propagating a raw RPC error up to the Electrum client. Wallet clients consuming this server need a stable way to distinguish "proof unavailable by design" from "server malfunction."

### 3.5 Steady-state indexing needs no changes

Once the bootstrap height from §3.2 is established, ordinary block-by-block indexing via `getblock`/`get_block_txids` works exactly as upstream intends, because Elektron Net never prunes a block before it exceeds the retention window. The fork's divergence from upstream is concentrated entirely in startup/bootstrap (§3.2) and the merkle-proof edge case (§3.4) — the day-to-day indexing path, RocksDB schema, and Electrum-protocol server (`src/electrum.rs`) should be reusable close to as-is.

## 4. Relationship to Header-Chain Availability

Elektron Net always retains the full header chain, even though block bodies are pruned. This means chain-of-work verification for headers is unaffected by any of the above — only per-transaction merkle proofs are impacted (§3.4), not header sync itself. This should be called out explicitly in `elektron-net-electrs`'s own documentation so downstream wallet developers do not conflate the two.

## 5. Checklist

- [ ] Fork `romanz/electrs` at a pinned commit; set up a rebase cadence against upstream for security/protocol fixes
- [ ] Replace the hard-fail pruned-node check in `src/daemon.rs::rpc_poll()` with the UTXO-set bootstrap path (§3.2)
- [ ] Confirm/implement the `elektrond`-side RPC the bootstrap depends on (a `scantxoutset`-equivalent exposing live UTXO iteration)
- [ ] Decide and document the `Network` enum strategy (§3.3) — patched `rust-bitcoin` vs. repurposed variant with overridden constants
- [ ] Implement the graceful merkle-proof fallback in `src/merkle.rs` (§3.4)
- [ ] Document the header-chain vs. block-body distinction (§4) prominently, so wallet integrators don't misread the trust model
- [ ] Enable and test the `elektron-electrs` Docker Compose profile already present in `elektron-net-mempool`
- [ ] Hand off to wallet-client integration once a scripthash lookup, a balance query, and a recent-block merkle proof all return correct results end-to-end

## 6. Open Questions

1. Should the bootstrap RPC be a new `elektrond` RPC method, or should `scantxoutset` be extended/reused as-is?
2. Which `Network`-enum strategy (§3.3) should be adopted, and who owns the `rust-bitcoin` patch if option 1 is chosen?
3. Should this fork be published publicly from the start, or developed privately until the bootstrap/merkle-proof adaptations are stable?
