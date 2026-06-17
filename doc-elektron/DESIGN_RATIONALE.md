# Design Rationale: 137 Days, 120 Seconds, 60 Seconds

*The philosophy behind the three time constants of Elektron Net.*

---

## Introduction

Three numbers shape almost everything about how Elektron Net feels, behaves,
and stays secure:

- **60 seconds** — the heartbeat of the chain (block interval)
- **120 seconds** — the open hand for small miners (Stoic Awakening window)
- **137 days** — the limit of memory (mandatory prune depth, 197,280 blocks)

Each number on its own looks like a tuning parameter. Together they form a
coherent design that trades the archival, industrial Bitcoin of today for
something closer to Satoshi's original vision: a peer-to-peer cash system
secured by many small, individual participants, with privacy built in by the
simple act of forgetting.

This document explains the *why*. The *how* lives in
[`BITCOIN_CORE_DIFF.md`](../doc/BITCOIN_CORE_DIFF.md), the *philosophy* in
[`WHITEPAPER.md`](../WHITEPAPER.md), and the *audit* in
[`AUDIT_PRUNING_SNAPSHOT.md`](../doc/AUDIT_PRUNING_SNAPSHOT.md).

---

## 60 Seconds — The Heartbeat

Bitcoin targets a block every 600 seconds (10 minutes). Elektron Net targets
one every 60. Code reference: `nPowTargetSpacing = 60` in
`src/kernel/chainparams.cpp`.

### Why faster

A 10-minute block was a reasonable choice for a system that had to prove
itself in 2009. Today, when people pay for coffee with their phones, waiting
ten minutes for the first confirmation is a museum piece. At 60 seconds:

