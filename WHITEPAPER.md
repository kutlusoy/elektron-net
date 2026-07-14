# Elektron Net — Whitepaper
**Version 4.0.0 | Author: Ali Kutlusoy | Graz, Austria, 2026**<sup>¹</sup>

---

## Abstract

Elektron Net is a minimal, focused fork of Bitcoin Core. It preserves Bitcoin's proven proof-of-work consensus, emission schedule, and network architecture in their entirety. Two deliberate protocol changes are introduced:

1. **Block time reduced to 60 seconds** — faster confirmation latency without altering the economic model.
2. **Mandatory 137-day pruning** — transaction history is mathematically erased after α⁻¹ days, leaving only headers, checkpoints, and the current UTXO set.

Every block carries an on-chain UTXO attestation (37 bytes); full snapshot *files* are written only every 137 days. See §4.3 and [`doc-elektron/BITCOIN_CORE_DIFF.md`](doc-elektron/BITCOIN_CORE_DIFF.md) for the complete implementation diff vs Bitcoin Core.

Everything else remains Bitcoin Core. The SHA-256d proof-of-work, the 21-million supply cap, the halving rhythm, and the Nakamoto consensus are untouched.

---

## 1 — Why

### The Lydian Elektron

Around 600 BC, the Lydians in Asia Minor struck the first coins in the world. They were made of Elektron – a naturally occurring alloy of gold and silver. These coins fundamentally changed civilisation: for the first time, a person could trade without giving their name. Value became transferable, divisible, anonymous.

The Lydian Elektron coins were the first decentralised currency in history. No king had to be present. No scribe had to record the exchange. The value lay in the material itself – in the mathematics of the alloy.

2,600 years later, the world faces the same problem, solved with the same means: mathematics instead of trust.

### 1.0 Philosophy & Stoic Stance

The Stoic masters — Seneca, Epictetus, Marcus Aurelius — taught a single, devastating discipline: *distinguish between what is in your power and what is not*.

Your thoughts. Your keys. Your next breath. These are yours.

The opinions of strangers. The records of yesterday. The judgment of history. These are not.

Bitcoin’s ledger is a monument to the past. Every transaction, every mistake, every late-night payment is etched in cryptographic stone — forever. That immutability is magnificent, but it is also a chain. A chain that grows heavier with every block. A chain that binds you to a history you never chose to carry.

Elektron Net rejects that chain. We do not deny history; we simply refuse to be its prisoner. The network remembers only what is necessary for consensus — the UTXO set, the headers, the checkpoints. The rest is released. Not hidden behind encryption. Not buried in fine print. Erased. Gone. As if it never was.

This is the **Stoic protocol**: secure what you can control, let go of what you cannot.

> *"You cannot stop the wind, but you can adjust your sails. You cannot erase the world’s memory, but you can build a network that forgets by design."*

### 1.1 The Pocket Philosophy

Elektron Net is deliberately built around a simple, ancient human experience: When you put on your pants in the morning and reach into your pocket, you immediately know three things:

- How much money you have right now — not last week, not last month.
- That this money is yours — because it is your pocket.
- You do not need to remember where each coin came from, what you bought yesterday, or who paid you last year. The past is irrelevant. Only the present matters.

This everyday reality is the guiding metaphor for the entire protocol.

Your UTXO set is your pocket. It contains exactly what you possess in this moment. Nothing more, nothing less. It is the only permanent data structure in the network.

137 days of pruning is natural forgetting. Just as you do not keep receipts from five months ago in your pocket, the network mathematically erases all transaction history after exactly 137 days (α⁻¹). Not because someone ordered it, but because the protocol makes it impossible to keep. No subpoenas, no archives, no eternal digital footprint.

- **Recovery is finding your pocket again.** Your 24-word seed is enough. Scan the current UTXO set, recognise what belongs to you, and reclaim it — no history required.

This is not merely privacy engineering. It is the **exercise of the worldwide right to be forgotten, by design**. No administrator can override it. No majority vote can extend it. The will of the user is encoded in consensus: after 137 days, the data is gone. Not because a company decided to comply, but because mathematics and physics demand it.

For a comprehensive legal overview of the right to be forgotten across jurisdictions, see [`right-to-be-forgotten.md`](right-to-be-forgotten.md).

> *"Mathematics secures your money. Time erases your traces. You own the moment."*

