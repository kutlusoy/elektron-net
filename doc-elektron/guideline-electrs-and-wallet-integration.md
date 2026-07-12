# Elektron Net — Wallet Integration Guideline

- **Version:** 0.1 (draft)
- **Date:** July 12, 2026
- **Audience:** Mobile wallet vendors, Electrum-protocol server operators, Lightning integrators of any kind
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) — `src/wallet/wallet.cpp` (`ScanUTXOSet()`/`CreditUTXOFromChain()`), `src/kernel/chainparams.cpp` — treat these as the ground truth
- **Reference explorer/indexer:** [`elektron-net-mempool`](https://github.com/kutlusoy/elektron-net-mempool) — see its `docker-compose.yml` for the pre-wired, not-yet-built `elektron-net-electrs` service profile
- **See also:** [`BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](https://github.com/kutlusoy/elektron-net/blob/main/WHITEPAPER.md)

This document is written as a general integration guideline for any wallet vendor building a non-custodial mobile wallet for Elektron Net (ELEK) — not as instructions to a specific team. Requirement levels follow common practice: **MUST** = mandatory for correct/safe operation, **SHOULD** = strongly recommended, **MAY** = optional.

---

## 1. Executive Summary

Elektron Net has no "history problem" to solve, because the network is deliberately designed so that consensus never depends on block history — it depends on a continuously attested UTXO set (MuHash, active from block 137,000; `MandatoryPruneDepth` of 197,280 blocks ≈ 137 days at a 60-second block time). The reference node wallet (`elektrond`) already implements a clean recovery path for this (`CWallet::ScanUTXOSet()`).

For a mobile, non-custodial wallet with Lightning support (internally, this layer may end up branded as the "Photon Protocol" — a naming idea under consideration, not yet decided; purely cosmetic and without any effect on the technical plan below), exactly one central piece of infrastructure is currently missing: an Electrum-protocol-compatible server (`elektron-net-electrs`). It is already referenced as a placeholder in the `elektron-net-mempool` repository but has not been built yet. Everything else — chain parameters, wallet client fork, Lightning integration — depends on this one component existing.

**Recommendation for any wallet vendor:** Electrum Wallet (or a lightweight fork of it) as the client, paired with a purpose-built `elektron-net-electrs` server that bootstraps from a UTXO snapshot rather than replaying from genesis — mirroring the network's own pruning philosophy instead of contradicting it. Lightning support additionally requires dedicated bootstrap routing nodes, regardless of which wallet is chosen, since no LN graph exists on this network yet.

---

## 2. Current State: What Already Exists

### 2.1 Network Parameters (from `src/kernel/chainparams.cpp`, mainnet)

| Parameter | Value |
|---|---|
| `pchMessageStart` (P2P magic bytes) | `0xe1 0xec 0x7a 0x6e` |
| `nDefaultPort` (P2P) | `8333` (identical to Bitcoin mainnet) |
| Bech32 HRP | `be` (addresses: `be1q…`, Taproot `be1p…`) |
| Base58 `PUBKEY_ADDRESS` | `0x00` — **identical to Bitcoin mainnet** |
| Base58 `SCRIPT_ADDRESS` | `0x05` — identical to Bitcoin mainnet |
| Base58 `SECRET_KEY` (WIF) | `0x80` — identical to Bitcoin mainnet |
| `EXT_PUBLIC_KEY` / `EXT_SECRET_KEY` | `0488B21E` / `0488ADE4` — identical (xpub/xprv) |
| Block time | 60 seconds (retarget window still 2016 blocks) |
| Halving interval | 2,102,400 blocks |
| Genesis subsidy | 5 ELEK |
| `MuhashAttestationActivationHeight` | 137,000 |
| `MandatoryPruneDepth` / `nPruneAfterHeight` | 197,280 blocks ≈ 137 days — **enforced on every node, no archival mode exists** |
| Currency unit | ELEK / lep (instead of BTC / sat) |

**Safety consequence for any wallet:** Legacy Base58 addresses (P2PKH/P2SH) are **byte-identical** to Bitcoin mainnet. A legacy Elektron Net key is also a valid Bitcoin mainnet key, and vice versa. This is intentional (the pool/faucet code notes that Elektron Net "reuses" these prefixes), but it creates a real misdirected-funds risk that any wallet UI must actively address — see §4.4.

### 2.2 Native Wallet Recovery in `elektron-net` Itself

`src/wallet/wallet.cpp` already implements a full fallback for the case where a block-based rescan is impossible due to pruning:

- **`CWallet::ScanUTXOSet()`** iterates `chain().forEachCoin()` → `ActiveChainstate().CoinsTip().ForEachUnspent()` (the current, live UTXO set) and checks `IsMine()` for each coin.
- **`CWallet::CreditUTXOFromChain()`** builds a synthetic, partial `CWalletTx` entry from a match (only the relevant `vout` is known), tagged with the coin's current block height.
- **`CWallet::MaybeRescanUTXOSetAfterSnapshot()`** automatically re-runs the scan if the wallet was loaded mid-IBD and the first pass only saw a near-empty pre-activation UTXO set.
- Triggered inside **`AttachChain()`** whenever `chain.havePruned()` prevents a normal rescan. User-facing warning: *"Wallet balances recovered from the current UTXO set. Pruned transaction history is unavailable."*

Any wallet-server component built for mobile clients should reproduce this exact model — see §3.2.

### 2.3 Explorer/Indexer: `elektron-net-mempool`

Already adapted for HRP, difficulty, and unit display (`be1`, 60s block time, ELEK/lep). Currently runs in **`MEMPOOL_BACKEND=none`** mode, talking directly to node RPC. Block-level metadata (heights, hashes, stats) is cached permanently in the explorer's own database as blocks arrive, independent of later pruning on the node.

**The critical point:** The repository's `docker-compose.yml` already pre-wires (but does not enable) an `elektron-electrs` service profile, with a note that address lookups and pruning-independent historical transaction detail require an Electrum-protocol server, and that `elektron-net-electrs` is planned as a **separate, not-yet-built repository** for this purpose. This is the central gap this guideline is organized around.

### 2.4 Mining Infrastructure (`elektron-net-pool`, `-ppool`)

Both strictly respect the attestation model: block templates are per-miner (`getblocktemplate` including the payout address), because the UTXO attestation hash is computed over the full coinbase content — shared templates would be rejected as `bad-utxo-attestation`. Not directly relevant to wallet integration, but it illustrates how strictly the network enforces attestation integrity — the same trust model any lightweight wallet server must lean on (see §3.2).

### 2.5 Seeder (`elektron-net-seeder`)

A conventional C++ DNS seeder (Bitcoin Seeder lineage) supplying peer addresses to `elektrond` nodes. Only indirectly relevant to mobile wallets (relevant only if a wallet were to use full P2P connectivity instead of a server protocol — not recommended, see §3.1).

---

## 3. Core Requirements for a Mobile, Non-Custodial Wallet

### 3.1 Chain Parameters in the Client

Any wallet client (Electrum fork or otherwise) MUST implement at minimum:

- Bech32 HRP `be` (plus, if Electrum's native Lightning implementation is used, a corresponding BOLT11 HRP for invoices)
- Base58 version bytes as in §2.1, with an explicit note that these are *intentionally* identical to Bitcoin mainnet
- Genesis hash, `pchMessageStart`, default port 8333 (only relevant if a client ever speaks P2P directly instead of a server protocol)
- A BIP32 path / `BIP44_COIN_TYPE` — it is currently unclear whether Elektron Net has registered its own SLIP-44 coin type or reuses Bitcoin's `0'`. **This must be clarified with the core protocol team before any wallet fork fixes a derivation path.**

### 3.2 Server Protocol Choice — the Central Architectural Decision

| Option | History required? | Compatible with pruning philosophy? | Effort |
|---|---|---|---|
| **ElectrumX/Fulcrum (standard)** | Yes — indexes from genesis by design, expects full history | No, direct contradiction | Low (off-the-shelf), but architecturally wrong |
| **Custom `elektron-net-electrs` (UTXO-snapshot bootstrapped)** | No — bootstraps from the current UTXO set, same as the native wallet | Yes, matches the model exactly | Medium (fork of `electrs`/ElectrumX + bootstrap logic) |
| **Esplora/REST (as commonly used with BDK/mempool.space)** | Yes, classically genesis-based as well | No, same problem as ElectrumX | Medium |
| **Neutrino/BIP157 (compact block filters)** | Partially — filters are small and could be retained even once block bodies are pruned | Yes, with extra effort (filter retention beyond the pruning window) | High (additional filter index alongside the node) |
| **Custom RPC gateway (`scantxoutset`-style, direct against `elektrond`)** | No | Yes, most consistent | Lowest on the server side, but no standard wallet speaks this natively |

**Recommendation:** Option 2. It is already the stated goal in the `elektron-net-mempool` repository, and the native recovery logic in §2.2 shows exactly the bootstrap mechanism such a server should adopt:

> Instead of running `elektron-net-electrs` from genesis onward (which the mempool README's current wording would require — "indexing must start before blocks age past the prune horizon, otherwise that history is unrecoverable"), the server should use the **same `forEachCoin()`/`ScanUTXOSet()` bootstrap** as the native wallet: build an initial scripthash-to-UTXO map once from the current UTXO set, and only accumulate real history **from that point forward**. This eliminates the unrecoverable-data risk described in the README entirely, and makes cold-start for new server operators exactly as trivial as it already is for new nodes.

For transaction history predating a server's own bootstrap point, and for merkle proofs (`blockchain.transaction.get_merkle`) of already-pruned blocks, the same design choice as the native wallet applies: **unavailable by design**, communicated to the user in the client rather than surfaced as an error.

**Important, but not a problem:** Header chains are, per the network's design, always retained in full (never pruned). This means standard Electrum SPV header verification (proof-of-work chain checking via `blockchain.py`/header sync) works **unmodified, with no adaptation needed** — only merkle inclusion proofs for individual old transactions are affected, not chain integrity itself.

### 3.3 Lightning Integration Options

**Option A — Electrum's built-in Lightning (recommended)**
Electrum has shipped its own Python-based Lightning implementation since version 4 (trampoline routing rather than full graph routing, to keep mobile clients lightweight). Advantage: no second infrastructure component (no separate LND/CLN needed) — a single server type (`elektron-net-electrs`) serves both on-chain and Lightning funding transactions.

**Option B — Zeus + a dedicated LND/CLN fork**
Requires an additional chain-param-patched LND/CLN fork that reproduces the `ScanUTXOSet()` mechanism inside `btcwallet`'s or CLN's wallet layer. Functionally equivalent, but two parallel components (Electrum server *and* LND/CLN fork) instead of one.

**Regardless of wallet choice:** No Lightning network graph exists on Elektron Net yet. At least one, ideally several, permanently online, well-connected routing/trampoline nodes with initial liquidity are required — otherwise no payment path can exist between two arbitrary wallets, no matter how good the wallet software itself is. This is a pure bootstrap problem, not a software problem, and applies identically to any wallet vendor.

### 3.4 Watchtower / Liveness

The UTXO attestation model fully solves balance recovery and long-term auditability, but it does **not** solve real-time reaction to a breach within the CSV timeout window. A continuously running watchtower service remains necessary regardless of wallet or server choice, and any wallet vendor offering channel management should either run one or integrate with a third-party one.

---

## 4. Evaluation: Electrum Wallet as a Candidate

### 4.1 Why Electrum Fits Well

- **Established precedent:** there is a well-known pattern for Bitcoin Core forks to fork Electrum as a client (e.g. `electrum-ltc` for Litecoin) — chain parameters live in a central `constants.py`-style file plus `servers.json`/`checkpoints.json`, without deep changes to the rest of the codebase.
- **Open source, actively maintained, light enough for mobile** (Kivy UI on Android; iOS exists but with some limitations).
- **Server URL is freely configurable** — matches the requirement that branding is unimportant as long as server address and HRP are configurable.
- **Built-in Lightning** removes the need for a second infrastructure component (see 3.3, Option A).
- **Header-sync model fits seamlessly**, since Elektron Net always retains the full header chain (see 3.2).

### 4.2 What Must Be Adapted

1. Chain-parameter fork (HRP `be`, Base58 bytes, genesis hash, `pchMessageStart`, and a dedicated BOLT11 HRP for LN invoices if applicable)
2. **`elektron-net-electrs` must be built** (see 3.2) — this is the actual prerequisite; without it, an Electrum fork has no server to talk to
3. Merkle-proof handling must degrade gracefully instead of failing when a requested historical transaction falls outside the 197,280-block window
4. The trust model must be documented clearly: since classic SPV merkle proofs for older data no longer exist, the client relies more heavily on the network-wide attestation model than on per-transaction proofs. This is not a security regression relative to the consensus model itself (which is built on exactly this foundation), but it should be named explicitly in code comments and, where relevant, in the UI, so it is never mistaken for classic Bitcoin SPV guarantees.

### 4.3 What Does NOT Need To Be Built

- No second LND/CLN fork (if Option A is chosen for Lightning)
- No snapshot-diff RPC (an idea raised earlier in this project's design discussions but not required, since consensus never depends on history in the first place)
- No changes to the existing `ScanUTXOSet()` mechanism itself — it should remain as-is and serve as the reference implementation for the electrs bootstrap

### 4.4 Safety/UX Requirement: Base58 Collision with Bitcoin Mainnet

Because legacy addresses and WIF keys are byte-identical to Bitcoin mainnet, any wallet fork MUST:

- Default to showing and generating **only Bech32 receive addresses (`be1q…`/`be1p…`)**, never legacy addresses by default
- Show an explicit warning when importing a legacy key or address ("this address is also valid on Bitcoin mainnet — risk of confusion")

This should be treated as a **mandatory review item** for any wallet integration, not a nice-to-have.

---

## 5. Comparison Table: Electrum Fork vs. Zeus+LND/CLN Fork

| Criterion | Electrum fork (recommended) | Zeus + LND/CLN fork |
|---|---|---|
| Additional server component | One `elektron-net-electrs` | One patched LND/CLN, plus a separate explorer backend |
| Lightning implementation | Built-in (trampoline) | External (LND/CLN, full graph routing) |
| Rescan/recovery model | Must be reproduced in the new electrs server | Must be reproduced in btcwallet (LND) or CLN's wallet layer |
| Existing fork precedent | Yes (electrum-ltc and others) | Partial (LND has supported pruned bitcoind backends since v0.13, but not "network-wide pruned with no fallback peer") |
| Overall effort | Medium (one new component) | Medium–high (one new component plus deeper changes to a third-party codebase) |
| Mobile maturity | Android solid, iOS more limited | Android + iOS (Zeus), but the app itself carries no chain knowledge — all adaptation burden sits in the backend |

---

## 6. Implementation Plan (Phased, for Any Integrating Team)

**Phase 0 — Clarification (before any code)**
- [ ] SLIP-44/BIP44 coin type for Elektron Net must be decided (register a dedicated number, or deliberately reuse Bitcoin's `0'`?)
- [ ] A dedicated BOLT11 HRP for Lightning invoices should be decided (if Option A is chosen)
- [ ] Option A (Electrum-native Lightning) vs. Option B (Zeus+LND/CLN) should be finalized before further work begins

**Phase 1 — `elektron-net-electrs`**
- [ ] A base for forking must be chosen: `electrs` (romanz) or ElectrumX/Fulcrum
- [ ] A bootstrap mode must be built: a one-time UTXO-set scan (mirroring `forEachCoin()`/`ScanUTXOSet()`) instead of a genesis replay
- [ ] A merkle-proof fallback must be implemented for requests outside the pruning window
- [ ] The Docker Compose profile in `elektron-net-mempool` should be enabled and tested against the new service

**Phase 2 — Wallet Client Fork**
- [ ] Electrum should be forked, with a `constants.py` equivalent populated from §2.1
- [ ] The default server list must point at `elektron-net-electrs` instances
- [ ] Bech32 default addresses must be enforced, with the legacy warning from §4.4 implemented
- [ ] The Lightning module should be tested (trampoline configuration, BOLT11 HRP from Phase 0)

**Phase 3 — Lightning Network Bootstrap**
- [ ] At least one to two permanently online, well-reachable trampoline/routing nodes with initial liquidity must be set up
- [ ] A watchtower service should be added as an additional component to the existing Docker stack

**Phase 4 — Testing & Rollout**
- [ ] End-to-end testing on testnet is required (testnet chain parameters must be checked for HRP collision with Bitcoin testnet's `tb`; a dedicated testnet HRP should be assigned if needed)
- [ ] A security review of §4.4 (Base58 collision) in particular must be conducted
- [ ] Documentation for wallet vendors should be produced (warning copy, pruning behavior, attestation model), consistent with the existing `doc-elektron/` documents

---

## 7. Open Questions for the Core Protocol Team

1. Should Option A (Electrum-native Lightning) or Option B (Zeus+LND/CLN) be pursued, or should both be evaluated in parallel?
2. Is there already a preference for registering a dedicated SLIP-44 coin type?
3. Who will own the `elektron-net-electrs` fork — an internal team or an external contractor?
