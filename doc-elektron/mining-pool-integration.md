# Elektron Net — Mining Pool Integration Guide

**Version:** 4.0.1
**Date:** June 23, 2026
**Audience:** Pool operators, Stratum backend developers, integrators of any kind
**Reference implementation:** [`mining/miner.py`](../mining/miner.py) — treat this file as the ground truth
**See also:** [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](../WHITEPAPER.md) §4.3

> **READ THIS FIRST.** Most pool integration failures we have seen come from
> appending bytes to the coinbase `scriptSig`. If you remember nothing else
> from this document, remember the line below:
>
> ```python
> # Use the exact prefix from getblocktemplate so UTXO attestation matches.
> script_sig = bytes.fromhex(prefix_hex)
> ```
>
> Anything else in `scriptSig` — extranonce padding, pool tags, pad bytes —
> changes the coinbase txid and the node will reject your block with
> `bad-utxo-attestation`. There is no exception. This document explains why
> and how to integrate accordingly.

---

## 1. Who needs to change what?

| Role | Changes required? | Effort |
|------|-------------------|--------|
| **ASIC firmware** (Antminer, Whatsminer, NerdMiner, Bitaxe, …) | **None to the binary** — but see §3.7 about the Stratum settings the firmware sees | — |
| **ASIC operators** (worker config) | None — pool URL / worker name as usual | — |
| **Mining pool** (Stratum backend) | **Yes — substantial** | Coinbase construction + Stratum advertisement |
| **GBT miners** (cgminer, bfgminer, custom software) | **Yes** | Read `coinbase_required_outputs`, use `coinbase_script_sig_prefix` verbatim |
| **In‑node mining** (`generatetoaddress`, `miner.py`, `miner.cpp`) | None — already compliant | — |

ASICs only hash block headers. The **pool builds the coinbase** (or a
GBT‑capable miner does). Without the pool adjustments described here,
**every block your pool submits will be rejected**.

Elektron Net adds two consensus changes that affect pools:

- **Per‑block UTXO attestation** baked into the coinbase
- **60‑second blocks** with Stoic Awakening minimum‑difficulty escape

Standard Bitcoin pool software produces blocks that are invalid on
Elektron Net. Read §3 carefully.

---

## 2. Elektron‑specific GBT fields

`getblocktemplate` returns the standard Bitcoin fields plus the Elektron
extensions. Call it with at least `{"rules": ["segwit"], "coinbaseaddress": "<be1q…>"}`.

```json
{
  "version": 536870912,
  "previousblockhash": "...",
  "height": 42,
  "coinbasevalue": 500000000,
  "bits": "1d7fffff",
  "curtime": 1781164284,
  "transactions": [ ... ],
  "coinbase_required_outputs": [
    { "value": 0, "scriptPubKey": "6a022a0020<32-byte UTXO hash>" },
    { "value": 0, "scriptPubKey": "6a24aa21a9ed<32-byte witness root commitment>" }
  ],
  "coinbase_script_sig_prefix": "012a",
  "default_witness_commitment": "6a24aa21a9ed..."
}
```

### Fields the pool MUST use

| Field | What you do with it |
|-------|---------------------|
| `height` | Used for `nLockTime = height - 1` and (fallback) `scriptSig` |
| `coinbasevalue` | The full coinbase reward in leptons — goes into `vout[0]` |
| `coinbase_required_outputs` | **Append verbatim** to the coinbase, in array order, starting at `vout[1]` |
| `coinbase_script_sig_prefix` | **Use as the complete `scriptSig`**. Nothing before, nothing after. |
| `transactions` | Mempool txs for the Merkle tree (you may also choose to drop them — see §3.6) |
| `bits` | Difficulty (already includes Stoic Awakening) |
| `previousblockhash`, `curtime`, `version` | Standard header fields |

### Fields the pool MUST NOT do anything other than copy

`coinbase_required_outputs[i].scriptPubKey` and `coinbase_script_sig_prefix`
are byte strings the node has already committed to via the attestation hash.
**Do not modify, reorder, recompute, or augment them.** Copy from the JSON,
write into the coinbase, done.

---

## 3. Building the coinbase correctly

### 3.1 Required coinbase structure