### 1.2 History & The Problem

In 2009, Bitcoin emerged from the wreckage of institutional failure — a way to move value across the world without asking permission, without signing papers, without trusting a bank. Its invention of the blockchain created something unprecedented: an eternal, auditable, uncensorable record. In those early years, that permanence felt like liberation.

But permanence has a shadow.

As the chain grew, so did the surveillance surface. Chain-analysis firms mapped every wallet. State actors traced every flow. Data brokers packaged your financial life into sellable profiles. The very transparency that made Bitcoin trustworthy became the cage that made its users transparent. What began as liberation from banks quietly became, for millions, a panopticon they could never leave.

The European Union recognized this tension and codified the **Right to be Forgotten** in the GDPR. Other jurisdictions followed. Yet on a public blockchain, legal rights are paper tigers without technical enforcement. A court can order a search engine to delist a link; no court on Earth can order a million distributed nodes to forget a block they were designed to keep forever.

Elektron Net solves this at the root.

The 137-day pruning window is not a setting you can toggle. It is a structural guarantee that the network *cannot* retain what it has been ordered to forget — because the protocol itself makes retention impossible. Not difficult. Not illegal. Impossible.

An immutable global ledger of all financial history creates three inevitable catastrophes:

- **Permanent surveillance surface** — every payment you ever made is traceable forever, by anyone with a node and a database.
- **Unbounded storage growth** — nodes must carry the full weight of decades of data, pricing out ordinary users and centralising power.
- **Institutional risk** — data that exists can be subpoenaed, analysed, weaponised, and sold. If it is stored, it will be exploited.

Bitcoin was built for institutional resilience. Elektron Net is built for human sovereignty — and for the ancient, inalienable right of every person to be forgotten.

---

## 2 — Technical Foundation

**Base:** Bitcoin Core (C++20 fork). All P2P, storage, script, wallet, and consensus infrastructure is inherited directly.

| Component | Source | Note |
|---|---|---|
| Language | Bitcoin Core | C++20 |
| Consensus | Bitcoin Core | SHA-256d PoW, Nakamoto longest-chain rule |
| Difficulty Adjustment | Bitcoin Core | Every 2,016 blocks (unchanged) |
| Script Engine | Bitcoin Core | OP_CODES unchanged |
| P2P Network | Bitcoin Core | TCP/IP, addr relay, DoS protection |
| Wallet | Bitcoin Core | BIP-32/39/44, descriptor wallets, Bech32m |
| Storage | Bitcoin Core | LevelDB / RocksDB |
| RPC / P2P | Bitcoin Core | Port **8332** / **8333** (unchanged) |

**Changes to Bitcoin Core:**

| Parameter | Bitcoin | Elektron Net |
|---|---|---|
| Block time | 10 minutes | **60 seconds** |
| Blocks per day | 144 | **1,440** |
| Retarget interval | 2,016 blocks (2 weeks) | **2,016 blocks (1.4 days)** |
| Pruning | Optional, user-defined | **Mandatory, 137 days** |

All other consensus rules, opcodes, signature schemes (ECDSA, Schnorr/Taproot), and network behaviour are preserved exactly.

---

## 3 — The 60-Second Block

### 3.1 Rationale

A 10-minute interval was conservative in 2009. In 2026, network propagation, hardware, and bandwidth make 60-second blocks practical without materially increasing orphan rates, provided difficulty retargeting remains responsive.

### 3.2 Economic Preservation

Reducing block time by 10× would inflate supply by 10× if the block reward were unchanged. The emission schedule is therefore scaled proportionally:

| Parameter | Value |
|---|---|
| Block time | 60 seconds |
| Blocks per day | 1,440 |
| Genesis block reward | **5.00 Elek** |
| Halving interval | 2,102,400 blocks (4 years) |
| Maximum supply | **21,000,000 Elek** |

| Period | Block Reward | Per Day | Cumulative |
|---|---|---|---|
| Year 1–4 | 5.00 Elek | 7,200 | 10,512,000 |
| Year 5–8 | 2.50 Elek | 3,600 | 15,768,000 |
| Year 9–12 | 1.25 Elek | 1,800 | 18,396,000 |
| … | … | … | … |
| ~Year 115 | ≈0.00000 | 0 | 21,000,000 |

