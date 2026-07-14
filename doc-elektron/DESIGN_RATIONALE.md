# Design Rationale: 137 Days, 60 Seconds

*The philosophy behind Elektron Net's two active time constants — and one
retired experiment.*

---

## Introduction

Two numbers shape almost everything about how Elektron Net feels, behaves,
and stays secure:

- **60 seconds** — the heartbeat of the chain (block interval)
- **137 days** — the limit of memory (mandatory prune depth, 197,280 blocks)

Each number on its own looks like a tuning parameter. Together they form a
coherent design that trades the archival, industrial Bitcoin of today for
something closer to Satoshi's original vision: a peer-to-peer cash system
with privacy built in by the simple act of forgetting.

A third number, **120 seconds**, was part of this design from genesis
through block height 150,000: the *Stoic Awakening* min-difficulty window,
discussed in its own section below along with why it was retired. It no
longer shapes mainnet behavior today, but it shaped the chain's first
150,000 blocks, and it shaped what the team learned about designing
liveness mechanisms — so it stays in this document rather than being
deleted from it.

This document explains the *why*. The *how* lives in
[`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md), the *philosophy* in
[`WHITEPAPER.md`](../WHITEPAPER.md), and the *audit* in
[`AUDIT_PRUNING_SNAPSHOT.md`](AUDIT_PRUNING_SNAPSHOT.md).

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

## 120 Seconds — The Open Hand (retired at height 150,000)

If 60 seconds is the heartbeat and 137 days is the memory, 120 seconds was
meant to be the gesture of welcome — an "open hand" that occasionally let a
solo miner with a Bitaxe or NerdMiner win a block at minimum difficulty when
the network went quiet for a moment, without taking anything away from the
professional pools carrying the chain's regular security.

The idea, in short: **if more than 120 seconds passed since the last block,
the next one could be mined at minimum difficulty**, then the timer reset.
Not a subsidy, not a sustained low-difficulty period — in theory, one block
of relief per slow gap, immediately followed by a return to normal
difficulty. The reasoning is preserved in full in the retirement changelog,
since it's worth understanding *why* the idea seemed sound before seeing why
it didn't hold up.

### Why it was retired

Reality didn't match the model. Block discovery is a Poisson process, so a
>120s gap happens by pure statistical chance roughly **13.5% of the time**
even at perfectly stable hashrate — not the ~5% the original design assumed.
Worse, when one of these "relief" blocks happened to land on the *last*
block of a 2,016-block retarget period, the standard difficulty formula
inherited its crashed value as the baseline for the *entire next epoch*,
depressing difficulty for days rather than one block — during which the
chain was trivially attackable and opportunistic miners piled in to harvest
near-free rewards, which kept prolonging the crash rather than letting it
recover. Live mainnet data confirmed this exact failure mode in practice.

The mechanism was retired on mainnet at block height **150,000**
(`Consensus::Params::StoicAwakeningEndHeight`, `src/pow.cpp`). Difficulty
on Elektron Net now adjusts purely through the standard 2,016-block
retarget, same as Bitcoin. See
[`CHANGELOG-stoic-awakening-retirement.md`](CHANGELOG-stoic-awakening-retirement.md)
for the full technical account, including the live evidence that led to the
decision.

### What we're taking from it

A good intention — give small, honest hardware a real chance — collided
with a statistical reality that wasn't modeled carefully enough before
shipping to mainnet. The fix wasn't to patch the threshold and hope; it was
to remove the mechanism and be plain about why. That is the more
consequential "stoic" lesson here: not the 120-second window itself, but
owning what didn't work instead of quietly forgetting it.

---

## The Synthesis — Why the Constants Need Each Other

Each constant in isolation is interesting. Together they form an
architecture.

- **60 seconds** makes mining frequent enough that solo participation is
  meaningful — and confirmation latency low enough to feel like a live
  payment network rather than a settlement layer.

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

*(The points below rest on 60-second blocks, mandatory pruning, and
per-block attestation — not on Stoic Awakening, which is retired. Solo
hobbyist participation is still real and still distributed, but without the
120-second window a lone Bitaxe competes at full difficulty like any other
miner, same as on Bitcoin.)*

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

## The Gold-Panner — an image that didn't survive contact with reality

There is an older image that captures what Stoic Awakening was trying to do,
worth keeping precisely because the mechanism it describes no longer exists.

Before industrial mining came to the American West, the early gold rushes
were prospectors with pans, sluices, and stubborn optimism. The Colorado
rivers were full of strangers who built small towns, traded news, watched
each other's claims, and occasionally — not often, but occasionally —
found a nugget. The economy of those rivers was not concentrated. It
spread itself across the continent precisely because the work was small
enough for individuals.

Industrial mining made that economy obsolete. The same has happened to
Bitcoin: the romance of running a node and finding a block is now mostly
just romance. Stoic Awakening's 120-second window was meant to put the
panners back at the river — a Bitaxe under a desk finding, every so often,
a block nobody expected it to find.

It didn't work that way in practice. The window opened far more often than
intended, and each opening risked dragging the whole river dry for days
rather than handing out the occasional nugget — see the section above and
[`CHANGELOG-stoic-awakening-retirement.md`](CHANGELOG-stoic-awakening-retirement.md).
Retiring it at height 150,000 was the honest response once the data was in.

The image stays in this document anyway. Not every good idea survives
contact with a live network, and pretending otherwise would be its own kind
of dishonesty. **Elektron Net still values the gold-panner** — it just
doesn't currently believe minimum-difficulty windows are how you protect
one.