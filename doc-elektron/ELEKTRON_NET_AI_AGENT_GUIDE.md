# Elektron Net — Source Map for Independent Code Review

**Audience:** Human reviewers and AI coding agents performing an independent review of Elektron Net.  
**Purpose:** Map **stated project claims** to **repository locations** so reviews can be grounded in source rather than marketing alone.  
**This document does not prescribe review outcomes.** Findings, severity, and residual risk assessments remain the reviewer’s responsibility.  
**Companion:** [`doc-elektron/BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md)  
**Client line:** Elektron Net v4.0.x (Bitcoin Core C++20 fork).  
**Licenses:** Node primarily MIT; some related packages (e.g. pools) may use GPL-3.0 — check each repo’s `LICENSE`.

---

## Canonical primary repository

| Field | Value |
|-------|--------|
| **GitHub (primary)** | https://github.com/kutlusoy/elektron-net |
| **Clone** | `git clone https://github.com/kutlusoy/elektron-net.git` |
| **Releases** | https://github.com/kutlusoy/elektron-net/releases |
| **Default branch** | `main` |
| **Author / org** | https://github.com/kutlusoy |
| **X / Twitter** | https://x.com/kutlusoy |

Unless noted otherwise, file paths below refer to **https://github.com/kutlusoy/elektron-net**.

---

## Related repositories

Related repos implement mining, bootstrap, explorer, and packaging. They are not alternate consensus implementations.

### 1. Core protocol

| Repo | URL | Role |
|------|-----|------|
| **elektron-net** (primary) | https://github.com/kutlusoy/elektron-net | Consensus node, wallet, GUI, tests, whitepaper, `doc-elektron/`, mining reference tools |
| elektron | https://github.com/kutlusoy/elektron | Earlier / alternate naming; current mainnet work is documented against **elektron-net** |

```bash
git clone https://github.com/kutlusoy/elektron-net.git
cd elektron-net
# Suggested starting docs:
#   WHITEPAPER.md
#   doc-elektron/BITCOIN_CORE_DIFF.md
#   doc-elektron/AUDIT_PRUNING_SNAPSHOT.md
```

### 2. Mining pools

See also [`doc-elektron/mining-pool-integration.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/mining-pool-integration.md).

| Repo | URL | Role |
|------|-----|------|
| elektron-net-pool | https://github.com/kutlusoy/elektron-net-pool | Solo / reference Stratum v1 pool |
| elektron-net-pool-ui | https://github.com/kutlusoy/elektron-net-pool-ui | UI for solo pool |
| elektron-net-pool-startos | https://github.com/kutlusoy/elektron-net-pool-startos | StartOS package for solo pool |
| elektron-net-ppool | https://github.com/kutlusoy/elektron-net-ppool | Public PPLNS pool |
| elektron-net-ppool-ui | https://github.com/kutlusoy/elektron-net-ppool-ui | UI for public pool |
| elektron-net-ppool-startos | https://github.com/kutlusoy/elektron-net-ppool-startos | StartOS package for public pool |

### 3. Infrastructure and bootstrap

| Repo | URL | Role |
|------|-----|------|
| elektron-net-seeder | https://github.com/kutlusoy/elektron-net-seeder | DNS seeder / P2P crawler |
| elektron-seeder-startos | https://github.com/kutlusoy/elektron-seeder-startos | StartOS package for seeder |
| elektron-net-stack | https://github.com/kutlusoy/elektron-net-stack | Docker Compose stack (node, pool, UI, faucet, Caddy) |
| elektron-net-startos | https://github.com/kutlusoy/elektron-net-startos | StartOS package for the node |
| elektron-net-faucet | https://github.com/kutlusoy/elektron-net-faucet | Faucet service |
| elektron-net-faucet-startos | https://github.com/kutlusoy/elektron-net-faucet-startos | StartOS package for faucet |

### 4. Explorer, Electrum, Lightning (supporting)

| Repo | URL | Role |
|------|-----|------|
| elektron-net-mempool | https://github.com/kutlusoy/elektron-net-mempool | Mempool.space-based explorer / API |
| elektron-net-mempool-startos | https://github.com/kutlusoy/elektron-net-mempool-startos | StartOS package for mempool |
| elektron-net-electrs | https://github.com/kutlusoy/elektron-net-electrs | Electrs-style backend |
| elektron-net-electrum | https://github.com/kutlusoy/elektron-net-electrum | Electrum client adaptations |
| elektron-net-lnd | https://github.com/kutlusoy/elektron-net-lnd | LND adaptations (supporting / experimental) |
| elektron-net-lnd-startos | https://github.com/kutlusoy/elektron-net-lnd-startos | StartOS package for LND |

### 5. Public live surfaces (operational context only)

| Surface | URL |
|---------|-----|
| Project site | https://www.elektron-net.org/ |
| Block explorer | https://mempool.elektron-net.org/ |
| Stats / retarget | https://elektron-net.org/stats/ |
| Faucet | https://faucet.elektron-net.org/ |
| PPLNS pool UI | https://pplns.elektron-net.org/ |

### Suggested review order

1. https://github.com/kutlusoy/elektron-net — consensus, attestation, pruning, snapshots, wallet recovery paths  
2. https://github.com/kutlusoy/elektron-net-pool and https://github.com/kutlusoy/elektron-net-ppool — mining template / coinbase handling  
3. https://github.com/kutlusoy/elektron-net-seeder — peer discovery  
4. Stack / explorer / Electrum / LND — operational behaviour; consensus rules should still be taken from the primary node repo  

---

## How to use this map

Useful practices for any independent review:

1. Trace each claim to source (path + symbol) before accepting or rejecting it.  
2. Prefer `src/`, `doc-elektron/`, `WHITEPAPER.md`, and in-tree tests over secondary commentary.  
3. Separate **consensus rules** (shared validation) from **policy / peer behaviour** (advertisements, optional probes).  
4. If a path or symbol listed here is missing or differs on the branch under review, report that discrepancy.  
5. Record residual risks even when code matches a claim (e.g. low hashrate, archive nodes, operational snapshot availability).

This guide is a **locator**, not a verdict.

---

## Design hinge: continuous per-block UTXO attestation

The project’s central technical claim is that **per-block UTXO attestation** is what turns aggressive pruning from a mere retention policy into a **consistently secured design**. Reviewers should treat the following as the stated hinge — and verify each link in source.

### Stated mechanism

1. **Every block at height > 0** (from the first post-genesis block onward) must carry a hash of the UTXO set (after the block is connected) in the coinbase (`OP_RETURN` attestation payload).  
2. **Validation is consensus-critical:** missing or mismatched attestation → block rejected (`missing-utxo-attestation` / `bad-utxo-attestation`).  
3. That produces a **continuous, on-chain-bound chain of UTXO states** (not only occasional checkpoints).  
4. **Mandatory pruning (~137 days)** can then delete old full block data without abandoning a verifiable account of the *current* UTXO set: integrity rides on the attestation trail + headers + current coins, not on retaining every historical transaction forever.  
5. **Automatic checkpoint snapshots** are built on the same commitment model and can be checked against claimed state (sidecar content hash; subsequent blocks continue the attestation chain).

### Headers, PoW, and the “bloodline” argument (code-checkable)

Before objecting that “the old block is gone, so the past is unprovable,” separate **three layers** the protocol keeps or commits to:

| Layer | Retained after prune? | What it proves | Where to inspect |
|-------|----------------------|----------------|------------------|
| **Full block body** (txs, scripts, amounts) | **No** (beyond prune window) | Exact historical transaction content | Prune paths; `MandatoryPruneDepth` |
| **Block headers** (80 bytes each, chained `hashPrevBlock`) | **Yes — permanent** | An unbroken header chain from genesis to tip; each link commits to the previous header | Header storage / `CBlockIndex`; IBD header sync |
| **Per-block UTXO attestation** (in coinbase of each block while it existed; validated at connect) | Commitment was enforced **when the block was accepted**; tip continues the chain | That the UTXO set at that height matched what miners/nodes validated under consensus | `ValidateUTXOCheckpoint` on every connect height > 0 |

**Stated argument (for reviewers to stress-test):**

1. **Attestation from block 1.** Consensus requires UTXO attestation on every height > 0 from the start of the chain rules under review — not only after a late soft-fork or only at rare checkpoints. Confirm in `ValidateUTXOCheckpoint` / `ConnectBlock` that height ≤ 0 is the sole skip (genesis), not an optional window.  
2. **When a full block is later pruned, its *existence and linkage* are not erased from the consensus surface.** The header chain remains. Each new header embeds `hashPrevBlock` (and the rest of the header fields that entered the PoW hash). Today’s tip header is the cumulative tip of that chain: changing any past header would break the hash link all the way forward.  
3. **PoW is the network’s ongoing notarization.** Miners expend work on headers; nodes accept headers/blocks only if PoW and rules pass. The living network (miners + validating nodes) continuously extends and re-checks that chain. Under Nakamoto consensus, the proof that “this history was the one the network built on” is the **work-weighted header chain**, not indefinite retention of every old `blk*.dat` file.  
4. **You do not need the old block body to hold the chain’s linkage proof.** The project’s framing: the proof that past blocks occupied their places is carried forward in **today’s headers** — each header is a child of the previous one, so the tip carries the “DNA” of every ancestor header. Metaphor used in project discussion: a **continuing bloodline** — the living tip certifies descent; the discarded flesh (full historical txs) is not required to prove the line of succession.  
5. **UTXO attestation adds a second spine for *balances*.** Headers prove the sequence of blocks and work; per-block UTXO attestation (while each block was connected) bound the **coin set** at that height. After prune, the **current** UTXO set plus the ongoing attestation on new blocks is what nodes use for present ownership; snapshots bootstrap that present state against checkpoint metadata.

**What this is *not* claiming (keep precision):**

- It does **not** claim you can reconstruct arbitrary old transaction graphs after prune without an external archive.  
- It does **not** claim headers alone re-derive historical UTXO contents byte-for-byte without snapshots / prior state.  
- It **does** claim: unbroken **header** succession under PoW remains after prune; **present** UTXO integrity is maintained via continuous attestation + current coins (+ snapshots at checkpoints).

Under that split, “I no longer need to *see* the old block body” is a deliberate design choice: the **lineage proof** lives in the header bloodline; the **present pocket** lives in the UTXO set and its attestations.

### Why the project presents this as distinctive

| Approach | Typical pattern | Contrast claimed by Elektron Net |
|----------|-----------------|----------------------------------|
| Classic Bitcoin | UTXO set is implicit; no per-block on-chain UTXO commitment; full history optional but culture is archival | Continuous explicit UTXO commitment every block; mandatory prune of bodies; headers permanent |
| Bitcoin AssumeUTXO | Rare, hardcoded / socially distributed snapshot hashes | Automatic snapshots plus ongoing per-block attestation from height 1 |
| Account-based chains (e.g. Ethereum-style) | State roots in a different (account) model | Same family remains **UTXO + PoW**, with added per-block UTXO binding |
| “Aggressive prune only” | Delete history without a continuous state commitment | Pruning is paired with consensus-enforced attestation + permanent headers |

**Project framing (for reviewers to test, not to accept on trust):**  
The combination of **continuous per-block UTXO attestation (from height 1) + permanent PoW header chain + mandatory pruning of bodies + automatic checkpoint snapshots** inside a **Bitcoin-like UTXO PoW** chain is presented as the unique hinge. Without the attestation trail, heavy pruning would be a storage choice with a weaker integrity story for *balances*; without permanent headers, succession of the chain would be weaker. Together, deleting old block *bodies* is argued to remain compatible with proving both **lineage** (headers/PoW) and **present coin integrity** (UTXO + attestation).

### Where to verify the hinge in code

| Step | What to confirm | Primary locations |
|------|-----------------|-------------------|
| Commitment from height 1 | Attestation required for every height > 0 | `ValidateUTXOCheckpoint` — early return only for `nHeight <= 0` |
| Commitment produced | Attestation hash derived from post-connect UTXO view | `ComputeBlockUTXOAttestationHash` — `src/validation.cpp` |
| Commitment embedded | Coinbase carries height + 32-byte hash | `ExtractCoinbaseUTXOAttestation`; miner / GBT `coinbase_required_outputs` |
| Commitment enforced | Connect fails on missing/mismatch | `ValidateUTXOCheckpoint` from `ConnectBlock` — `missing-utxo-attestation`, `bad-utxo-attestation` |
| Header permanence | Headers retained independent of body prune | Block index / header chain; prune deletes block files, not the header index lineage |
| PoW linkage | Each header commits to previous via `hashPrevBlock`; work accumulates at tip | Same as Bitcoin Core header validation; chain work comparison |
| Continuity of attestation | Not checkpoint-only | Same validation path every block; checkpoint logging may differ only in verbosity |
| Algorithm evolution | Full rescan vs MuHash after activation height | `MuhashAttestationActivationHeight`; `src/kernel/utxo_muhash.h` |
| Same-block spends | Intra-block dependent txs handled after fix height | `IntraBlockAttestationFixActivationHeight`; fix report under `doc-elektron/` |
| Prune coupling | Body history depth capped; headers + UTXO + new attestations remain | `MandatoryPruneDepth`; prune range in validation / blockstorage |
| Snapshot coupling | Checkpoint files + `.hash` sidecar; activation checks | `WriteAutomaticSnapshot`, `MaybeActivateAutomaticSnapshot`, `PopulateAndValidateSnapshot` |

Open review questions (examples): Is the attestation computed against the intended view (pre/post coinbase OP_RETURN edge cases)? Does every honest miner path always include `coinbase_required_outputs`? After prune, can a node prove *lineage* (yes, headers) vs *old tx contents* (no, by design)? What residual risks remain if snapshots are scarce or if peers archive the public stream off-protocol?

---

## Project claims and where to inspect them

The following rows summarize **what the project states** and **where to look**. Whether the implementation is complete, safe, or sufficient is for the reviewer to decide.

### Scope (high level)

| Stated claim | Primary inspection points |
|--------------|---------------------------|
| Bitcoin Core C++ fork; SHA-256d PoW | Tree layout; PoW path vs upstream Core; no alternate PoW module |
| ~21M supply; 60s blocks; scaled subsidy | `src/kernel/chainparams.cpp`, subsidy / halving parameters |
| No pre-mine / no founder allocation | Genesis construction in `chainparams`; subsidy schedule |
| Mandatory ~137-day pruning | `MandatoryPruneDepth`; prune paths in `validation` / `blockstorage`; GUI options |
| **Per-block UTXO attestation (design hinge)** | `ComputeBlockUTXOAttestationHash`, `ValidateUTXOCheckpoint`, miner GBT fields — see section above |
| Checkpoint snapshots + P2P bootstrap | `WriteAutomaticSnapshot`, `MaybeActivateAutomaticSnapshot`, `src/protocol.h` messages |
| Privacy via time-bounded history (not ZK) | Whitepaper + prune design; absence of mandatory stealth/ring/ZK modules |

Human-readable project docs:

- https://github.com/kutlusoy/elektron-net/blob/main/WHITEPAPER.md  
- https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md  
- https://github.com/kutlusoy/elektron-net/blob/main/right-to-be-forgotten.md  
- https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/mining-pool-integration.md  
- https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/AUDIT_PRUNING_SNAPSHOT.md  

---

## Mainnet parameters reported in tree

**Files:** `src/kernel/chainparams.cpp` (`CMainParams`), `src/consensus/params.h`

Values observed in the documented v4.0.x mainnet configuration (confirm on the commit under review):

| Parameter | Reported mainnet value | Symbols / notes |
|-----------|------------------------|-----------------|
| Block target spacing | 60 seconds | `nPowTargetSpacing` |
| Mandatory prune depth | 197280 blocks (~137 days at 60s) | `MandatoryPruneDepth` |
| Network magic | `0xe1ec7a6e` | `pchMessageStart` |
| Bech32 HRP | `be` | `bech32_hrp` |
| MuHash attestation activation | 137000 | `MuhashAttestationActivationHeight` |
| Stoic Awakening end | 150000 | `StoicAwakeningEndHeight` |
| Intra-block attestation fix | 170000 | `IntraBlockAttestationFixActivationHeight` |
| Protocol version | 70017 | protocol / chainparams constants |
| SLIP-44 coin type | 1370 (`ELEK`) | wallet docs / changelogs |

Reviewers should re-read these constants on the exact commit hash under audit.

---

## Feature inspection map (with stated argumentations)

Each subsection: **claim → why the project argues it matters → where to verify → residual questions.**

### Mandatory pruning

**Stated claim:** Full transaction bodies older than ~137 days are deleted in the reference client; retention is depth-based and not a user “full archive” toggle.

**Argumentation:**  
Old *bodies* are treated like spent receipts — useful for a while, then discarded. **Lineage** does not live in those files; it lives in the **header chain** (permanent) and in **PoW** accumulated at the tip. **Present balances** live in the **UTXO set**, continuously rebound by per-block attestation. Therefore pruning is framed as removing bulk data whose consensus role has already been discharged at connect time, not as erasing the bloodline of the chain.

| Topic | Locations to inspect |
|-------|----------------------|
| Depth constant | `MandatoryPruneDepth` in `src/consensus/params.h`; set in `src/kernel/chainparams.cpp` |
| Enforcement | Prune range logic in `src/validation.cpp`, `src/node/blockstorage.cpp` |
| User controls | GUI/options: `src/qt/optionsdialog.cpp`, `optionsmodel.cpp`, `intro.cpp` |
| Design write-up | `doc-elektron/BITCOIN_CORE_DIFF.md` |

**Review notes:** Reference-client deletion ≠ impossibility of off-protocol archival of the public stream. Confirm headers/index are not pruned away with bodies. Deep reorgs beyond the prune window are a deliberate non-goal — document that as design, not accidental omission.

### Per-block UTXO attestation

**Stated claim:** From height 1 onward, every block’s coinbase commits to the post-connect UTXO hash; mismatch rejects the block under consensus.

**Argumentation:**  
This is the **balance spine**. Headers prove *order and work*; attestation proves *the coin set the network agreed on at that height*. Together they answer: “Even if I delete the old body, did the network, at that moment, accept a specific UTXO state?” Yes — or the block would not have connected. Ongoing tips continue the same rule, so the present remains bound.

| Symbol / area | File (approximate) | Role |
|---------------|--------------------|------|
| `ComputeBlockUTXOAttestationHash` | `src/validation.cpp` | Recompute attestation for a block |
| `ExtractCoinbaseUTXOAttestation` | `src/validation.cpp` | Parse coinbase `OP_RETURN` payload |
| `ValidateUTXOCheckpoint` | `src/validation.cpp` | Validation on connect (skip only height ≤ 0) |
| `ConnectBlock` call site | `src/validation.cpp` | Whether failure rejects the block |
| GBT / miner outputs | `src/node/miner.cpp`, `src/rpc/mining.cpp` | `coinbase_required_outputs` |
| MuHash state | `src/kernel/utxo_muhash.h` (+ validation integration) | Incremental path after activation height |
| Intra-block spend fix | gated by `IntraBlockAttestationFixActivationHeight` | Same-block dependency handling |
| Fix report | `doc-elektron/fix-report-utxo-attestation-intra-block-chain.md` | Background and tests |
| Functional test | `test/functional/feature_utxo_attestation_intra_block_chain.py` | Behaviour below/above fix height |

**Review notes:** Verify height-1 continuity (not delayed activation of the *requirement*). MuHash activation changes *how* the hash is computed, not *whether* attestation is required. Intra-block fix height is a rollout gate for a correctness fix — confirm behaviour below/above.

### Header chain and PoW lineage (“bloodline”)

**Stated claim:** After bodies are pruned, the proof that past blocks occupied their places remains in the permanent header chain; each tip header transitively commits to all ancestor headers under SHA-256d PoW.

**Argumentation:**  
A block body can be discarded once its rules were checked at connect time. The **header** is the fixed-size digest link: `hashPrevBlock` ties each generation to the last. The network’s miners extend that line with work; nodes accept only valid work and links. **Today’s header is the living end of that bloodline** — altering any ancestor header would invalidate every descendant hash through to the tip. That is a stronger operational answer to “prove the chain was unbroken” than keeping multi-gigabyte historical bodies on every node. Full bodies are for *replay of old tx detail*; headers + work are for *succession*.

| Topic | Locations to inspect |
|-------|----------------------|
| Header fields / prev hash | Core block header structure; `hashPrevBlock` in validation |
| Chain work / tip selection | Same family as Bitcoin Core most-work chain rules |
| What prune deletes vs keeps | `blockstorage` prune of `blk*.dat` / undo vs block index headers |
| IBD headers-first | Header sync before bodies; remains valid after prune |

**Review notes:** Headers do not by themselves restore old UTXO *contents*; pair this section with attestation + snapshots for *balances*. PoW security still scales with honest hashrate — a young network’s work total is an operational fact, not hidden by the metaphor.

### Snapshots and P2P bootstrap

**Stated claim:** At checkpoint intervals, nodes write UTXO snapshot files (`.dat` + `.hash`); new or lagged nodes can bootstrap the *present* coin set without downloading pruned bodies.

**Argumentation:**  
Headers give lineage; attestation gives continuous state binding; **snapshots** give a practical *bootstrap artifact* for the UTXO set at checkpoint heights so a node need not replay 137 days of bodies that peers no longer serve. Content hash checks reduce “blind file” trust; **subsequent blocks’ attestations** are the live cross-check.

#### Wrong snapshot → self-isolation (not chain capture)

**Project argument (verify in code, do not treat as a closed audit verdict):**

1. A node that activates a **false but self-consistent** UTXO snapshot (file matches its own `.hash`) does **not** rewrite the honest network tip.  
2. On the next **honest** block from the real chain, local UTXO attestation recomputation diverges from the coinbase commitment → block rejected (`bad-utxo-attestation` / related paths).  
3. That node **fails to follow the network** and **isolates itself** (stalls on the poisoned view or only extends a private invalid fork). Honest peers are not pulled onto the bad state: they already share the work-weighted header chain and matching coin sets.  
4. Under the working assumption that **remaining snapshot-serving nodes are clean** (honest software, correct checkpoint snapshots), the misaligned joiner is the outlier — the same outcome as any peer that violates consensus and is left behind.  
5. **PoW energy is bound into the snapshot’s place in history.** A checkpoint snapshot is not free-floating: it is keyed to a **checkpoint block hash** that must sit on the **header chain**. That chain is the cumulative product of network PoW (miners) and ongoing validation (nodes). The full “weight” of past work is not discarded when bodies are pruned; it remains in the permanent header bloodline and in every later block built on those headers. A fabricated UTXO set does **not** fabricate alternate accumulated work.

**Net effect claimed:**  
Snapshot bootstrap is a **trust surface for joiners**, not a lever to stop or capture the running chain. Bad UTXO state under an honest tip **self-excludes** the misaligned node; clean nodes with good snapshots continue.

**Residual review point (documented in-tree):**  
Current code may still take the **first** `UTXOSNAPSHOT` peer’s advertised hash when starting a download (`dl.utxo_hash = utxo_hash`). That is a **bootstrap first-response race** for new nodes — see `doc-elektron/fix-report-snapshot-bootstrap-trust.md`. Separate clearly:

| Question | Project-facing answer to stress-test |
|----------|--------------------------------------|
| Can a bad snapshot stop or rewrite the honest tip? | No — tip follows PoW + attestation among honest nodes |
| Can one racing peer mis-lead a *fresh* joiner’s initial UTXO file? | Possible until multi-peer hash agreement (or equivalent) is implemented |
| What happens to that joiner under an honest tip? | Self-isolation via `bad-utxo-attestation` |

| Topic | Locations |
|-------|-----------|
| Snapshot write | `WriteAutomaticSnapshot` in `src/validation.cpp` |
| Activation | `MaybeActivateAutomaticSnapshot` in `src/init.cpp` |
| Content check | `PopulateAndValidateSnapshot` / `ActivateSnapshot` (`expected_utxo_hash`) |
| Service bit | `NODE_SNAPSHOT` in `src/protocol.h` |
| Request logic | `MaybeRequestSnapshot` in `src/net_processing.cpp` |
| First-peer hash init | `UTXOSNAPSHOT` handler in `src/net_processing.cpp` |
| Self-isolation path | `ValidateUTXOCheckpoint` → `bad-utxo-attestation` on later honest blocks |

**P2P message type strings** (wire limit `MESSAGE_TYPE_SIZE == 12` in `src/protocol.h`):

| Constant | Reported wire string |
|----------|----------------------|
| `GETUTXOSNAPSHOT` | `getutxosnap` |
| `UTXOSNAPSHOT` | `utxosnapshot` |
| `GETSNAPSHOTDATA` | `getsnapdata` |
| `SNAPSHOTDATA` | `snapshotdata` |

Project docs describe a past bug where longer names overflowed the 12-byte type field. Reviewers can confirm current strings and handlers independently.

**Trust questions for reviewers (open):**

- What is verified at activation (sidecar presence, content hash, metadata base hash)?  
- Confirm self-isolation: diverging UTXO vs honest tip → `bad-utxo-attestation` (joiner left behind, tip unaffected).  
- How is snapshot *availability* among honest peers guaranteed operationally?  
- Is multi-peer hash agreement implemented yet, or still only proposed in `fix-report-snapshot-bootstrap-trust.md`?

See `doc-elektron/AUDIT_PRUNING_SNAPSHOT.md` and `doc-elektron/fix-report-snapshot-bootstrap-trust.md`.

### Wallet recovery on pruned nodes

**Stated claim:** With only a seed and the current UTXO set, a wallet can recover **balances** (“pocket”) without historical transaction lists.

**Argumentation:**  
Aligns with the Pocket philosophy: ownership is what you hold *now*, not a permanent receipt archive. After prune, `rescanblockchain` over missing bodies cannot replay old txs; **`ScanUTXOSet` / `forEachCoin`** match wallet scripts against the live coin set instead. History unavailability is intentional; balance recovery is the supported path.

| Topic | Locations |
|-------|-----------|
| UTXO iteration API | `forEachCoin` — `src/interfaces/chain.h`, `src/node/interfaces.cpp` |
| Scan / credit | `ScanUTXOSet`, related wallet paths — `src/wallet/wallet.cpp` |
| Attach behaviour when pruned | wallet `AttachChain` / load paths |

**Review notes:** Confirm user-facing messages do not promise full tx history after prune. Descriptor / SLIP-44 coin type 1370 paths should be checked for wallet compatibility.

### Mining / pool surface

**Stated claim:** Valid blocks require coinbase bytes that match the attestation commitment; pools must not mutate coinbase in ways that break the hash (e.g. classic extranonce splicing into scriptSig).

**Argumentation:**  
Attestation is only as strong as every miner’s coinbase construction. GBT exposes `coinbase_required_outputs`; reference pools append them verbatim and keep extranonce out of the committed coinbase body (`extranonce2_size = 0` pattern in integration docs). A pool that ignores this produces blocks the network rejects — the consensus rule is the backstop, not trust in pool software.

| Topic | Locations |
|-------|-----------|
| Integration guide | `doc-elektron/mining-pool-integration.md` |
| Reference miners | `mining/miner.py`, `mining/miner.cpp` |
| Pool implementations | `elektron-net-pool`, `elektron-net-ppool` (separate repos) |
| GBT field | `coinbase_required_outputs` via `src/rpc/mining.cpp` |

**Review notes:** Check per-miner templates if payout address is bound into the attestation input; fee outputs that change coinbase structure; Stratum compatibility lists.

### Randomness

**Stated claim:** Key and seed material follow Bitcoin Core’s strong RNG path — no project-local weak PRNG fallback.

**Argumentation:**  
Integrity of attestation and ownership assumes sound key generation. Reviewers comparing to incidents like weak embedded PRNG failures should inspect `GetStrongRandBytes` and entropy sources for regressions vs Core, not assume branding changes imply crypto changes.

| Topic | Locations |
|-------|-----------|
| Strong bytes API | `GetStrongRandBytes` in `src/random.cpp` |
| Entropy sources | RdSeed / RdRand / OS CSPRNG paths in the same file |

### Stoic Awakening (historical mainnet behaviour)

**Stated claim:** A min-difficulty escape after long gaps existed from genesis and was **retired** at a fixed height after live operation showed instability.

**Argumentation:**  
Documented retirement is part of the project’s “own the outcomes” stance: a liveness heuristic that misbehaved under real hashrate was removed rather than left as permanent consensus debt. Reviewers should not describe post-end-height mainnet as still using that escape.

| Topic | Locations |
|-------|-----------|
| Heights | `MinDifficultyActivationHeight`, `StoicAwakeningEndHeight` in chainparams / `params.h` |
| Retirement notes | `doc-elektron/CHANGELOG-stoic-awakening-retirement.md` |

Confirm whether the tip under review is past the reported end height before describing current difficulty behaviour.

---

## Comparative context (for framing only)

| Topic | Typical questions | Elektron Net inspection focus |
|-------|-------------------|-------------------------------|
| Supply / allocation | Premine, admin mint? | Genesis + subsidy code |
| Crypto surface | Novel primitives? | Divergence from Core; MuHash for attestations |
| History retention | Full archive default? | Mandatory prune of **bodies**; **headers permanent** |
| State commitment | Epoch-only / none / account state roots? | **Per-block UTXO attestation from height 1** |
| Lineage after prune | “Old block gone ⇒ past unprovable”? | Header bloodline + PoW tip; bodies optional for succession proof |
| Snapshot trust | Social / HTTP / hardcoded only? | Sidecar hash, activation checks, follow-on attestation chain |
| Prune + integrity | Delete history without continuous commitment? | Attestation + headers is what the project claims makes heavy prune coherent |
| Privacy model | ZK / mixer / transparent forever? | Time-bounded public bodies; not ZK |

This table is a prompt for questions, not a scorecard. The uniqueness argument lives or dies on whether continuous attestation and header permanence are correctly enforced in consensus — not on marketing wording.

---

## Activation heights (as documented for mainnet)

```
MuhashAttestationActivationHeight        = 137000
StoicAwakeningEndHeight                  = 150000
IntraBlockAttestationFixActivationHeight = 170000
MandatoryPruneDepth                      = 197280
```

Re-verify in `src/kernel/chainparams.cpp` on the audited commit.

---

## Optional verification checklist

Use as a work list; pass/fail is the reviewer’s call.

### Consensus integrity

- [ ] Locate `ValidateUTXOCheckpoint` and its `ConnectBlock` call site; note failure codes  
- [ ] Confirm attestation is required for **all heights > 0** (genesis-only skip), not only checkpoints  
- [ ] Locate MuHash vs legacy path and intra-block fix gate in `ComputeBlockUTXOAttestationHash`  
- [ ] Confirm `MandatoryPruneDepth` consistency across chainparams, validation, and tests  
- [ ] Confirm prune removes block **bodies** while **headers / block index lineage** remain  
- [ ] Note how tip selection uses chain work on the header chain (lineage under PoW)  

### Snapshot bootstrap

- [ ] Confirm P2P type strings length ≤ 12 in `src/protocol.h`  
- [ ] Trace when `NODE_SNAPSHOT` is set/cleared  
- [ ] Trace activation refusal paths (missing `.hash`, content mismatch)  
- [ ] Note behaviour after a divergent UTXO view on subsequent blocks  

### Mining surface

- [ ] Confirm GBT exposes required coinbase outputs  
- [ ] Compare reference miner/pool behaviour to integration doc  
- [ ] Note any fee/output patterns that would change attestation input bytes  

### Wallet / prune UX

- [ ] Trace pruned-node balance recovery path (`ScanUTXOSet` or equivalent)  
- [ ] Check whether GUI/RPC still allow a full archival reference configuration  

### Crypto hygiene

- [ ] Diff `src/random.cpp` (or equivalent) against upstream expectations  
- [ ] Search for project-local weak PRNG use in key/seed paths  

### Documentation alignment

- [ ] Compare whitepaper parameter tables to chainparams  
- [ ] Note any retired features still described as active (e.g. Stoic Awakening)  
- [ ] State privacy model accurately (structural retention limits vs ZK)  

Suggested finding format:

`[severity] claim or risk — evidence: path:symbol — observation: ... — residual risk: ...`

---

## Document map

| Document | Typical use |
|----------|-------------|
| `WHITEPAPER.md` | Stated philosophy and parameters |
| `doc-elektron/BITCOIN_CORE_DIFF.md` | File-level diff vs Bitcoin Core |
| `doc-elektron/AUDIT_PRUNING_SNAPSHOT.md` | Project audit scenarios |
| `doc-elektron/fix-report-utxo-attestation-scalability.md` | MuHash / attestation scalability |
| `doc-elektron/fix-report-utxo-attestation-intra-block-chain.md` | Intra-block dependency fix |
| `doc-elektron/CHANGELOG-muhash-attestation.md` | Activation and related fixes |
| `doc-elektron/CHANGELOG-stoic-awakening-retirement.md` | Difficulty escape retirement |
| `doc-elektron/mining-pool-integration.md` | Pool / Stratum integrators |
| `right-to-be-forgotten.md` | Legal framing of pruning |
| `mining/README.md` | Reference miners |

---

## Maintainer note

When consensus parameters or attestation code change, update this map’s parameter tables and feature paths, and keep links aligned with `doc-elektron/BITCOIN_CORE_DIFF.md` and relevant changelogs.

**Alignment target:** v4.0.x tree documentation for mainnet heights 137000 / 150000 / 170000 and prune depth 197280 — always re-check the commit under review.

---

*End of source map. Independent reviewers should form their own conclusions from the code and tests.*