**No pre-mine. No airdrop. No founder allocation.** Every Elek is earned by producing valid proof-of-work.

### 3.3 Difficulty Adjustment

Elektron Net retargets difficulty every 2,016 blocks — identical to Bitcoin
Core, just compressed into 1.4 days instead of two weeks by the faster block
time. No other consensus-level difficulty mechanism is active on mainnet
today.

> **Historical note.** From genesis until block height 150,000, mainnet also
> ran a second mechanism, *Stoic Awakening*: if more than 120 seconds passed
> since the last block, the next block could be mined at minimum difficulty,
> intended as a liveness guarantee against sudden hashrate loss. Live
> operation showed it firing far more often than intended under ordinary,
> stable hashrate — and interacting badly with the standard retarget
> calculation in a way that could depress difficulty for days at a time. It
> was retired at height **150,000** (`Consensus::Params::StoicAwakeningEndHeight`).
> The full technical account, including the live data that led to the
> decision, is kept at
> [`doc-elektron/CHANGELOG-stoic-awakening-retirement.md`](doc-elektron/CHANGELOG-stoic-awakening-retirement.md)
> — a stoic protocol records what didn't work, it doesn't erase it.

---

## 4 — 137-Day Pruning: The Forgetting

### 4.1 Core Principle

Elektron Net does not store transaction history indefinitely. Full block data (transactions, inputs, outputs) is deleted after exactly 137 days — α⁻¹, the inverse fine-structure constant.

**There is no grace period.** Pruning starts at the first checkpoint (`nPruneAfterHeight = 197,280` blocks on mainnet). Unlike earlier designs that delayed pruning until 274 days (`2 × MANDATORY_PRUNE_DEPTH`), v4.0 enforces the 137-day window from the first checkpoint onward. User `-prune=<GB>` is ignored; retention is depth-based only.

This is structural impossibility, not policy:
- Every node deletes simultaneously — no node can retain data without forking onto a separate chain.
- No company can be subpoenaed for records that no longer exist.
- No hacker can breach a database that has been mathematically erased.

The 137-day window enforces the **right to be forgotten** as a protocol invariant. It is not a service offered to users; it is a property of the network demanded by them. The user’s will is not expressed through a privacy toggle or a terms-of-service checkbox. It is expressed through hash power, through node operation, through the consensus rules themselves. To run Elektron Net is to vote for forgetting. To mine Elektron Net is to execute that vote.

#### 4.1.1 Express Declaration of Will

By choosing Elektron Net, every user makes an **express, informed, and irrevocable declaration of will**: they affirmatively exercise their right to be forgotten. This blockchain was selected precisely because it transforms a legal claim — recognised across jurisdictions — into a protocol guarantee. No petition is required. No controller must be persuaded. The right is exercised automatically, by mathematics, every 137 days.

The following jurisdictions explicitly recognise this right in law:

| Jurisdiction | Legal Instrument | Relevant Article / Paragraph |
|---|---|---|
| 🇪🇺 European Union | GDPR (Regulation EU 2016/679) | Art. 17 (right to erasure), Art. 25 (data protection by design) |
| 🇬🇧 United Kingdom | UK GDPR / Data Protection Act 2018 | Art. 17, § 1 Rehabilitation of Offenders Act 1974 |
| 🇧🇷 Brazil | LGPD (Law No. 13,709/2018) | Art. 5(XVI), Art. 15, Art. 16, Art. 18(IV), Art. 18(VI) |
| 🇨🇦 Canada | PIPEDA / CPPA Bill C-27 / Quebec Law 25 | Principle 5, Principle 9, § 55 (proposed), § 28.1 (Quebec) |
| 🇰🇷 South Korea | PIPA (Act No. 10465) | Art. 36, Art. 37, Art. 39-3 |
| 🇦🇷 Argentina | PDPA (Law No. 25,326) | Art. 16, Art. 43 (habeas data) |
| 🇵🇭 Philippines | Data Privacy Act (RA 10173) | § 16(d) |
| 🇯🇵 Japan | APPI (Act No. 57/2003) | Art. 24, Art. 33, Art. 34, Art. 35 |
| 🇹🇷 Turkey | KVKK (Law No. 6698) | Art. 7, Art. 11(e), Art. 11(f) |
| 🇷🇸 Serbia | Law on Personal Data Protection (No. 87/2018) | Art. 25, Art. 30, Art. 31 |
| 🇺🇸 United States | CCPA / CPRA (California) | Cal. Civ. Code § 1798.105 |
| 🇺🇸 United States | Virginia CDPA | § 59.1-577(A)(4) |
| 🇺🇸 United States | Colorado CPA | C.R.S. § 6-1-1306(1)(d) |
| 🌐 International | UDHR (UN, 1948) | Art. 12 |
| 🌐 International | ICCPR (UN, 1966) | Art. 17 |
| 🌐 International | ECHR (Council of Europe) | Art. 8 |
| 🌐 International | Convention 108+ (CoE, 2018) | Art. 9 |