```
vin[0]:
- prevout.hash:  32 × 0x00
- prevout.n:     0xFFFFFFFF
- scriptSig:     EXACTLY the `coinbase_script_sig_prefix` from GBT
                 (nothing appended — no extranonce, no pool tag, no padding)
- nSequence:     0xFFFFFFFE  (MAX_SEQUENCE_NONFINAL — required for the timelock)

vout[0]:         payout
                 - value:        coinbasevalue
                 - scriptPubKey: pay-to-address for the miner's payout

vout[1]:         coinbase_required_outputs[0] — the UTXO attestation OP_RETURN
                 (copy `value` and `scriptPubKey` verbatim from GBT)

vout[2]:         coinbase_required_outputs[1] — the witness commitment OP_RETURN
                 (copy `value` and `scriptPubKey` verbatim from GBT)

nLockTime:       height - 1   (Elektron consensus rule — not standard Bitcoin)

Witness on vin[0]: a single 32-byte stack item containing 32 × 0x00
                   (BIP141 coinbase witness reserved value)
```

Validate every produced block with `getrawtransaction <coinbase_txid> true <blockhash>`
before going to production.

### 3.2 The hard rule: scriptSig is EXACTLY the prefix

This is where almost every failing integration goes wrong. The node
computes the UTXO attestation hash against the coinbase whose `scriptSig`
is exactly `coinbase_script_sig_prefix`. **The same scriptSig must appear
in the block you submit**, byte for byte. There is no slack.

The Python reference implementation is unambiguous:

```python
# mining/miner.py — _build_coinbase_tx
prefix_hex = template.get('coinbase_script_sig_prefix')
if prefix_hex:
    # Use the exact prefix from getblocktemplate so UTXO attestation matches.
    script_sig = bytes.fromhex(prefix_hex)
else:
    script_sig = _script_num(height)
    if len(script_sig) < 2:
        script_sig += bytes([0x00])  # OP_0 — bad-cb-length guard
```

Note carefully what is **not** in this function:

- No `extranonce1` appended to `script_sig`
- No `extranonce2` appended to `script_sig`
- No pool identifier / coinbase tag
- No padding bytes for "Stratum slot reservation"

If you implement anything that adds bytes to `scriptSig`, the node will
recompute a different UTXO set hash than what was placed into
`coinbase_required_outputs[0]`. The block will be rejected with:

```
bad-utxo-attestation, UTXO attestation mismatch at height N:
  expected <hash_from_OP_RETURN>, got <recomputed_hash>
```

This is a hard consensus rule with no opt‑out, no configuration, and no
plan to relax it. Plan your Stratum layer around the fact that scriptSig
is fixed.

### 3.3 UTXO attestation (consensus)

- **Format:** `OP_RETURN <push: height> <push: 32-byte HASH_SERIALIZED>`
- The hash is computed by the node at template time over the post‑block
  UTXO state (excluding the coinbase's own attestation OP_RETURN, so
  there is no feedback loop).
- **The pool copies `vout[1].scriptPubKey` verbatim from GBT.** You do
  not, and cannot, compute the hash yourself — the node owns it.
- The hash is fresh per block. Reusing it across blocks (stale template)
  yields `bad-utxo-attestation`.

### 3.4 Witness commitment (BIP141, unchanged from Bitcoin)

- Standard BIP141 format, but for pool implementation purposes
  the node hands you the bytes via `coinbase_required_outputs[1]` and
  also (redundantly) as `default_witness_commitment`. Use the array
  entry; do not recompute.

### 3.5 Stratum v1 layer — required configuration

Standard Stratum v1 lets the worker iterate `extranonce2` and the pool
splice `extranonce1`, with both bytes ending up **inside `scriptSig`**.
On Elektron Net, **this breaks the UTXO attestation**. There is no way
to splice anything into `scriptSig` and have the node accept the block.

The only viable Stratum configuration for Elektron Net is therefore:

| Stratum field | Value | Effect |
|---|---|---|
| `extranonce1` (in subscribe response) | `""` (empty hex string) | Pool splices nothing |
| `extranonce2_size` (in subscribe response) | `0` | Worker iterates nothing in the coinbase |
| `coinb1` (in `mining.notify`) | The **complete** non‑witness coinbase serialization | Worker treats it as the coinbase |
| `coinb2` (in `mining.notify`) | `""` (empty hex string) | Nothing follows the worker's (empty) splice |

With both extranonce sizes set to `0`, the canonical Stratum equation

```
coinbase = coinb1 + extranonce1 + extranonce2 + coinb2
```

degenerates to

```
coinbase = coinb1
```

— which is **exactly** the bytes `miner.py` emits.

The worker's only sources of header‑search entropy are then:

- `nNonce` (32 bits)
- `nTime` rolling (small window, must stay above MTP and below network time + 2 h)
- BIP320 version‑rolling, if the worker negotiates `mining.configure`