- First confirmation in a minute, six confirmations in six.
- 1,440 blocks per day (vs. Bitcoin's 144).
- The chain feels alive — block explorers update like a pulse.

### What stays the same

- **Total supply** is unchanged: `MAX_MONEY = 21,000,000 * COIN`.
- **Halving cadence** is rescaled to `nSubsidyHalvingInterval = 2,102,400`
  — same ~4 calendar years, just expressed in more blocks.
- **Smallest unit** is still 10⁸ per coin (called *lepton* here, instead of
  satoshi).

### The hidden consequence

Faster blocks create more frequent mining opportunities. That alone wouldn't
matter — Litecoin and Dogecoin already proved that. But combined with the
other two numbers, 60 seconds becomes the foundation of something bigger.

---

## 137 Days — The Memory

Constant: `MANDATORY_PRUNE_DEPTH = 197280` in `src/validation.h:79`.
At 60-second blocks, that's almost exactly 137 days.

### Why a hard limit, and not "optional pruning"

Bitcoin lets the user choose: keep the full archive, or prune to save space.
Elektron Net makes the choice for everyone: **all nodes prune to the last
137 days**. There is no full-archive mode. The user's `-prune=<GB>` setting
is silently ignored (`src/node/blockmanager_args.cpp`).

This is not a storage optimization. It is a **statement about what a public
ledger should be**.

#### The "Pocket" model

A wallet should be like a pocket — light enough to carry, holding what's
useful, forgetting what isn't. The right to be forgotten is not an
afterthought; it's the first principle. If the network cannot serve old
transactions, no third party can dig them up either. Forensic
de-anonymization tools depend on archival history. Without it, the chain
itself protects its users.

#### What is preserved, what is lost

| Preserved forever | Lost after 137 days |
|---|---|
| The current balance of every address | When a UTXO was received |
| Spendability of every coin | From whom it was received |
| The UTXO commitment of every checkpoint | The full transaction graph |
| Proof-of-work chain of headers | Per-transaction forensic trail |

Crucially: **balances are not lost when blocks are pruned.** The UTXO set
in `chainstate/` is a *cumulative* database. An unspent output created in
block 100 stays in the UTXO set until it is spent, even if block 100 itself
was deleted a year ago. The snapshot mechanism (see below) captures this
complete state at every checkpoint, so newcomers can join the network with
full and current knowledge of every balance.

#### Implicit finality

A 51% attacker on Bitcoin can, in theory, rewrite an arbitrarily deep
section of history given enough hashrate and time. On Elektron Net, after
137 days the blocks are simply gone. A reorg that deep is *physically
impossible* because no honest node has the data to participate. The chain
becomes finalized by erasure, not by checkpoint consensus.

---

## 120 Seconds — The Open Hand

If 60 seconds is the heartbeat and 137 days is the memory, 120 seconds is
the gesture of welcome.

The rule (Stoic Awakening, mainnet, active from height 1):
**if more than 120 seconds have passed since the last block, the next block
may be mined at the minimum difficulty.**

Code: `MinDifficultyActivationHeight = 1` in `src/kernel/chainparams.cpp`,
implementation in `src/pow.cpp`.

### What it actually does

It is *one* block of relief, then the timer resets. The next block after
that is back to normal difficulty unless 120 seconds pass again. This is
not a sustained low-difficulty period — it is a single open window, and it
only opens when the network slows down.

### Why it is not a security hole

A 51% attacker cannot use this to "chain-rush" — every min-difficulty block
needs another 120-second gap, which means waiting in real time. The
chainwork gained is marginal.

But for a Bitaxe in someone's kitchen, those 120 seconds are everything.

### The hand-up for small miners

Modern small mining devices — Bitaxe, NerdMiner, hobby ASICs in the
500 GH/s to 5 TH/s range — have essentially zero chance of finding a
Bitcoin block. The math is brutal: a single device against a global
exahash network is a lottery with no winners.

On Elektron Net, when the 120-second window opens, the difficulty drops to
`powLimit`. A solo Bitaxe can realistically find that block. Not often —
but realistically. The lottery has winners now.

### The barbell

This produces a deliberately asymmetric miner population:

- **Professional pools and data centres** mine the ~95% of blocks that
  appear within 120 seconds, at full difficulty. They carry the cost and
  carry the security.
- **Solo adventurers** with Bitaxes, NerdMiners, and basement rigs catch
  the blocks that fall through the 120-second window.

Nobody is displaced. Professionals are not undercut — they still win the
vast majority of blocks and the steady income. Hobbyists are not excluded
— they have a real, non-zero chance every time the network breathes.

### The energy ceiling

Bitcoin's energy use grows because difficulty grows because hashrate
grows. The arms race has no built-in brake. On Elektron Net, if difficulty
rises so high that even professional miners regularly take more than 120
seconds per block, the overflow falls to solo miners at minimum
difficulty. This acts as an **implicit safety valve** against unbounded
hashrate escalation. Over long horizons, the system self-regulates toward
a hashrate level where the arms race is no longer economically attractive.

This is not proof-of-work versus proof-of-stake. It is proof-of-work
**with self-limitation built in.**

---

## The Synthesis — Why the Three Numbers Need Each Other

Each constant in isolation is interesting. Together they form an
architecture.

- **60 seconds** makes mining frequent enough that solo participation is
  meaningful. At 600-second blocks, a Bitaxe owner would wait years
  between meaningful chances even with min-difficulty windows. At 60
  seconds, the chances arrive constantly.

- **120 seconds** opens the door for small miners *without* taking
  anything from large miners. The big operators still win when blocks
  come fast (most of the time). The small operators win when blocks come
  slow.

- **137 days** keeps the chain light enough that a Raspberry Pi, a phone,
  or a low-end laptop can be a full node. Light chains attract more
  nodes. More nodes mean more verification, more diversity, more
  resilience.

- **Per-block UTXO attestation** stitches it all together
  cryptographically. Every block commits, in its coinbase, to the hash
  of the UTXO set after that block. A new node bootstrapping from a
  snapshot does not have to trust the snapshot's author — it can verify
  the snapshot against the on-chain commitment, which is secured by all
  the proof-of-work backing the chain. (Code: `ValidateUTXOCheckpoint`,
  `src/validation.cpp:2920`; `WriteAutomaticSnapshot`,
  `src/validation.cpp:2439`.)

Pull on any one of these threads and the others come along.

---

## 51% Attacks — What This Architecture Changes

A 51% attack is still possible in principle — proof-of-work makes no other
promise. But the *shape* of the threat changes in ways that matter.

### What is harder

- **Renting hashrate.** Solo miners are not on NiceHash. They are in
  kitchens, garages, workshops. An attacker cannot simply purchase enough
  hashrate to overpower the chain — much of the honest hashrate is
  geographically and operationally distributed in places no marketplace
  reaches.

- **Geographic concentration.** Industrial mining tends toward a handful
  of regulatory regimes and a handful of cheap-electricity locations.
  Hobbyist mining lives everywhere — across power grids, ISPs, and
  jurisdictions. Confiscating or coercing that hashrate would mean
  knocking on a million doors.

- **Deep reorgs.** Past 137 days, no honest node holds the data needed to
  validate an alternative chain. Reorgs are bounded by erasure.

- **Snapshot poisoning.** A malicious peer cannot feed a new node a
  fabricated UTXO set, because the snapshot must hash-match the
  on-chain coinbase attestation. To forge a snapshot, the attacker would
  have to re-mine the checkpoint block and every block after it.

### What remains the same

- Short-range double-spends within the recent window are as feasible as
  on any proof-of-work chain.
- A young chain with low total hashrate is vulnerable, regardless of
  pruning model. This is a launch-phase risk, not a design flaw.

### What is structurally improved

A 51% adversary on Bitcoin must out-spend a few large pools. A 51%
adversary on a mature Elektron Net must out-spend a global swarm of
individuals who, by selection, are running their devices for reasons
other than pure profit. Ideologically motivated hashrate is sticky
hashrate. It is the hardest kind of hashrate to buy.

---

## The Gold-Panner

There is an older image that captures what this design is trying to do.

Before industrial mining came to the American West, the early gold rushes
were prospectors with pans, sluices, and stubborn optimism. The Colorado
rivers were full of strangers who built small towns, traded news, watched
each other's claims, and occasionally — not often, but occasionally —
found a nugget. The economy of those rivers was not concentrated. It
spread itself across the continent precisely because the work was small
enough for individuals.

Industrial mining made that economy obsolete. The same has happened to
Bitcoin: the romance of running a node and finding a block is now mostly
just romance.

Elektron Net puts the panners back at the river.

A Bitaxe under a desk is a pan in a stream. Most days it finds nothing.
But the stream is moving, the 120-second window keeps opening, and every
so often someone in Stuttgart or São Paulo or Sapporo will check their
device in the morning and find a block.

**Everyone, eventually, finds a nugget.**

That is not just a feature. It is the security model.