For the full legal overview, see [`right-to-be-forgotten.md`](right-to-be-forgotten.md).

**This is not passive consent. This is active, informed, pre-emptive invocation of a right recognised in law across more than 50 jurisdictions worldwide.**

### 4.2 What Each Node Stores

| Retention | Data |
|---|---|
| **Permanent** | Genesis block header (80 bytes) |
| | All block headers (80 bytes each, chain integrity) |
| | UTXO set (current unspent outputs) |
| | On-chain UTXO attestations (37 bytes per block in coinbase) |
| | On-disk UTXO snapshot files (one per 137-day checkpoint) |
| **137 days, then deleted** | Full transaction content |
| | Input/output scripts and amounts |
| **Never stored** | User identity, IP mappings, transaction graphs |

### 4.3 UTXO Attestation & Checkpoint Snapshots

Elektron Net separates two concerns that are often conflated:

1. **Continuous integrity** — every block must cryptographically bind the UTXO set.
2. **Bootstrap efficiency** — every 137 days, a full snapshot file is written for new nodes.

This is deliberate: a 37-byte `OP_RETURN` per block is not data waste; shipping a multi-gigabyte snapshot file every block would be.

#### 4.3.1 Per-Block UTXO Attestation (Consensus)

**Every block at height > 0** must carry a UTXO attestation in the coinbase:

- Format: `OP_RETURN <height(4 bytes)> <UTXO set hash(32 bytes)>`.
- The hash is `HASH_SERIALIZED` of the UTXO set computed *after* all transactions in that block are connected.
- Miners obtain the required output from `getblocktemplate` via `coinbase_required_outputs` (alongside the witness commitment).
- External miners that omit the attestation produce invalid blocks; nodes reject them with `missing-utxo-attestation` or `bad-utxo-attestation`.
- `OP_RETURN` outputs are not added to the UTXO set, so the attestation does not create a hash feedback loop.

This prevents a malicious miner from silently diverging the UTXO state between checkpoints. The chain carries a continuous, verifiable UTXO trail — not merely a checkpoint every 137 days.

**Cost:** each connected block triggers one `ComputeUTXOStats` pass (~1,440 times per day at 60s spacing). This is the price of continuous on-chain UTXO accountability.

#### 4.3.2 Checkpoint Snapshots (Every 137 Days)

After 137 days, block *files* are gone. New nodes need a way to trust the UTXO set without downloading 197,280 blocks that no peer retains.

**On-chain checkpoint blocks** (height divisible by 197,280):

- Use the same attestation format as every other block.
- Nodes log these as *checkpoints*; validation is identical, only log verbosity differs.

**Automatic snapshot files on disk:**

- Written **only** when a checkpoint block is accepted (`WriteAutomaticSnapshot`).
- Path: `<datadir>/snapshots/<height>-<blockhash>.dat` plus a `.hash` sidecar.
- Uses Bitcoin Core's AssumeUTXO serialization format (metadata + coins).
- Snapshots are preserved even when block files are pruned.
- Obsolete snapshot files from earlier checkpoints are deleted automatically.

**P2P Snapshot Transfer:**
- New nodes advertise `NODE_NETWORK_LIMITED` because no node stores blocks older than 137 days.
- Messages:
  - `getutxosnapshot` — request snapshot metadata for a checkpoint block hash.
  - `utxosnapshot` — response with checkpoint height, block hash, and UTXO set hash.
  - `getsnapshotdata` — request a chunk of snapshot file data (offset + length).
  - `snapshotdata` — response with the requested file chunk.
- This allows new nodes to download the UTXO snapshot directly from peers, chunk by chunk, without relying on external file distribution.

