# Elektron Net — Lightning Trampoline/Routing Node Deployment Guideline

- **Version:** 0.1 (draft)
- **Date:** July 18, 2026
- **Audience:** Infrastructure operators and Lightning integrators planning the first real Elektron Net trampoline/routing node
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) — `src/kernel/chainparams.cpp` (`CMainParams`), `src/rpc/blockchain.cpp`/`src/rpc/fees.cpp`/`src/rpc/rawtransaction.cpp` (RPC surface), `src/init.cpp` (ZMQ) — treat these as ground truth for anything this doc references
- **Fork base (proposed):** [`ACINQ/eclair`](https://github.com/ACINQ/eclair)
- **Consumer:** [`elektron-net-electrum`](https://github.com/kutlusoy/elektron-net-electrum) — `electrum/trampoline.py` (`TRAMPOLINE_NODES_MAINNET`), `electrum/lnworker.py` (`suggest_peer()`, `is_trampoline_peer()`) — the wallet already implements the client side of this (see `fix-report`-level history in that repo's `doc/elektron.md`); this doc plans the missing server side. Also relevant: `elektron-net-stack` (deployment/docker-compose home for `electrs`/`mempool`/etc., the natural place to add this)
- **See also:** [`guideline-wallet-integration.md`](./guideline-wallet-integration.md) §3.3 (Lightning Integration Options, Option A already chosen upstream in the wallet fork), [`guideline-electrs-fork-integration.md`](./guideline-electrs-fork-integration.md) (closest precedent for forking a third-party Bitcoin-adjacent server component for this chain; several findings below directly reuse its reasoning), [`BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md)

Requirement levels follow common practice: **MUST** = mandatory for correct/safe operation, **SHOULD** = strongly recommended, **MAY** = optional.

- never use this char "—" in your texts and comments. Use instead another notation.

---

## 1. Why This Is Needed

`elektron-net-electrum` (this project's wallet fork) already implements Electrum's Option A Lightning architecture: trampoline routing rather than full graph routing, so mobile clients stay lightweight and need no second wallet-side component (see `guideline-wallet-integration.md` SS3.3). As of this writing, `TRAMPOLINE_NODES_MAINNET` in the wallet is intentionally empty (a confirmed live failure showed the previous placeholder entries pointed at real Bitcoin mainnet nodes, which correctly rejected the connection with "no common chain found with remote" -- see that repo's `doc/elektron.md`). **No wallet can open a trampoline-routed channel or make a trampoline payment on Elektron Net until at least one real, permanently online, well-connected trampoline node exists.** This is a pure infrastructure gap, not a wallet software gap, and it blocks Lightning entirely regardless of which wallet a user runs.

## 2. Fork Candidate Selection

### 2.1 The deciding constraint: trampoline-forwarding support, not general LN compatibility

A naive "which Lightning node is easiest to fork for an altcoin" comparison would favor **LND**: it has a native multi-chain abstraction (`chainreg`/`chaincfg.Params`) that already registers Bitcoin and Litecoin side by side, and there is a real, maintained precedent fork (`ltcsuite/lnd`, "lndltc") that did exactly this kind of chain-parameter port for a close Bitcoin derivative. Core Lightning and Eclair have no comparable multi-chain abstraction or altcoin-fork precedent found.

**That comparison is the wrong one for this project, because it optimizes for the wrong requirement.** The wallet's trampoline mode requires the *remote hub node* to understand and forward trampoline-onion payments (BOLT extension `option_trampoline_routing`, checked in the wallet as `LnFeatures.OPTION_TRAMPOLINE_ROUTING_OPT_ECLAIR` / `OPT_ELECTRUM`, see `lnworker.py::is_trampoline_peer()`). This is **not** the same thing as generic Lightning protocol compatibility. Research confirms:

- **Eclair (ACINQ) is the only Lightning implementation with production trampoline-forwarding support** -- it co-designed the extension and it powers ACINQ's own Phoenix mobile wallet, whose whole value proposition depends on it.
- LND and CLN implement standard BOLT-spec source-based onion routing over the full public gossip graph. Neither implements trampoline-onion unwrapping/repacking as a forwarding hub. No amount of chain-parameter forking changes this -- it is a routing-logic gap, not a network-identity one.

**Conclusion: a chain-parameter fork of LND or CLN, however easy to build, would be functionally useless as a trampoline hub for this wallet's already-built architecture.** Eclair is therefore the only viable fork candidate for this specific role, independent of how much forking work it costs -- the alternative isn't "an easier fork," it's "a hub that doesn't do what the wallet needs it to do."

### 2.2 Consequence for wallet-integration guideline SS3.3/SS5

The existing comparison table in `guideline-wallet-integration.md` (SS5) frames "Zeus + a dedicated LND/CLN fork" as *Option B*, an alternative to Electrum's built-in Lightning (*Option A*, already chosen). That framing remains correct for the wallet-side choice. This document is scoped differently: **regardless of Option A vs B, or which wallet a user runs, the trampoline hub infrastructure itself must speak trampoline routing, which narrows the hub's own implementation to Eclair specifically.** This isn't a new fork of the wallet-choice decision, it's the un-optional consequence of Option A already having been picked.

## 3. Elektron Net Parameters an Eclair Fork MUST Account For

All values below are sourced directly from `elektron-net/src/kernel/chainparams.cpp` (`CMainParams`), i.e. real, load-bearing values, not placeholders.

| Parameter | Elektron Net mainnet value | Notes |
|---|---|---|
| Genesis block hash (`chainHash` basis) | `00000006b054338443f1a5d5534df21eab0d13232028158ae198edbb169f9dad` | See SS4.1 -- likely the *only* value that strictly must be supplied, if the research in SS4.1 holds up under direct source inspection. |
| P2P network magic (`pchMessageStart`) | `e1 ec 7a 6e` | **Not relevant if Eclair talks to `elektrond` purely over RPC/ZMQ** (see SS4.1) -- same finding as the electrs guideline SS3.3 point 2 ("magic bytes matter to the DNS seeder and full nodes, not to an RPC-only client"). Flag explicitly if a future Eclair version ever gains an embedded P2P client mode. |
| Default P2P port | `8333` | Same caveat as above -- irrelevant to an RPC-only integration. |
| RPC port | `8332` (unchanged from Bitcoin Core default, per the electrs guideline's own SS3.3 table, cross-checked against this repo) | **Identical to Bitcoin**, no override needed. |
| `base58Prefixes[PUBKEY_ADDRESS]` | `0` | **Identical to Bitcoin mainnet** (deliberate, see `elektron-net-electrum`'s `doc/elektron.md` "Open items"). |
| `base58Prefixes[SCRIPT_ADDRESS]` | `5` | **Identical to Bitcoin mainnet.** |
| `base58Prefixes[SECRET_KEY]` (WIF) | `128` | **Identical to Bitcoin mainnet.** |
| Extended key prefixes (xpub/xprv) | `0488B21E` / `0488ADE4` | **Identical to Bitcoin mainnet.** |
| Bech32 HRP | `be` | **Differs from Bitcoin's `bc`.** MUST be overridden wherever Eclair encodes/decodes on-chain addresses (e.g. mutual-close and force-close output scripts, on-chain wallet addresses for channel funding). |
| BOLT11 HRP (Lightning invoices) | **Undecided** -- currently a placeholder equal to `SEGWIT_HRP` (`be`) in the wallet fork; see `guideline-wallet-integration.md` SS4 checklist, still open | **Blocking cross-repo decision.** The hub node and every wallet MUST agree on the exact same BOLT11 HRP, or invoices generated by one side will be unparseable/rejected by the other. This MUST be finalized before -- or at the very latest alongside -- this node's deployment, not decided independently by whichever component gets built first. |
| Mandatory pruning depth (`nPruneAfterHeight`) | `197280` blocks (~137 days at 60s blocks) -- **enforced on every Elektron Net node, no archival mode exists anywhere on the network** | The single most consequential parameter for this doc; see SS4.2. |
| BIP44 coin type | `1370` (registered SLIP-44) | Relevant only to Eclair's *own* on-chain wallet's key derivation, a separate and independent decision from `elektron-net-electrum`'s choice to keep `m/0'`/`m/1'` for its own wallet (see that repo's `doc/elektron.md` for the reasoning precedent -- worth reading before deciding here, not necessarily worth reusing the same conclusion, since a hub node's on-chain wallet has a different risk profile from an end-user wallet). |

### 4.1 Chain-identity mechanism: preliminary, needs direct source confirmation

Search-tool-based research (not yet a direct clone-and-read, unlike the electrs guideline's methodology, see caveat below) into `eclair-core/src/main/scala/fr/acinq/eclair/Setup.scala` suggests Eclair's chain-identity handling is meaningfully lighter than LND's `chaincfg.Params` model:

- At startup, Eclair calls `getblockhash 0` against the configured bitcoind RPC and derives its working `chainHash` **directly from whatever the connected node reports as its own genesis** -- it does not primarily rely on a hardcoded per-chain constant the way LND's `chaincfg.Params` structs do.
- A separate, smaller lookup table (keyed by a `chain` config string: `mainnet`/`testnet`/`testnet4`/`signet`/`regtest`) supplies a `chainCheckTx` value used for some kind of sanity check; `regtest` already maps to `None` in this table, i.e. Eclair already tolerates a chain that isn't one of the well-known public ones.
- An explicit assertion (`assert(bitcoinChainHash == nodeParams.chainHash, ...)`) fails startup if the configured chain and the connected bitcoind's real genesis disagree -- meaning naively pointing a stock Eclair configured for `chain = "mainnet"` at `elektrond` would deterministically crash (Bitcoin's real mainnet chainHash vs Elektron Net's real genesis hash), confirming *some* patch is still required, just possibly a smaller one than initially assumed.

**This is preliminary and MUST be verified against a direct clone of `ACINQ/eclair` before any implementation estimate is trusted** -- the electrs guideline's rigor (citing exact functions after actually cloning and reading the fork/upstream source) is the standard to match here, and this section does not yet meet it. Treat SS4.1 as "where to start looking," not "confirmed scope of work."

### 4.2 Mandatory network-wide pruning: resolved, not a blocker

The existing `guideline-wallet-integration.md` (SS5 comparison table) left this as an open caveat: *"LND has supported pruned bitcoind backends since v0.13, but not 'network-wide pruned with no fallback peer'"* -- i.e. it was unclear whether any LN implementation's pruned-node support assumes a non-pruned fallback exists somewhere on the network, which Elektron Net can never provide (every node prunes at 197,280 blocks, by design, with no exception -- see the electrs guideline SS1/SS3.6, and `elektron-net-electrum`'s own decision to never become a de-facto archival index for the identical reason).

Research into Eclair's own pruned-bitcoind support (added deliberately, not a workaround) shows it is **self-sufficient against a single pruned node with no archival fallback anywhere** -- confirmed by the feature's own documentation, which states real, actionable operational constraints instead of an unresolvable dependency on an archival peer:

- **Eclair MUST NOT be offline longer than the connected node's prune retention window**, or it cannot catch up (the exact same "gap in retained history" risk any client has against a pruned node). Elektron Net's ~137-day window is generous for this in practice -- comfortably longer than a routine maintenance outage, but this becomes a real operational SLA the hub operator MUST plan around (monitoring/alerting on hub downtime, not just "restart it eventually").
- Bitcoind's configured prune target size MUST be at least ~25GB for Eclair's own startup validation to pass. This is a config value to set correctly on whichever `elektrond` instance the hub node points at, not a blocker.
- Eclair explicitly accepts that it "won't be able to validate all gossip messages" in pruned mode. Low-impact for Elektron Net specifically at this stage: `guideline-wallet-integration.md` SS3.3 already states no LN graph exists on this network yet, so there is minimal-to-no third-party gossip to validate in the first place.

**Conclusion: mandatory pruning does not block this deployment.** It does mean the hub node's `elektrond` backend SHOULD be a dedicated instance the hub operator directly controls uptime for (not a shared, best-effort node), and monitoring hub-node uptime against the pruning window SHOULD be an explicit operational requirement from day one.

## 5. Proposed Deployment Shape

Following this project's established pattern (see the electrs guideline SS1/checklist, and `elektron-net-stack`'s existing pre-wired-but-initially-disabled service profile for `elektron-electrs`):

- The Eclair fork SHOULD run as its own `docker-compose` service in `elektron-net-stack`, alongside the existing `elektrond` and `electrs` services, added as a new (initially disabled/opt-in) service profile mirroring how `elektron-electrs` was introduced.
- It SHOULD point at a **dedicated** `elektrond` instance (own RPC/ZMQ endpoints), not share the one `electrs` already depends on, so the hub operator has full, isolated control over that node's uptime and prune target size (SS4.2) independent of anything else on the stack.
- Eclair's JVM footprint is a real, non-trivial resource cost compared to Go-based LND -- worth sizing explicitly against whatever Hetzner instance class this gets deployed to, rather than assuming it fits the same footprint as `electrs`/`elektrond` did.
- Default LN P2P port (`9735` upstream) MAY be kept as-is (it's a Lightning-layer convention, not a chain-consensus parameter, and keeping it standard reduces operator confusion) unless a concrete conflict with something else on the same host requires changing it.

## 6. Checklist

- [ ] Resolve the BOLT11 HRP decision (SS3, blocking) -- coordinate with `elektron-net-electrum`'s own open item, don't decide independently
- [ ] Clone `ACINQ/eclair` and directly confirm/refine SS4.1's chain-identity findings against real source (not search-tool summaries) -- produces the actual patch-surface scope
- [ ] Decide and document the bech32 HRP override point(s) in the fork (on-chain address encode/decode for channel funding/closing)
- [ ] Decide BIP44 coin type usage for the hub's own on-chain wallet seed (independent decision from the end-user wallet's SLIP-44 precedent, SS4)
- [ ] Confirm RPC/ZMQ-only integration (no embedded P2P client) holds for whichever Eclair version is forked, so `pchMessageStart`/P2P port truly stay irrelevant (SS4)
- [ ] Provision a dedicated `elektrond` instance for the hub node, with prune target size set to at least Eclair's ~25GB minimum (SS4.2)
- [ ] Set up uptime monitoring for the hub node against the ~137-day pruning window as an explicit operational SLA (SS4.2)
- [ ] Add a pre-wired `docker-compose` service profile to `elektron-net-stack` (SS5)
- [ ] Add the resulting node's real `host`/`port`/`pubkey` as a new entry to `TRAMPOLINE_NODES_MAINNET` in `elektron-net-electrum` (`electrum/trampoline.py` already has a commented-out format example waiting for this)
- [ ] End-to-end test: open a trampoline channel and send a trampoline payment between two real wallets through this node

## 7. Open Questions

1. BOLT11 HRP: same open question already tracked in `guideline-wallet-integration.md` SS4/SS6 -- restated here because this doc cannot be finalized independently of that decision.
2. Should the initial hub node be operated privately (single trusted node, bootstrap phase) before being publicized, similar to how a young network's first routing nodes are typically seeded deliberately rather than announced immediately?
3. Is a second, independently operated trampoline node planned from the start (redundancy, matching `guideline-wallet-integration.md` SS3.3's "at least one, ideally several" framing), or is a single-node bootstrap the deliberate first step?
4. Does Eclair's own on-chain wallet functionality (for channel funding/closing) need to be exposed/managed separately from `elektrond`'s own wallet, or kept fully internal to the Eclair fork -- affects the docker-compose service boundary in SS5.
