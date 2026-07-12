# Elektron Net — `elektron-net-electrs` Fork Integration Guideline

- **Version:** 0.3
- **Date:** July 12, 2026 (draft: July 12, 2026)
- **Audience:** Rust developers, Electrum-protocol server operators, anyone forking `romanz/electrs` for Elektron Net
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) — `src/wallet/wallet.cpp` (`ScanUTXOSet()`/`CreditUTXOFromChain()`), `src/kernel/chainparams.cpp` — treat these as the ground truth for the bootstrap model and network parameters
- **Fork base:** [`romanz/electrs`](https://github.com/romanz/electrs)
- **Consumer:** [`elektron-net-mempool`](https://github.com/kutlusoy/elektron-net-mempool) — see its `docker-compose.yml` for the pre-wired, currently-disabled `elektron-electrs` service profile that this fork is meant to satisfy
- **See also:** [`Elektron Net — Wallet Integration Guideline`](./guideline-wallet-integration.md) (this document covers §3.2/§3.5 of that guideline in full depth), [`BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](https://github.com/kutlusoy/elektron-net/blob/main/WHITEPAPER.md)

This document can be worked on independently of the wallet-client fork it feeds. Requirement levels follow common practice: **MUST** = mandatory for correct/safe operation, **SHOULD** = strongly recommended, **MAY** = optional.

> **Review note (v0.2):** Every claim below about `elektron-net-electrs` behavior has been re-checked against the actual fork at `kutlusoy/elektron-net-electrs@main` (`src/daemon.rs`, `src/electrum.rs`, `src/merkle.rs`, `src/config.rs`), and every claim about Elektron Net's own parameters has been re-checked against `kutlusoy/elektron-net@main` (`src/kernel/chainparams.cpp`, `src/rpc/blockchain.cpp`). §3.2 and §3.3 were revised based on that inspection; changes are marked inline.
>
> **v0.3:** adaptation work has started — the fork now exists (base: upstream v0.10.10, adaptation branch `integration`); §3.2 (pruned-check replacement), §3.4 (typed code-3 error) and the P2P handshake requirements (magic override, protocol version 70017) are implemented there. New in this revision: §3.6 — the index itself MUST follow the chain's retention rule (self-contained concept), decided July 12, 2026.

---

## 1. Why `elektron-net-electrs` Is Needed

Elektron Net enforces mandatory pruning on every node (`MandatoryPruneDepth` / `nPruneAfterHeight` = 197,280 blocks ≈ 137 days at a 60-second block time, no archival mode exists) and bases consensus on a continuously attested UTXO set (MuHash, active from block 137,000) rather than on block history. `elektron-net-mempool` already anticipates an Electrum-protocol server for address lookups and pruning-independent transaction detail, and pre-wires an `elektron-electrs` service profile for it. The fork now exists at [`elektron-net-electrs`](https://github.com/kutlusoy/elektron-net-electrs) (based on upstream v0.10.10; adaptations on the `integration` branch). This guideline defines what building it correctly requires.

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

whenever `info.pruned` is true (confirmed verbatim in the fork at `src/daemon.rs`, `Daemon::rpc_poll()`). Since every Elektron Net node enforces pruning unconditionally, this check will always trigger, and there is no non-pruned node anywhere on the network to point it at instead — this is not a configuration problem to work around, it is an assumption baked into upstream that does not hold for this chain.

**Required replacement logic:** on startup, when `info.pruned` is true (i.e. always, for Elektron Net):

1. Obtain a full scripthash → UTXO snapshot to seed the index in one pass, rather than replaying it block-by-block from a `-txindex` that Elektron Net doesn't allow.
2. Record the block height at which this bootstrap ran as the index's effective "genesis" — every scripthash's history before this height is, by design, unavailable, not corrupted.
3. Fall through to the existing block-by-block indexing loop for everything from that height forward, which needs no further modification (see §3.5).

**Recommended bootstrap source (revised in v0.2 — resolves Open Question 1):** `elektrond` already writes exactly this kind of artifact on its own, unprompted. `src/kernel/chainparams.cpp`/`src/validation.cpp`'s automatic checkpoint mechanism (`WriteAutomaticSnapshot`, see the *Elektron Net vs Bitcoin Core* diff, §2.3) dumps a full AssumeUTXO-format `.dat` snapshot plus a `.hash` sidecar to `datadir/snapshots/` at every `MandatoryPruneDepth` boundary (every 197,280 blocks on mainnet), and the node itself already validates that file's content against the sidecar hash before ever trusting it (`PopulateAndValidateSnapshot`). Standard Bitcoin Core's `dumptxoutset` RPC is also still present and unmodified (confirmed registered in `src/rpc/blockchain.cpp`'s `RegisterBlockchainRPCCommands()`, alongside `scantxoutset`), and produces the same snapshot format on demand for any height, not just checkpoint heights.

Two consequences for the bootstrap design:

- **Prefer `dumptxoutset` (or reading the latest validated `datadir/snapshots/*.dat` + `.hash` pair directly) over `scantxoutset`.** `scantxoutset` only scans for UTXOs matching caller-supplied descriptors/scripts — it has no "give me everything" mode, so it cannot serve as a full-index bootstrap on its own. `dumptxoutset` (or the on-disk checkpoint snapshot) is a full UTXO-set dump and is the correct fit.
- **Do not target `interfaces::Chain::forEachCoin()` / `CWallet::ScanUTXOSet()`.** Those are cited in the wallet-side diff as the *in-process* mechanism Elektron Net's own bundled wallet uses (they're C++ interface calls inside the same `elektrond` binary, not a JSON-RPC method) — `elektron-net-electrs` runs as a separate OS process and cannot call them directly. They are useful only as evidence that a full-UTXO-set iteration primitive already exists node-side; the actual integration point for an external process is the RPC/snapshot-file surface above, not this interface.

This resolves Open Question 1 (§6) with a concrete recommendation: no new custom `elektrond` RPC is needed. Build the bootstrap against the existing AssumeUTXO-format snapshot artifact, either via `dumptxoutset` or by parsing the automatically-written checkpoint files directly.

### 3.3 Network identity: `rust-bitcoin`'s `Network` enum does not know Elektron Net

`src/config.rs` hardcodes RPC ports, Electrum-protocol ports, and monitoring ports per `Network` variant (e.g. `Network::Bitcoin` → RPC 8332 / Electrum 50001 / monitoring 4224; `Testnet` → 18332/60001/14224; etc.), and downstream address/HRP encoding is derived from the same enum via the `rust-bitcoin` dependency chain. There is no variant for a new chain, and none can be added without touching a third-party crate.

**Revised in v0.2 — the required override surface is much smaller than the two-strategy framing below originally implied.** Direct comparison of `elektron-net-electrs`'s `config.rs` defaults against Elektron Net's actual `src/kernel/chainparams.cpp` shows:

| Parameter | Elektron Net mainnet | `rust-bitcoin` `Network::Bitcoin` | Match? |
|---|---|---|---|
| RPC port | 8332 (unchanged from Bitcoin, per `chainparams.cpp`/`BITCOIN_CORE_DIFF.md` §1) | 8332 (electrs default) | **Identical** |
| `base58Prefixes[PUBKEY_ADDRESS]` | `0` | `0` | **Identical** |
| `base58Prefixes[SCRIPT_ADDRESS]` | `5` | `5` | **Identical** |
| `base58Prefixes[SECRET_KEY]` (WIF) | `128` | `128` | **Identical** |
| Extended key prefixes (xpub/xprv) | `0488B21E` / `0488ADE4` | `0488B21E` / `0488ADE4` | **Identical** |
| Bech32 HRP | `be` | `bc` | **Differs** |

And for testnet/testnet4/regtest, Elektron Net's `base58Prefixes` and `bech32_hrp` (`tb` / `bcrt`) are also byte-identical to `rust-bitcoin`'s built-in `Network::Testnet`/`Network::Regtest` values — only the genesis block, magic bytes, port numbers chosen for the *P2P* layer, and pruning/consensus parameters differ, none of which `electrs` reads.

Two consequences:

1. **If strategy 2 (repurpose an existing `Network` variant, typically `Network::Bitcoin`/`Network::Testnet`) is chosen, the only override actually required is the mainnet bech32 HRP (`be` instead of `bc`).** No port override and no base58-prefix override are needed — they already match. This is a materially smaller task than "override HRP, magic bytes, and port constants," and should be reflected in whatever ADR or code comment documents the chosen strategy (see the original note below on making the choice explicit).
2. **Network magic (`pchMessageStart`) is not relevant to `electrs` at all**, and should be dropped from the override list regardless of strategy. `elektron-net-electrs` has no P2P client — `src/daemon.rs` only ever talks to `elektrond` over JSON-RPC (`getblockchaininfo`, `getblock`, `sendrawtransaction`, etc.); there is no code path anywhere in the fork that opens a peer-to-peer connection or parses a P2P message header. Magic bytes matter to the DNS seeder and to full nodes, not to this component.

Two viable strategies remain, both used by comparable altcoin electrs forks:

1. **Vendor a patched `rust-bitcoin`** with an added `ElektronNet` variant. Correct long-term, but invasive, and needs re-patching on every `rust-bitcoin` upgrade — a real maintenance cost to budget for.
2. **Repurpose an existing variant** (commonly `Network::Bitcoin` for mainnet, `Network::Testnet` for testnet) as an internal stand-in, overriding only the mainnet bech32 HRP (`be`) at the `elektron-net-electrs` config layer — per the table above, nothing else needs overriding.

Either is workable. What matters is that the choice is made **explicitly and documented in code comments** at the point of substitution — not left as an implicit side effect of copy-pasting the Bitcoin variant's config block, where a future contributor could easily mistake it for an actual Bitcoin-mainnet code path.

**Known, accepted constraint worth documenting prominently (new in v0.2):** because Elektron Net mainnet reuses Bitcoin's exact base58 version bytes, a legacy (base58, non-bech32) Elektron mainnet address is byte-for-byte indistinguishable in form from a Bitcoin mainnet address — only the bech32-encoded form (`be1...` vs `bc1...`) is visibly distinct. This is inherited from `elektron-net`'s own `chainparams.cpp` and is not something `electrs` can or should try to fix; it should simply be called out in this fork's own README/config docs so wallet integrators aren't surprised when a `1...`-style address doesn't visually identify which chain it belongs to.

### 3.4 Merkle-proof generation MUST degrade gracefully past the pruning window

`src/merkle.rs`'s `Proof::create()` itself is a pure function over already-fetched txids and does no I/O; the RPC dependency lives one layer up, in `src/electrum.rs`'s `transaction_get_merkle()`, which calls `Daemon::get_block_txids()` (`src/daemon.rs`, wrapping the `getblock` RPC) to obtain the block's transactions before building the proof. Once a block ages past the 197,280-block retention window, that `getblock` call will fail against a pruned `elektrond` node — expected and correct, not a bug to route around.

Confirmed in the current fork: today, that failure does **not** crash the server — `Call::response()` (`src/electrum.rs`) already catches any daemon RPC error generically and returns it as a JSON-RPC error object (`RpcError::DaemonError`, code `2`). What's missing is that this generic path forwards bitcoind's raw, untyped message (e.g. whatever text `elektrond` returns for "block file pruned") indistinguishably from any other RPC failure (auth error, timeout, malformed request, etc.).

**Required behavior:** `blockchain.transaction.get_merkle` (and `blockchain.transaction.id_from_pos`'s merkle branch, which follows the identical `get_block_txids` → `Proof::create` path in `transaction_from_pos()`) MUST detect this specific case and return a typed, documented "proof unavailable — block pruned" response, rather than letting bitcoind's raw error message pass through the generic `DaemonError` path. Wallet clients consuming this server need a stable way to distinguish "proof unavailable by design" from "server malfunction."

### 3.5 Steady-state indexing needs no changes for correctness

Once the bootstrap height from §3.2 is established, ordinary block-by-block indexing works exactly as upstream intends, because Elektron Net never prunes a block before it exceeds the retention window. The day-to-day indexing path, RocksDB schema, and Electrum-protocol server (`src/electrum.rs`) are reusable close to as-is — the one deliberate addition to steady-state operation is the index's own retention rule (§3.6).

### 3.6 The index MUST follow the chain's retention rule (self-contained concept)

Decided July 12, 2026: `elektron-net-electrs` MUST NOT become a de-facto archival node. Upstream electrs indexes the entire chain since genesis and never deletes; on Elektron Net that would silently undermine the chain's own "time erases your traces" guarantee — every node forgets history after 197,280 blocks, but a stock electrs sitting next to it would remember everything. (`elektron-net-mempool` already made the identical call for its own `TxIndexRepository`, which self-prunes to the same depth.)

The index therefore mirrors the chain's retention semantics one-to-one, which the `elektron-net` source guarantees on its side:

| Data | Chain (`elektrond`) | electrs index |
|---|---|---|
| Headers | kept forever — `PruneOneBlockFile()` (`src/node/blockstorage.cpp`) only clears the `BLOCK_HAVE_DATA`/`BLOCK_HAVE_UNDO` flags and deletes `blk*.dat`/`rev*.dat`; the `CBlockIndex` entries survive | header chain kept forever (upstream behavior, unchanged) |
| UTXO set | kept forever — structurally untouched by pruning, and consensus-critical on this chain (per-block MuHash attestation and checkpoint snapshots are computed over it) | **unspent entries per scripthash kept forever — exempt from index pruning** (otherwise balances would be silently wrong for old, still-unspent coins) |
| Block bodies / history | 197,280 blocks, then deleted | history/spent entries older than 197,280 blocks deleted by a periodic index-pruning job |

Two protocol-visible consequences to document for wallet integrators:

1. A scripthash's Electrum status hash changes when old history entries age out of the window (wallets simply re-fetch that address's history — harmless, but observable).
2. Wallets see at most ~137 days of transaction history; balances remain complete. This is the same statement the Wallet Integration Guideline already makes about the network — here it is enforced server-side as well.

Together with §3.2 this makes the system self-contained: a server synced from genesis and a server bootstrapped later from a UTXO snapshot converge to the identical state — UTXO set plus a rolling 197,280-block history window.

## 4. Relationship to Header-Chain Availability

Elektron Net always retains the full header chain, even though block bodies are pruned. This means chain-of-work verification for headers is unaffected by any of the above — only per-transaction merkle proofs are impacted (§3.4), not header sync itself. Confirmed directly in `src/electrum.rs`: `transaction_get_merkle()` resolves the block hash from `self.tracker.headers()` (always available) before it ever calls the daemon for the block body — the header lookup cannot be the point of failure; only the subsequent `get_block_txids()` RPC can be. This should be called out explicitly in `elektron-net-electrs`'s own documentation so downstream wallet developers do not conflate the two.

## 5. Checklist

- [ ] Fork `romanz/electrs` at a pinned commit; set up a rebase cadence against upstream for security/protocol fixes
- [ ] Replace the hard-fail pruned-node check in `src/daemon.rs::rpc_poll()` with the UTXO-snapshot bootstrap path (§3.2), sourced from `dumptxoutset` or the automatically-written checkpoint snapshot files — no new `elektrond` RPC required
- [ ] Decide and document the `Network` enum strategy (§3.3) — patched `rust-bitcoin` vs. repurposed variant; if repurposing, the only override needed is the mainnet bech32 HRP (`be`)
- [ ] Implement the graceful merkle-proof fallback in both `transaction_get_merkle()` and `transaction_from_pos()` (`src/electrum.rs`, §3.4) — **done on `integration`** (typed Electrum error, code 3)
- [ ] Implement the index retention rule (§3.6): periodic pruning of history/spent entries older than 197,280 blocks, unspent entries exempt
- [ ] Document the header-chain vs. block-body distinction (§4) prominently, so wallet integrators don't misread the trust model
- [ ] Document the shared-base58-prefix / address-ambiguity constraint (§3.3) prominently, so wallet integrators don't misread mainnet legacy addresses as chain-identifying
- [ ] Enable and test the `elektron-electrs` Docker Compose profile already present in `elektron-net-mempool`
- [ ] Hand off to wallet-client integration once a scripthash lookup, a balance query, and a recent-block merkle proof all return correct results end-to-end

## 6. Open Questions

1. ~~Should the bootstrap RPC be a new `elektrond` RPC method, or should `scantxoutset` be extended/reused as-is?~~ **Resolved in v0.2:** neither — use `dumptxoutset` (or the existing automatic checkpoint `.dat`/`.hash` snapshot files), both already producing a full AssumeUTXO-format dump; no new RPC and no extension of `scantxoutset` (which is scoped to caller-supplied descriptors, not a full-set dump) is needed. See §3.2.
2. Which `Network`-enum strategy (§3.3) should be adopted, and who owns the `rust-bitcoin` patch if option 1 is chosen? (Narrowed in v0.2: if option 2 is chosen instead, the only value needing an override is the mainnet bech32 HRP — everything else already matches Bitcoin's built-in constants.)
3. Should this fork be published publicly from the start, or developed privately until the bootstrap/merkle-proof adaptations are stable?