**IBD vs snapshot bootstrap (`awaiting_snapshot_bootstrap`):**

While the sync gap to the target checkpoint is **≤ 197,280 blocks**, the node downloads blocks normally — pruned peers still retain that window. Historical block download is skipped only when the gap **exceeds** `MANDATORY_PRUNE_DEPTH` or a local snapshot (`.dat` + `.hash`) is already present. This prevents fresh nodes from stalling on blocks no peer can serve, without blocking normal IBD inside the retention window.

**Bootstrap for a new node:**

*Before the first checkpoint (height < 197,280):* sync behaves like Bitcoin Core — headers and blocks from genesis.

*After the first checkpoint exists:*

1. Sync headers from any peer (headers are permanent and tiny).
2. Identify the most recent checkpoint block in the headers chain.
3. Request `getutxosnapshot` from peers advertising `NODE_SNAPSHOT` (only peers with both `.dat` and `.hash` advertise this bit).
4. Download the snapshot file via `getsnapshotdata` / `snapshotdata` chunks (1 MB chunks; 30-minute stall timeout with automatic retry).
5. Download completes only when both the `.dat` file **and** the `.hash` sidecar exist; peers without a sidecar do not offer snapshots.
6. Activation (`MaybeActivateAutomaticSnapshot`) requires the `.hash` sidecar, verifies it against the on-chain coinbase attestation when the checkpoint block is on disk, and calls `PopulateAndValidateSnapshot` with `expected_utxo_hash` so deserialized content must match the sidecar hash.
7. Sync the remaining blocks from the checkpoint to the current tip (at most 197,280 blocks).

No trusted third party. No manual file download. No full historical sync after the first checkpoint. The entire bootstrap is peer-to-peer and cryptographically verified at three layers: sidecar hash, on-chain attestation, and post-load content hash. See [`doc-elektron/BITCOIN_CORE_DIFF.md`](doc-elektron/BITCOIN_CORE_DIFF.md) §2.3 and [`doc-elektron/AUDIT_PRUNING_SNAPSHOT.md`](doc-elektron/AUDIT_PRUNING_SNAPSHOT.md) for the full security model.

#### 4.3.3 Mining Template Contract

`getblocktemplate` exposes `coinbase_required_outputs`: an array of outputs the miner **must** include in the coinbase (witness commitment + UTXO attestation). The reference miners in `mining/miner.py` and `mining/miner.cpp` consume this field. Pool operators integrating Stratum must follow [`doc-elektron/mining-pool-integration.md`](doc-elektron/mining-pool-integration.md). Any mining software that ignores `coinbase_required_outputs` will produce blocks rejected by the network.

### 4.4 Storage Projection

| Component | Year 1 | Year 5 | Year 10 |
|---|---|---|---|
| UTXO Set | ~50 MB | ~1–2 GB | ~3–6 GB |
| Block Headers | <1 MB | ~12 MB | ~24 MB |
| Checkpoints | <1 MB | <1 MB | <1 MB |
| **Total permanent storage** | **~100 MB** | **~2–3 GB** | **~5–8 GB** |

Compared to Bitcoin's unbounded growth, this is a **~100× reduction** after 10 years.

### 4.5 Wallet Recovery Without History

Recovery follows the **Pocket philosophy**: your seed finds your pocket (the current UTXO set), not your receipts.

1. Enter 24-word BIP-39 seed.
2. Derive all wallet descriptors (BIP-44/84/86).
3. On wallet load, if pruned block history is unavailable for the wallet's last-synced height, the node runs **`ScanUTXOSet`** automatically (`CWallet::AttachChain` → `Chain::forEachCoin` → `CreditUTXOFromChain`).
4. Outputs matching derived addresses are credited; balance is spendable immediately.
5. **Transaction history before the 137-day pruning window is not recoverable** from the network — by design. `rescanblockchain` beyond the prune height still fails; reload the wallet to trigger UTXO scan.

This is built into the node; no manual UTXO download or `-reindex` is required on pruned nodes. Wallet vendors and integrators should read [`doc-elektron/BITCOIN_CORE_DIFF.md`](doc-elektron/BITCOIN_CORE_DIFF.md) §2.6 and §9.3.

---

## 5 — Quantum Security

