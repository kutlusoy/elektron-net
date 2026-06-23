# Elektron Net — Mining Pool Integration Guide

**Version:** 4.0.1 

**Date:** June 23, 2026 

**Audience:** Pool operators, Stratum backend developers, integrators of any kind 

**Reference implementation:** [`mining/miner.py`](../mining/miner.py) — treat this file as the ground truth 

**Reference pool:** [`elektron-net-pool`](https://github.com/kutlusoy/elektron-net-pool) — `MiningJob.ts` mirrors `miner.py` byte for byte 

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

> **NEW IN 4.0.1 — hobby-miner compatibility.** The previous revision claimed
> there was no pool-side way to keep firmwares that reject empty `extranonce1`
> (NerdMiner V2, Bitaxe, NerdAxe, ESP-Miner, …) connected without breaking the
> UTXO attestation. That was wrong. The two Stratum slots can be decoupled:
> advertise a **non-empty `extranonce1`** so the firmware accepts the subscribe
> reply, while keeping **`extranonce2_size = 0`** so the worker iterates
> nothing and cannot splice into `scriptSig`. The coinbase the pool actually
> submits is still built from `coinbase_script_sig_prefix` verbatim, so
> attestation is unaffected. See §3.5 and §3.7. The reference pool ships this
> behaviour by default.

---

## 1. Who needs to change what?

| Role | Changes required? | Effort |
|------|-------------------|--------|
| **ASIC firmware** (Antminer, Whatsminer, NerdMiner, Bitaxe, …) | **None to the binary** — the pool now advertises a non-empty `extranonce1` so even strict firmwares stay connected (§3.7) | — |
| **ASIC operators** (worker config) | None — pool URL / worker name as usual | — |
| **Mining pool** (Stratum backend) | **Yes — substantial** | Coinbase construction + Stratum advertisement (§3, §5) |
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

The viable Stratum configuration for Elektron Net therefore decouples
the two extranonce slots and treats them differently:

| Stratum field | Value | Effect |
|---|---|---|
| `extranonce1` (in subscribe response) | **Non‑empty random hex** (4 bytes / 8 hex chars, per‑session) | Cosmetic / session id only — pool **never splices it into the coinbase**. Required because some firmwares disconnect on empty (§3.7). |
| `extranonce2_size` (in subscribe response) | **`0`** | Worker iterates nothing in the coinbase. This is what actually protects the attestation. |
| `coinb1` (in `mining.notify`) | The **complete** non‑witness coinbase serialization | Worker treats it as the coinbase |
| `coinb2` (in `mining.notify`) | `""` (empty hex string) | Nothing follows the worker's (empty) splice |

With `extranonce2_size = 0`, the canonical Stratum equation

```
coinbase = coinb1 + extranonce1 + extranonce2 + coinb2
```

degenerates on the worker side to

```
coinbase = coinb1
```

because the worker never iterates and produces zero bytes for both
extranonce slots — regardless of what string was sent as `extranonce1`
in the subscribe response. **The pool must never read the value of
`extranonce1` back from the subscribe response and splice it into the
coinbase it serializes.** It is a session label on the wire, nothing more.
The coinbase the pool actually submits is exactly the bytes `miner.py`
emits.

**Why this works without breaking consensus:** the byte string sent in
the `extranonce1` slot of the subscribe response is *only* used by the
worker firmware in the formula above. When `extranonce2_size = 0` and
the pool's coinbase builder ignores `extranonce1`, the value has no
path into `scriptSig`. It functions purely as a session/notify channel
identifier. UTXO attestation is therefore preserved.

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
5. **Do not advertise a non-zero `extranonce2_size`** to "stay
   Stratum-compatible". The moment the worker starts iterating an
   extranonce2 of any length, the coinbase mutates per iteration and
   `bad-utxo-attestation` returns. Use the §3.5 decoupling instead:
   non-empty `extranonce1` for firmware acceptance, `extranonce2_size = 0`
   for consensus safety.
6. **Do not feed the wire-level `extranonce1` value back into your
   coinbase builder.** It is a session label, not coinbase material.
   The reference pool emits a random 4-byte hex string per connection
   for the subscribe response and the coinbase builder ignores that
   string entirely.

### 3.7 ASIC and hobby-miner firmware compatibility

ESP32-class hobby miners (NerdMiner V2, Bitaxe, NerdAxe, NerdQAxe,
ESP-Miner) and a handful of older ASIC firmwares **hard-require a
non-empty `extranonce1`** in the subscribe response. Sending an empty
string causes them to close the TCP socket immediately after subscribe,
which manifests in pool logs as a tight loop:

```
New client ID: , userAgent=NerdMiner, ::ffff:192.168.x.x:nnnnn
Client disconnected, hadError?:false
New client ID: , userAgent=NerdMiner, ::ffff:192.168.x.x:nnnnn+1
Client disconnected, hadError?:false
...
```

(Empty session id, then instant disconnect, repeated per reconnection
attempt.)

**This is fixable on the pool side alone, without touching the
firmware or the consensus rule.** The fix is exactly the §3.5
decoupling:

- Generate a per-connection random 4-byte hex string (8 hex chars).
- Send it as the `extranonce1` slot of the subscribe response **and** as
  the `mining.notify` channel id.
- Keep `extranonce2_size = 0`.
- Build the coinbase from `coinbase_script_sig_prefix` verbatim, with no
  reference to that hex string.

The firmware sees a non-empty `extranonce1`, accepts the subscribe
reply, and starts mining. Because `extranonce2_size = 0` it iterates no
extranonce — so the coinbase the pool serializes for `submitblock` is
byte-identical to what `miner.py` would produce, and the node accepts
the block.

**This is the reference pool's default behaviour as of
`elektron-net-pool` June 2026.** Operators inheriting older pool builds
should verify that:

- `subscribe` reply has a non-empty `extranonce1` (look at the second
  string in `result`, not just the notify channel id).
- `subscribe` reply has `extranonce2_size = 0` (third entry in `result`).
- The coinbase serializer reads from `coinbase_script_sig_prefix` and
  not from any per-session value.

A useful operational tell: each new connection should produce a log line
of the form `New client ID: <8 hex chars>, userAgent=..., mode=HOBBY|NORMAL`.
If the hex string is empty, the pool is on the broken pre-4.1 wiring
and hobby miners will not stay connected.

For pools that want fine-grained control, the reference implementation
exposes:

| Env var | Default | Purpose |
|---|---|---|
| `HOBBY_MINER_USER_AGENTS` | `NerdMiner,NerdminerV2,nerdminer,Bitaxe,NerdAxe,NerdQAxe,ESP-Miner` | Substring allow-list (case-insensitive) used to mark a session as hobby-class. |
| `HOBBY_MINER_DIFFICULTY` | `0.001` | Starting `mining.set_difficulty` for matched sessions — ESP32 devices at a few tens of kH/s need this low so shares actually arrive before the pool's dead-client timer fires. |

Sessions whose `userAgent` does not match the list still benefit from
the §3.5 wiring (modern ASICs accept non-empty `extranonce1` too); the
allow-list only affects the starting difficulty.

Stratum v2 (BIP not yet ratified) makes header-only mining a
first-class mode and avoids any of this. Once firmwares ship Stratum v2
support, Elektron pools should prefer it, but Stratum v1 with the §3.5
decoupling is already a full solution for the v1 firmware fleet.

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

The exact paths below assume the `elektron-net-pool` NestJS layout, but
the conceptual steps apply to any pool backend.

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
// In particular: do NOT read the per-session extranonce1 value from
// the subscribe response into this builder. extranonce1 is wire-only.

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

- `mining.subscribe` response:
  - `extranonce1` = a **non-empty** per-session random hex string
    (4 bytes / 8 hex chars in the reference pool — see `stratum.constants.ts`,
    `SUBSCRIBE_SESSION_ID_BYTES`).
  - `extranonce2_size` = `0`.
  - `mining.notify` subscription id = the same hex string (so the worker
    has a stable channel tag across `mining.set_difficulty` updates).
- `mining.notify`: `coinb1 = <full non-witness coinbase hex>`, `coinb2 = ""`.
- `mining.submit` handling: ignore the `extranonce2` field positionally
  (worker may still send something, e.g. empty string); do not use it
  in coinbase reconstruction.
- The per-session `extranonce1` value **must never be read into the
  coinbase builder**. Keep it as a connection-scoped session identifier
  and use it only for logging, dedup, and the wire-level fields above.

### 5.5 Block assembly & submission

When a worker submits a header that meets the network target:

1. Take the coinbase you already built for this template (it is
   immutable — the worker did not change it, because `extranonce2_size = 0`).
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
  load on the node's RPC. Plan for `elektrond` capacity accordingly;
  tune `STRATUM_MAX_CONNECTIONS_PER_LISTENER` to match.
- For hobby-miner fleets, set `HOBBY_MINER_DIFFICULTY` low enough that
  a typical device (a NerdMiner V2 around 50 kH/s, a Bitaxe Ultra
  around 500 GH/s) submits at least one share per 60 s block. Diff
  `0.001` is the reference default and works for both.
- Log line discipline: emit `mode=HOBBY|NORMAL` on subscribe and on
  every `mining.submit` so operators can confirm at a glance which
  code path each device follows.

---

## 6. Common rejection codes and what they mean

| `submitblock` result | Meaning | Fix |
|---|---|---|
| `null` / `""` | Block accepted. | — |
| `missing-utxo-attestation` | `vout` of coinbase does not contain an `OP_RETURN <height> <32-byte hash>` for the current height. | Append `coinbase_required_outputs[0]` correctly. |
| `bad-utxo-attestation` | Hash in `vout[1]` does not match the post‑block UTXO state the node recomputes. | scriptSig differs from `coinbase_script_sig_prefix` (most common: extranonce was spliced in because `extranonce2_size > 0` or the pool fed `extranonce1` into the coinbase), or you re‑used a stale template, or you reordered/modified `vout[1..N]`. See §3.2, §3.5 and §3.6. |
| `bad-utxo-attestation-compute` | Transient internal error. | Refetch GBT and retry. |
| `bad-cb-length` | `scriptSig` is shorter than 2 bytes. | At heights < 17, append a single `0x00` (OP_0) after the BIP34 height push, as `miner.py` does. |
| `bad-cb-amount` | Coinbase outputs sum exceeds `coinbasevalue`. | Recompute `vout[0].value`. |
| `bad-txnmrklroot` | Header merkle root doesn't match block transactions. | Recompute merkle from your final coinbase txid + template `merkle_branch`. |

In addition, the following **non-consensus** symptom is worth listing
here because it is the single most reported pool integration problem
and is not a `submitblock` rejection at all:

| Symptom | Meaning | Fix |
|---|---|---|
| Hobby miner (NerdMiner / Bitaxe / NerdAxe / ESP-Miner) connects, logs `userAgent=...`, then disconnects within ~10 ms — repeated forever | Subscribe reply contains an empty `extranonce1` slot. The firmware rejects the reply and closes the socket. | Apply §3.5 / §3.7 — send a non-empty random per-session hex string as `extranonce1`, keep `extranonce2_size = 0`. |

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

5. **Subscribe reply shape**
   Capture a real `mining.subscribe` exchange (`tcpdump` or a Stratum
   log) and confirm the reply matches:
   ```json
   {"id":N,"error":null,"result":[[["mining.notify","<8 hex>"]],"<8 hex>",0]}
   ```
   The two `<8 hex>` values must be the same per session, both
   non-empty, and the trailing integer must be `0` (extranonce2_size).

6. **Hobby-miner connectivity (only if you operate hobby miners)**
   Point a real NerdMiner V2 or Bitaxe at the pool. The pool log must
   show:
   ```
   New client ID: <8 hex>, userAgent=NerdMiner, mode=HOBBY, ::ffff:...
   mining.notify -> <8 hex> job=... height=... diff=0.001 ...
   mining.submit <- <8 hex> mode=HOBBY job=... ntime=... nonce=...
   ```
   No immediate disconnect, shares arriving within ~minutes.

7. **End‑to‑end Stratum**
   Connect a Stratum worker (`cgminer --userpass …`,
   `python -m bitaxe`, or a real ASIC). Verify `mining.notify` carries
   the full coinbase as `coinb1`. Verify the worker accepts subscribe
   with `extranonce2_size = 0`.

8. **Real block acceptance**
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
- [ ] Stratum subscribe reply advertises **non-empty** per-session
      `extranonce1` (8 hex chars recommended) and `extranonce2_size = 0`
- [ ] Coinbase builder ignores the wire-level `extranonce1` value
      (it is session metadata, not coinbase material)
- [ ] `mining.notify` sends full non‑witness coinbase as `coinb1`,
      empty `coinb2`
- [ ] Hobby-miner allow-list configured (`HOBBY_MINER_USER_AGENTS`)
      with a sensibly low `HOBBY_MINER_DIFFICULTY` (≤ 0.001 for
      ESP32-class devices)
- [ ] Subscribe and submit log lines carry a `mode=HOBBY|NORMAL` tag
- [ ] No dev/pool fee output in coinbase (fees are off‑chain)
- [ ] `PROTOCOL_VERSION` 70017
- [ ] All eight steps in §7 pass on a private regtest before mainnet rollout
- [ ] At least one real NerdMiner or Bitaxe verified to stay connected
      and submit shares against staging before production rollout

---

## 12. Reference files

| Topic | File |
|-------|------|
| **Reference miner (Python)** — the ground truth for coinbase layout | [`mining/miner.py`](../mining/miner.py) |
| Reference miner (C++) — same logic in C++ | [`mining/miner.cpp`](../mining/miner.cpp) |
| **Reference pool coinbase builder** — TypeScript 1:1 mirror of `miner.py` | [`elektron-net-pool/src/models/MiningJob.ts`](https://github.com/kutlusoy/elektron-net-pool/blob/main/src/models/MiningJob.ts) |
| **Reference pool Stratum wiring** — subscribe/notify/submit handlers | [`elektron-net-pool/src/models/StratumV1Client.ts`](https://github.com/kutlusoy/elektron-net-pool/blob/main/src/models/StratumV1Client.ts) |
| **Reference pool subscribe response** — non-empty `extranonce1`, `extranonce2_size = 0` | [`elektron-net-pool/src/models/stratum-messages/SubscriptionMessage.ts`](https://github.com/kutlusoy/elektron-net-pool/blob/main/src/models/stratum-messages/SubscriptionMessage.ts) |
| **Reference pool wire constants** — `SUBSCRIBE_SESSION_ID_BYTES`, `EXTRANONCE2_SIZE_BYTES` | [`elektron-net-pool/src/models/stratum.constants.ts`](https://github.com/kutlusoy/elektron-net-pool/blob/main/src/models/stratum.constants.ts) |
| GBT output (node) | `src/rpc/mining.cpp` |
| Coinbase build order (node) | `src/node/miner.cpp` |
| Attestation validation | `src/validation.cpp` |
| Technical diff vs Bitcoin | [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md) |

When in doubt, compare against `mining/miner.py`. It is small, it is
canonical, and it is verified against the node by every release test.
If your pool produces a different coinbase serialization than the
Python miner for the same template, your pool is wrong.

---

## Appendix A — Why §3.5 in earlier doc revisions was misleading

**Revisions ≤ 3.x** described the conventional Stratum v1 arrangement
(`extranonce` lives inside `scriptSig`) without explicitly noting that
it is **incompatible with Elektron Net's UTXO attestation**. That
phrasing led at least one pool integrator to reserve an extranonce
hole inside `scriptSig`, which broke the attestation on every submitted
block.

**Revision 4.0** corrected the consensus story but advised
`extranonce1 = ""` on the wire. That instruction, while consensus-safe,
caused all hobby-miner firmwares that hard-require a non-empty
`extranonce1` (NerdMiner V2, Bitaxe, NerdAxe, ESP-Miner) to disconnect
immediately after subscribe. Operators of those fleets concluded —
incorrectly — that Elektron Net was simply incompatible with their
hardware and that a separate "compatibility proxy" would have to be
built and maintained alongside the pool.

**Revision 4.1 (this document)** clarifies that the two Stratum slots
can and must be decoupled:

- `extranonce1` on the subscribe reply is a **session label** —
  non-empty random hex, ignored by the coinbase builder.
- `extranonce2_size = 0` is the **consensus-safety knob** — keeps the
  worker from iterating anything into the coinbase.

Set together, they satisfy both the firmware (non-empty `extranonce1`)
and the node (coinbase byte-identical to `miner.py`). The reference
pool (`elektron-net-pool`, June 2026) ships this behaviour as default
and the migration checklist in §11 makes it explicit.

To prevent regressions:

- §3.2 of this revision states the consensus rule in absolute terms:
  scriptSig is exactly the GBT prefix, period.
- §3.5 spells out the Stratum configuration that follows from §3.2:
  non-empty `extranonce1` for wire compatibility, `extranonce2_size = 0`
  for consensus safety, full coinbase in `coinb1`.
- §3.6 explicitly lists "do not append extranonce padding to scriptSig"
  and "do not feed `extranonce1` back into the coinbase" as pitfalls.
- §3.7 documents the hobby-miner disconnect symptom and the exact
  pool-side fix.
- §6 maps `bad-utxo-attestation` directly to "scriptSig differs from
  `coinbase_script_sig_prefix`" so a debugging operator finds the
  cause immediately, and includes the hobby-miner disconnect loop as
  a separately listed non-consensus symptom.

The conceptual mistakes underlying revisions ≤ 4.0 are easy to make
because Bitcoin pools have done the "extranonce inside scriptSig" thing
for a decade, and because no other chain decouples the `extranonce1`
slot from coinbase content. Elektron Net trades that flexibility for
the per-block UTXO attestation; revision 4.1 documents the exact
pattern that keeps Stratum v1 firmware compatibility on top of it.

---

## Appendix B — Why a separate "compatibility proxy" is unnecessary

A previously circulated design proposal recommended building a
standalone Stratum proxy in front of the pool to translate between
"hobby-miner Stratum" (non-empty `extranonce1`, classical merkle
splicing) and "Elektron Net Stratum" (empty extranonces, full coinbase
in `coinb1`). The proposal was based on the assumption that the two
were fundamentally incompatible.

They are not. The §3.5 decoupling — non-empty `extranonce1` as session
label, `extranonce2_size = 0` as the consensus-safety knob — closes
the gap in the pool itself. No additional process, no extra TCP hop,
no separate codebase to maintain.

The reference pool's actual delta against a "pre-hobby-miner" pool
build is small:

1. Allocate a non-zero number of random bytes for the per-session id
   (`SUBSCRIBE_SESSION_ID_BYTES = 4` in the reference) — *not* the
   extranonce splice size, which stays at `0`.
2. Emit that hex string in the subscribe reply as both the notify
   channel id and the `extranonce1` slot.
3. Allow-list hobby userAgents and tune their starting difficulty
   downward so a ~50 kH/s ESP32 device actually submits shares before
   the dead-client timeout.

That is the entire scope. Coinbase construction, template management,
RPC submission, and the rest of the pool stack are unchanged — they
were already consensus-correct in revision 4.0. Skip the proxy; ship
the three changes above.