For practical pool difficulties on Elektron Net (60 s blocks, current
network hash rate), this is more than enough. Push a fresh template on
every new tip and roughly every 30 s within a tip; that gives you ~2⁴⁸
search space per template per worker, which sustains modern ASICs.

### 3.6 What you must NOT do

These are the four ways we have seen pools "lose" blocks to
`bad-utxo-attestation`. Avoid each of them:

1. **Do not append an "extranonce hole" to `scriptSig`** to make
   classical Stratum `coinb1 + extranonce + coinb2` work. The 8 (or 4, or
   12) zero bytes you reserve there *are* part of `scriptSig`. The node
   recomputes the attestation against the actual bytes you submit, which
   include those zeros (or the worker's eventual extranonce overwrite).
   Either way the hash differs from the template's.
2. **Do not insert a pool identifier into `scriptSig`** (`/My Pool/`).
   This is a Bitcoin convention; on Elektron it breaks the attestation
   exactly like extranonce padding does.
3. **Do not reorder the coinbase outputs.** `vout[0]` is the payout,
   `vout[1..N]` are `coinbase_required_outputs` in exactly the order GBT
   delivers them. The node identifies the attestation by content, so
   reordering technically still validates the attestation — but Stratum
   `mining.notify` recipients and Merkle reconstruction depend on a
   stable layout. Keep template order.
4. **Do not reuse stale templates across tip changes.** The attestation
   hash changes every block. On a new tip, fetch a fresh GBT for every
   active worker and push a clean job with `clean_jobs=true`.

### 3.7 ASIC firmware compatibility caveat

A small number of ASIC firmwares hard‑require a **non‑empty** `extranonce1`
in the subscribe response and will close the TCP socket immediately on
an empty value. Two important points:

- The Elektron Net consensus rule is not negotiable. If the firmware
  truly requires non‑empty `extranonce1`, *and* the pool sends those
  bytes, *and* the firmware splices them into the coinbase, the resulting
  block will be rejected. There is no pool‑side trick that simultaneously
  satisfies the firmware's expectation and the node's attestation.
- For ASIC firmware that disconnects on empty `extranonce1`, either:
  - Update the firmware to one that respects `extranonce2_size = 0` and
    does not splice into scriptSig when `extranonce1` is empty (most
    modern firmwares already do this), or
  - Use GBT direct mining (`mining/miner.py`, `mining/miner.cpp`) for
    the workers that cannot be reconfigured.

Stratum v2 (BIP not yet ratified) makes header‑only mining a first‑class
mode and avoids this problem entirely; once firmwares ship Stratum v2
support, Elektron pools should prefer it.

---

## 4. Network parameters (mainnet v4.0)

| Parameter | Value |
|-----------|-------|
| Algorithm | SHA‑256d (same as Bitcoin) |
| P2P port | 8333 |
| RPC port | 8332 |
| Message start | `e1 ec 7a 6e` |
| Block time target | 60 s |
| Retarget interval | 2,016 blocks (~1.4 days) |
| `PROTOCOL_VERSION` | 70017 |
| Bech32 HRP | `be` |
| Block reward (start) | 5 ELEK |
| Stoic Awakening | from height 1, after > 120 s gap → `powLimit` |

Pool backend: connect to an **Elektron Net node** (not Bitcoin Core).

---

## 5. Implementation steps in a Bitcoin‑style pool codebase

The exact paths below assume the public‑pool / `elektron-net-pool`
NestJS layout, but the conceptual steps apply to any pool backend.

### 5.1 RPC client (`bitcoin-rpc.service.ts`)

- Add the `coinbaseaddress` parameter to every `getblocktemplate` call.
- Confirm both `coinbase_required_outputs` and `coinbase_script_sig_prefix`
  arrive in the response. If they are missing, your node is misconfigured
  or out of date.

### 5.2 Job / template service (`stratum-v1-jobs.service.ts`)

- Parse both `coinbase_required_outputs` and `coinbase_script_sig_prefix`
  into the in‑memory `IJobTemplate`.
- Trigger `clearJobs = true` whenever the tip advances.
- Recommended cadence: refresh on every tip event + a 30 s heartbeat
  refresh.

### 5.3 Coinbase builder (`MiningJob` / `stratum-v1.service.ts`)

This is the part that gets misimplemented most often. Mirror
`mining/miner.py:_build_coinbase_tx` line by line:

```ts
// PSEUDO-CODE — see MiningJob.ts in the reference implementation
const height = jobTemplate.blockData.height;

const scriptSig = jobTemplate.coinbase_script_sig_prefix;
// NOTHING ELSE goes in here. See §3.2.

const cb = new Transaction();
cb.version = 2;
cb.addInput(zero32, 0xFFFFFFFF, 0xFFFFFFFE);
cb.ins[0].script = scriptSig;
cb.ins[0].witness = [zero32];

cb.addOutput(payoutScript, coinbasevalue);
for (const out of jobTemplate.coinbase_required_outputs) {
    cb.addOutput(out.scriptPubKey, out.value);   // verbatim, in order
}

cb.locktime = height - 1;                        // NOT standard Bitcoin
```

Verify the produced coinbase serialization is byte‑identical to what
`mining/miner.py` produces for the same template + payout address.

### 5.4 Stratum wiring

- `mining.subscribe` response: `extranonce1 = ""`, `extranonce2_size = 0`
- `mining.notify`: `coinb1 = <full non-witness coinbase hex>`, `coinb2 = ""`
- `mining.submit` handling: ignore the `extranonce2` field positionally
  (worker may still send something); do not use it in coinbase
  reconstruction.

### 5.5 Block assembly & submission

When a worker submits a header that meets the network target:

1. Take the coinbase you already built for this template (it is
   immutable — the worker did not change it).
2. Recompute the merkle root from the coinbase's non‑witness txid and
   the template's `merkle_branch`.
3. Build the 80‑byte header with the worker's `nVersion`,
   `nTime`, `nNonce`.
4. Serialize the full block (header + tx count + coinbase + transactions).
5. Call `submitblock`.

A non‑null, non‑empty result string from `submitblock` is a rejection
reason. See §6 for the meanings.

### 5.6 Operational

- Bech32 HRP for mainnet payout addresses is `be`. Reject anything else
  on mainnet.
- **No dev fee / pool fee in the coinbase.** Elektron's attestation
  pins the coinbase to a single payout output and any output split would
  invalidate the template's attestation. Handle pool fees off‑chain
  (per‑payout deductions in the database, not in the coinbase).
- Per‑connection GBT calls (one per worker, every block) put nontrivial
  load on the node's RPC. Plan for `bitcoind`/`elektrond` capacity
  accordingly; tune `STRATUM_MAX_CONNECTIONS_PER_LISTENER` to match.

---

## 6. Common rejection codes and what they mean

| `submitblock` result | Meaning | Fix |
|---|---|---|
| `null` / `""` | Block accepted. | — |
| `missing-utxo-attestation` | `vout` of coinbase does not contain an `OP_RETURN <height> <32-byte hash>` for the current height. | Append `coinbase_required_outputs[0]` correctly. |
| `bad-utxo-attestation` | Hash in `vout[1]` does not match the post‑block UTXO state the node recomputes. | scriptSig differs from `coinbase_script_sig_prefix`, or you re‑used a stale template, or you reordered/modified `vout[1..N]`. See §3.2 and §3.6. |
| `bad-utxo-attestation-compute` | Transient internal error. | Refetch GBT and retry. |
| `bad-cb-length` | `scriptSig` is shorter than 2 bytes. | At heights < 17, append a single `0x00` (OP_0) after the BIP34 height push, as `miner.py` does. |
| `bad-cb-amount` | Coinbase outputs sum exceeds `coinbasevalue`. | Recompute `vout[0].value`. |
| `bad-txnmrklroot` | Header merkle root doesn't match block transactions. | Recompute merkle from your final coinbase txid + template `merkle_branch`. |

---

## 7. Test checklist

Run these in order. Each step must succeed before moving on.

1. **GBT shape**
   ```bash
   elektron-cli getblocktemplate '{"rules":["segwit"],"coinbaseaddress":"be1q…"}' \
     | jq '.coinbase_required_outputs, .coinbase_script_sig_prefix'
   ```
   You must see exactly two `coinbase_required_outputs` entries and a
   non‑empty `coinbase_script_sig_prefix`.

2. **Manual block via Python reference**
   ```bash
   python mining/miner.py --address be1q… --threads 1
   ```
   This will mine and submit a block. If it succeeds, your node setup
   is good. If it fails, fix the node before touching the pool.

3. **Pool produces identical coinbase**
   Build a coinbase from your pool code with the same template + payout
   address that `mining/miner.py` would use. Hex‑compare the
   non‑witness serializations — they must be byte‑identical.

4. **Pool produces identical scriptSig**
   Specifically grep the coinbase for any byte beyond
   `coinbase_script_sig_prefix` inside `vin[0].scriptSig`. There must
   be none.

5. **End‑to‑end Stratum**
   Connect a Stratum worker (`cgminer --userpass …`,
   `python -m bitaxe`, or a real ASIC). Verify `mining.notify` carries
   the full coinbase as `coinb1`. Verify the worker accepts subscribe
   with `extranonce2_size = 0`.

6. **Real block acceptance**
   Wait for a worker submit that meets the network target. Verify
   `submitblock` returns `null`. Inspect the on‑chain block:
   ```bash
   elektron-cli getrawtransaction <coinbase_txid> true <blockhash>
   ```
   `vout` must show payout, then the UTXO attestation OP_RETURN, then
   the witness commitment, in that order.

---

## 8. Stoic Awakening (no pool patch needed)

If `> 120 s` have passed since the previous block, the node sets `bits`
to `powLimit` in the next template. The pool forwards `bits` unchanged
to the worker. No special handling.

---

## 9. Difficulty at chain start

Genesis was mined at `nBits = 0x1d7fffff` (about 200× easier than Bitcoin
genesis). The first retarget is at height 2,016. Until then expect
CPU‑mineable difficulty; pools should set worker `mining.set_difficulty`
accordingly to keep share rates reasonable.

---

## 10. Recommendation: small miners first

This is operational guidance, not a consensus rule:

- Stage public Stratum endpoints after the initial CPU‑solo phase.
- Restrict early `getblocktemplate` access by API key.
- Announce the public pool launch transparently so early miners aren't
  surprised.

Solo via GBT works immediately and is the recommended bootstrap path:

```bash
python mining/miner.py --address be1q… --threads 8 --continuous
elektron-cli generatetoaddress 1 "be1q…"
```

---

## 11. Migration checklist (Bitcoin pool → Elektron Net pool)

Hard requirements, all of which must be true before you start accepting
real workers:

- [ ] Elektron Net node (≥ v4.0) as backend, fresh chain
- [ ] GBT call uses `coinbaseaddress` per worker
- [ ] Coinbase `scriptSig` = `coinbase_script_sig_prefix`, byte for byte
- [ ] **No extranonce padding, no pool tag, no extra bytes in `scriptSig`**
- [ ] `vout[0]` payout, `vout[1..]` `coinbase_required_outputs` in array order
- [ ] `nLockTime = height - 1`
- [ ] `nSequence = 0xFFFFFFFE`
- [ ] Coinbase witness = single 32‑byte zero item on `vin[0]`
- [ ] Merkle root recomputed from final coinbase non‑witness txid
- [ ] Stratum advertises `extranonce1 = ""`, `extranonce2_size = 0`
- [ ] `mining.notify` sends full non‑witness coinbase as `coinb1`,
      empty `coinb2`
- [ ] No dev/pool fee output in coinbase (fees are off‑chain)
- [ ] `PROTOCOL_VERSION` 70017
- [ ] All seven steps in §7 pass on a private regtest before mainnet rollout

---

## 12. Reference files

| Topic | File |
|-------|------|
| **Reference miner (Python)** — the ground truth for coinbase layout | [`mining/miner.py`](../mining/miner.py) |
| Reference miner (C++) — same logic in C++ | [`mining/miner.cpp`](../mining/miner.cpp) |
| GBT output (node) | `src/rpc/mining.cpp` |
| Coinbase build order (node) | `src/node/miner.cpp` |
| Attestation validation | `src/validation.cpp` |
| Technical diff vs Bitcoin | [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md) |

When in doubt, compare against `mining/miner.py`. It is small, it is
canonical, and it is verified against the node by every release test.
If your pool produces a different coinbase serialization than the
Python miner for the same template, your pool is wrong.

---

## Appendix A — Why §3.5 in the previous doc revision was misleading

Earlier versions of this document described the conventional Stratum v1
arrangement (`extranonce` lives inside `scriptSig`) without explicitly
noting that it is **incompatible with Elektron Net's UTXO attestation**.
That phrasing led at least one pool integrator to reserve an extranonce
hole inside `scriptSig`, which broke the attestation on every submitted
block.

To prevent this from happening again:

- §3.2 of this revision states the rule in absolute terms: scriptSig is
  exactly the GBT prefix, period.
- §3.5 spells out the Stratum configuration that follows from §3.2:
  `extranonce_size = 0` on both sides, full coinbase in `coinb1`.
- §3.6 explicitly lists "do not append extranonce padding to scriptSig"
  as the first pitfall.
- §6 maps `bad-utxo-attestation` directly to "scriptSig differs from
  `coinbase_script_sig_prefix`" so a debugging operator finds the cause
  immediately.

The conceptual mistake is easy to make because Bitcoin pools have done
the "extranonce inside scriptSig" thing for a decade. Elektron Net
trades that flexibility for the per‑block UTXO attestation. There is no
way to keep both.