Elektron Net does not introduce novel post-quantum primitives. Instead, it amplifies the protections already present in Bitcoin's design and accelerates them through the 60-second finality window.

### 5.1 Hash-Commitment at Rest

Standard Bech32m / P2WPKH / P2TR addresses publish only a hash of the public key. A quantum computer cannot reverse BLAKE3 or SHA-256 to recover the key. Coins that have not been spent are **quantum-safe at rest**.

### 5.2 The 60-Second Exposure Window

When a transaction is broadcast, the public key appears in the mempool:

```
t = 0s:   Transaction broadcast — public key visible.
t ≤ 60s:  Miner includes transaction in block.
t > 60s:  Block confirmed — UTXO spent — quantum computer is too late.
```

To steal funds, a quantum attacker would need to:
1. Intercept the transaction from the mempool.
2. Run Shor's algorithm to derive the private key.
3. Craft a conflicting transaction with a higher fee.
4. Out-propagate the honest network.

All within **60 seconds**. Error-correction overhead for a cryptographically-relevant quantum computer makes this window effectively unexploitable.

### 5.3 Pruning as a Harvest Limit

After 137 days, all spent outputs and their revealed public keys are pruned. A future quantum adversary cannot harvest historical keys from the chain. The attack surface is limited to the current mempool window.

### 5.4 Future-Proofing

If a credible quantum threat emerges, the network can soft-fork to post-quantum signature schemes (e.g., CRYSTALS-Dilithium, Falcon, SPHINCS+) through Bitcoin's established upgrade mechanisms. No hardcoded cryptography is changed today; the protocol retains full upgrade flexibility.

---

## 6 — Network Parameters

| Category | Parameter | Value |
|---|---|---|
| **Consensus** | Algorithm | SHA-256d Proof-of-Work |
| | Block time | 60 seconds |
| | Difficulty retarget | Every 2,016 blocks |
| | Finality | Probabilistic (Nakamoto) |
| **Economy** | Max supply | 21,000,000 Elek |
| | Genesis reward | 5.00 Elek |
| | Halving interval | 2,102,400 blocks (4 years) |
| | Fee model | Market-based, 100 % to miner |
| **Privacy** | Pruning window | 137 days (197,280 blocks) |
| | Address format | Bech32m (`be1q...` / `be1p...`) |
| | Default output type | P2WPKH / P2TR (Taproot) |
| **Node** | Minimum storage | ~100 MB (Year 1) |
| | Full node storage | ~5–8 GB (Year 10); GUI shows measured on-disk usage |
| | Pruning | Mandatory; `-prune` size ignored; `nPruneAfterHeight = 197,280`; no 274-day grace |
| | RPC / P2P ports | 8332 / 8333 (same as Bitcoin Core) |
| | RAM (full node) | 4–8 GB |
| **P2P** | New messages | `getutxosnapshot`, `utxosnapshot`, `getsnapshotdata`, `snapshotdata` |
| | New service bit | `NODE_SNAPSHOT` (1 << 12) |
| **Protocol** | `PROTOCOL_VERSION` | 70017 |
| | `MIN_PEER_PROTO_VERSION` | 70017 (from genesis) |

---

## 7 — Conclusion

Elektron Net is Bitcoin Core with surgical modifications:

1. **60-second blocks** — faster payments, preserved economics.
2. **137-day pruning** — privacy by mathematics, storage bounded forever.
3. **Per-block UTXO attestation** — continuous UTXO integrity; snapshot *files* only every 137 days.

No new trust models. No administrator keys. No pre-mine.

A fourth mechanism, *Stoic Awakening*, ran from genesis through block
150,000 and was retired after real-world operation showed it destabilizing
difficulty rather than protecting it — see §3.3 and
[`doc-elektron/CHANGELOG-stoic-awakening-retirement.md`](doc-elektron/CHANGELOG-stoic-awakening-retirement.md).
A stoic protocol owns its outcomes, not just its intentions.

The same SHA-256d proof-of-work. The same 21-million cap. The same Nakamoto consensus.

> *"Mathematics secures your money. Time erases your traces. You own the moment."*

---

<sup>¹</sup> *The network is young and still under active development; it is not yet intended for material financial use. Building from the latest source is recommended until a stable release. For private experiments, run `mining/mine_genesis.py` to generate your own `genesis_results.txt` and test with that genesis before joining the public chain.*

**License: MIT**
