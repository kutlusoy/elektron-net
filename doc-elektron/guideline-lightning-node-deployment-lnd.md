# Elektron Net — LND Gossip/Routing Node Deployment Guideline

- **Version:** 0.1 (draft)
- **Date:** July 18, 2026
- **Audience:** Infrastructure operators and Lightning integrators planning the first Elektron Net LND node
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) — `src/kernel/chainparams.cpp` (`CMainParams`), `src/rpc/blockchain.cpp`/`src/rpc/fees.cpp`/`src/rpc/rawtransaction.cpp` (RPC surface), `src/init.cpp` (ZMQ) — treat these as ground truth for anything this doc references
- **Fork base (proposed):** [`lightningnetwork/lnd`](https://github.com/lightningnetwork/lnd) (see SS2.2 for why `ltcsuite/lnd` / "lndltc" is the more directly relevant template to study first, even though upstream `lnd` is the actual fork base)
- **Consumer:** [`elektron-net-electrum`](https://github.com/kutlusoy/elektron-net-electrum) — `electrum/lnpeer.py` (`chain_hash` validation on both outgoing `open_channel` and incoming peer messages), gossip-mode channel opening in `new_channel_dialog.py`/`qechannelopener.py` (manual Node ID entry, separate code path from trampoline). Also relevant: `elektron-net-stack` (deployment/docker-compose home for `electrs`/`mempool`/etc.)
- **See also:** [`guideline-lightning-node-deployment.md`](./guideline-lightning-node-deployment.md) (the Eclair trampoline-hub plan -- **this document is a parallel, separate deployment, not a replacement**; SS1 below explains the relationship), [`guideline-wallet-integration.md`](./guideline-wallet-integration.md), [`guideline-electrs-fork-integration.md`](./guideline-electrs-fork-integration.md), [`BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md)

Requirement levels follow common practice: **MUST** = mandatory for correct/safe operation, **SHOULD** = strongly recommended, **MAY** = optional.

- never use this char "—" in your texts and comments. Use instead another notation.

---

## 1. Relationship to the Eclair Plan -- Read This First

`guideline-lightning-node-deployment.md` already established that **Eclair is the only viable candidate for the wallet's trampoline hub role**, because trampoline-forwarding (BOLT extension `option_trampoline_routing`) is only implemented in production by Eclair. That conclusion is unchanged by this document. **An LND node cannot substitute for that role** -- `elektron-net-electrum`'s `is_trampoline_peer()` (`lnworker.py`) checks for `LnFeatures.OPTION_TRAMPOLINE_ROUTING_OPT_ECLAIR`/`OPT_ELECTRUM`, which a stock LND node does not and will not advertise.

**What LND *is* good for on this network:** a normal BOLT-spec gossip-routing peer. `elektron-net-electrum` already supports classic gossip-mode channel opening as an alternative to trampoline (manual remote-node-ID entry, see `new_channel_dialog.py` -- the `if self.network.channel_db:` branch, separate from the trampoline combo box). An LND node, once chain-parameter-forked for Elektron Net, can peer with the wallet and with other nodes exactly like any Bitcoin-network LND node does today: publish channel gossip, forward payments along the public graph, and act as one more point of liquidity on a network that currently has zero routing nodes of any kind.

**Decided (2026-07-18): this LND deployment is planned as the *first* Lightning infrastructure piece built, ahead of the Eclair trampoline hub**, primarily because of its materially lighter resource footprint (SS4) fitting the project's current infrastructure more easily. This is a sequencing decision, not a replacement of the Eclair plan -- both remain necessary for the wallet's full intended Lightning experience (trampoline mode needs Eclair regardless of how many LND gossip peers exist).

## 2. Fork Candidate Selection

### 2.1 Why LND specifically (for this role)

Compared to Core Lightning, LND has:

- A native multi-chain configuration surface (`chainreg` package, `chaincfg.Params` structs), already registering Bitcoin and Litecoin side by side rather than assuming a single hardcoded chain.
- A real, actively maintained fork precedent for exactly this kind of Bitcoin-derivative altcoin port: [`ltcsuite/lnd`](https://github.com/ltcsuite/lnd) ("lndltc"), which did the equivalent chain-parameter and backend-naming work for Litecoin.
- A lightweight Go binary, in sharp contrast to Eclair's JVM footprint (SS4) -- directly relevant given this project's current infrastructure constraints.

Core Lightning has no comparable multi-chain abstraction or altcoin-fork precedent found during this research.

### 2.2 `ltcsuite/lnd` as a study template, not necessarily the fork base itself

`ltcsuite/lnd` is valuable primarily as **evidence of what a completed Bitcoin-derivative LND port looks like** (proof the `chainreg`/`chaincfg` model genuinely supports this, and a real diff to read for exact patch points) -- studying it first, before touching upstream `lnd`, SHOULD materially de-risk the estimate in SS5. Whether the actual Elektron Net fork should be based on upstream `lightningnetwork/lnd` directly (adding a third registered chain alongside Bitcoin/Litecoin) or forked from `ltcsuite/lnd` itself (already carrying the Litecoin-specific renaming pattern, arguably a closer starting template) is an open implementation-planning question, not resolved here (see SS8).

## 3. Elektron Net Parameters an LND Fork MUST Account For

Same real, load-bearing values as the Eclair plan (`elektron-net/src/kernel/chainparams.cpp`, `CMainParams`) -- restated here for a reader who only has this document open:

| Parameter | Elektron Net mainnet value | Notes |
|---|---|---|
| Genesis block hash (`chainHash` basis) | `00000006b054338443f1a5d5534df21eab0d13232028158ae198edbb169f9dad` | The core value any `chaincfg.Params`-equivalent entry needs. |
| P2P network magic (`pchMessageStart`) | `e1 ec 7a 6e` | Only relevant if the fork ever uses the `btcd` full-node backend (which speaks the chain's own P2P protocol directly). **Not relevant if the fork uses LND's `bitcoind` RPC-polling/ZMQ backend mode against `elektrond`** (SS3.1) -- same reasoning already established for the Eclair plan and the electrs guideline. |
| Default P2P port | `8333` | Same caveat as above. |
| RPC port | `8332` (unchanged from Bitcoin Core default) | Identical to Bitcoin, no override needed. |
| `base58Prefixes[PUBKEY_ADDRESS]` / `SCRIPT_ADDRESS` / `SECRET_KEY` (WIF) | `0` / `5` / `128` | **Identical to Bitcoin mainnet** (deliberate fork decision, see `elektron-net-electrum`'s `doc/elektron.md`). |
| Extended key prefixes (xpub/xprv) | `0488B21E` / `0488ADE4` | **Identical to Bitcoin mainnet.** |
| Bech32 HRP | `be` | **Differs from Bitcoin's `bc`.** MUST be overridden wherever the fork encodes/decodes on-chain addresses (channel funding/closing outputs, on-chain wallet addresses). |
| BOLT11 HRP (Lightning invoices) | **Finalized: `be`** (full invoice prefix `lnbe`) | Same value the wallet and the Eclair plan use -- see `guideline-lightning-node-deployment.md` SS3 for the full reasoning. Any node on this network, including this LND fork, MUST use the same value or its invoices will be unparseable by every other component. |
| Mandatory pruning depth (`nPruneAfterHeight`) | `197280` blocks (~137 days at 60s blocks) -- enforced on every Elektron Net node, no archival mode exists anywhere | The most consequential parameter here; see SS3.2 (materially less settled for LND than the Eclair research found for Eclair -- read this carefully before committing to LND-first sequencing on the strength of resource footprint alone). |
| BIP44 coin type | `1370` (registered SLIP-44) | Relevant only to this fork's *own* on-chain wallet key derivation (`btcwallet`), an independent decision from `elektron-net-electrum`'s choice to keep `m/0'`/`m/1'` -- see that repo's `doc/elektron.md` for the precedent reasoning, not necessarily the same conclusion. |

### 3.1 Chain-identity mechanism: preliminary, needs direct source confirmation

Same caveat as the Eclair document's SS4.1: this is search-tool-based research, not yet a direct clone-and-read. LND's chain identity is handled through the `chainreg`/`chaincfg.Params` model: a chain is a statically registered Go struct (genesis hash, net magic, default ports, address/HD-key version bytes, checkpoints) looked up by a `--bitcoin.<network>` style flag, rather than derived at runtime from the connected backend the way Eclair's `getblockhash 0` approach works. This means, unlike the Eclair finding, **a new Elektron Net entry likely MUST be registered as an actual new `chaincfg.Params` value (or an existing one repurposed) at compile time** -- there is no evidence found of a "trust whatever the backend reports" runtime path equivalent to Eclair's. This makes LND's patch surface plausibly *larger* in kind (a real new code-level chain registration, not primarily a config value), even though the individual parameter values needed (SS3) are the same ones already gathered. `ltcsuite/lnd`'s actual diff against upstream (SS2.2) is the concrete way to size this precisely -- study it before estimating further.

**LND's `bitcoind` backend mode SHOULD be used** (RPC-polling or ZMQ, both already available against `elektrond`, confirmed in that repo's `src/init.cpp` and RPC handlers) rather than the `btcd`-equivalent full P2P client mode -- same reasoning as the Eclair and electrs guidelines: avoids needing to fork/adapt a whole separate P2P-speaking binary, at the cost of `pchMessageStart`/default P2P port becoming irrelevant to this specific integration path.

### 3.2 Mandatory network-wide pruning: less settled than for Eclair -- read carefully

This is the most important difference from the Eclair plan, and the main reason LND-first sequencing SHOULD NOT be treated as strictly lower-risk just because the resource footprint (SS4) is smaller.

Research findings, in order of relevance:

- LND's historical stance on pruned `bitcoind` backends was explicitly that it **"hasn't been safe"**: while LND is offline, `bitcoind` can prune blocks LND still needs to complete a rescan (the mechanism that detects on-chain funding/spending of channel outputs) -- structurally the identical risk Eclair's own pruned-node documentation names, but LND's own historical framing of it was more cautious/unresolved-sounding than Eclair's.
- The tracking issue for **"first class pruned node support"** in `lightningnetwork/lnd` is closed, resolved via a dedicated `ChainPruner` mechanism (manual pruning control, configurable block retention) merged in a specific PR. This is a real, deliberate fix, not an assumption -- but this document has not yet directly confirmed its exact current guarantees (e.g. whether it fully closes the "offline during a prune" gap, or narrows it) against current `lnd` source/documentation.
- Both `lnd` and Eclair ultimately land on the same *operational* mitigation regardless of the exact mechanism: **the node must not be offline longer than the backend's prune retention window**. Elektron Net's ~137-day window remains generous for routine operations either way.

**This document's recommendation, pending direct verification:** treat pruned-node safety as **not yet fully confirmed for LND** in the way it now is for Eclair (SS4.2 of the Eclair doc cites a specific, well-documented, self-sufficient mechanism; this document can only cite "an issue was closed" without having directly confirmed the resulting guarantees). **Before committing to LND-first deployment on the strength of its resource footprint alone, directly verify current `lnd` documentation/source for the exact operational guarantees the `ChainPruner` mechanism provides against a network-wide-pruned chain with no archival fallback anywhere** -- this is materially different from Eclair's situation, where that verification has already been done with a concrete, citable outcome.

## 4. Resource Requirements

Research-based (ACINQ/LND public documentation and community sources), not yet load-tested against this project's actual usage -- same caveat as the Eclair document's SS5.

- **RAM:** ~2GB minimum for the LND process itself; 4-8GB recommended once channel count/volume grows past a modest bootstrap-phase scale. Reported to run on hardware as modest as a Raspberry Pi with 2GB RAM. This does not include the dedicated `elektrond` backend instance running alongside it (SS3.1).
- **CPU:** No hard minimum published; multi-core recommended only at higher channel counts (100+), not relevant to a bootstrap-phase deployment starting from zero channels.
- **Disk:** The LND process itself needs little (a few GB). The usual large cost -- 500GB-1TB for an unpruned Bitcoin full node -- **does not apply the same way here**, since every Elektron Net node (including the dedicated `elektrond` this fork would connect to) is mandatorily pruned to ~197,280 blocks. The disk footprint should stay small and roughly comparable to what `electrs`'s own backing node already needs.
- **Comparison to Eclair:** materially lighter across the board (no JVM baseline, no separately-recommended PostgreSQL instance for production scale). This is the concrete basis for the "current Hetzner instance" question in SS5, in contrast to the Eclair plan's explicit conclusion that co-location wasn't realistic there.

## 5. Proposed Deployment Shape

- SHOULD run as its own `docker-compose` service in `elektron-net-stack`, following the same pre-wired-but-initially-disabled service-profile pattern as `elektron-electrs` and the planned Eclair service.
- SHOULD point at a **dedicated** `elektrond` instance, for the same isolation reasoning as the Eclair plan (SS5 there) -- independent uptime/prune-target control, not shared with `electrs`'s backend.
- **Given LND's materially lighter footprint (SS4), co-locating this on the project's current Hetzner instance (the same one already running `elektrond`/`electrs`/`mempool`) MAY be realistic, unlike the Eclair plan's conclusion that separate infrastructure was needed.** This SHOULD be confirmed with actual headroom numbers (current usage on that box) before committing, not assumed from the general resource figures in SS4 alone.
- Default LN P2P port (`9735`) MAY be kept as-is for the same operator-clarity reasoning as the Eclair plan, unless it conflicts with the Eclair node if both end up deployed on ports visible to the same network segment.

## 6. Checklist

- [ ] Clone `lightningnetwork/lnd` **and** `ltcsuite/lnd`, and directly diff the latter against its own upstream base to concretely size the chain-registration patch surface (SS2.2/SS3.1) -- do this before further effort estimation, the same rigor standard as the Eclair plan's own open item
- [ ] Directly verify current `lnd` documentation/source for the exact operational guarantees of pruned-backend support (`ChainPruner`, SS3.2) against Elektron Net's network-wide-pruned, no-archival-fallback situation -- **do this before finalizing LND-first sequencing**, not after
- [ ] Decide whether the fork bases on upstream `lightningnetwork/lnd` (new third chain registration) or `ltcsuite/lnd` (closer starting template, SS2.2)
- [ ] Confirm the `bitcoind`-mode (RPC-polling or ZMQ) backend integration holds without needing a `btcd`-equivalent P2P client fork (SS3.1)
- [ ] Decide BIP44 coin type usage for this fork's own on-chain wallet seed (independent decision from the end-user wallet's precedent, SS3)
- [ ] Provision a dedicated `elektrond` instance for this node
- [ ] Confirm real available headroom on the current Hetzner instance before deciding co-location vs separate infrastructure (SS5)
- [ ] Add a pre-wired `docker-compose` service profile to `elektron-net-stack`
- [ ] End-to-end test: peer with a real wallet in gossip mode, open a direct (non-trampoline) channel, route a payment

## 7. Relationship to Eclair Plan Checklist Items

Some items are already resolved by the Eclair-plan work and directly reusable here without redoing them:

- BOLT11 HRP (`be`) -- already finalized, see SS3 of this document and `guideline-lightning-node-deployment.md` SS3.
- The general "no LN graph exists yet" bootstrap framing (`guideline-wallet-integration.md` SS3.3) applies identically -- this LND node, once live, becomes part of solving that bootstrap problem the same way the Eclair hub does, just via a different routing mechanism (public gossip vs trampoline).

## 8. Open Questions

1. Fork base: upstream `lightningnetwork/lnd` (SS2.2) or `ltcsuite/lnd` as the closer starting template -- not yet decided, first concrete step is diffing both (SS6).
2. Does the `ChainPruner`/pruned-node-support mechanism (SS3.2) provide the same level of documented, self-sufficient guarantee Eclair's does, once directly verified? This materially affects confidence in the LND-first sequencing decision (SS1).
3. Should this LND node and the eventual Eclair hub peer with *each other* (LND participating in the gossip graph, Eclair bridging trampoline payments into it), or are they intended as fully independent, unconnected pieces of infrastructure at least initially?
4. Co-location with the existing Hetzner stack (SS5) vs separate infrastructure -- pending real headroom numbers, not decided here.
