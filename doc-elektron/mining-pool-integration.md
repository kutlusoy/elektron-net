# Elektron Net — Mining Pool Integration Guide

**Version:** 4.0.0
**Audience:** Pool operators, Stratum backend developers
**Reference implementation:** [`mining/miner.py`](../mining/miner.py)
**See also:** [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](../WHITEPAPER.md) §4.3

---

## 1. Who needs to change what?

| Role | Changes required? | Effort |
|------|-------------------|--------|
| **ASIC firmware** (Antminer, Whatsminer, …) | **No** | — |
| **ASIC operators** (worker config) | **No** | Pool URL / worker only, as usual |
| **Mining pool** (Stratum backend) | **Yes** | Coinbase construction |
| **GBT miners** (cgminer, bfgminer, custom software) | **Yes** | Read `coinbase_required_outputs` |
| **In-node mining** (`generatetoaddress`, `miner.py`) | **No** | Already compatible |

ASICs only hash block headers. The **pool builds the coinbase** (or a GBT-capable miner does). Without pool changes, all Stratum blocks are **invalid** (`missing-utxo-attestation`).

---

## 2. Elektron-specific GBT fields

Standard Bitcoin `getblocktemplate` plus Elektron extensions:

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
    {
      "value": 0,
      "scriptPubKey": "6a022a0020<32-byte UTXO hash>"
    },
    {
      "value": 0,
      "scriptPubKey": "6a24aa21a9ed<32-byte witness root commitment>"
    }
  ],
  "coinbase_script_sig_prefix": "012a",
  "default_witness_commitment": "6a24aa21a9ed..."
}
```

**Order matters.** `coinbase_required_outputs[0]` is the **UTXO attestation**, `coinbase_required_outputs[1]` is the **witness commitment**. This is the order the node emits — pools must preserve it.

### Required fields for the pool

| Field | Meaning |
|-------|---------|
| `height` | BIP34 height for `scriptSig` |
| `coinbasevalue` | Max coinbase payout (block reward + fees) in **leptons** |
| `coinbase_required_outputs` | **Mandatory outputs** — UTXO attestation **then** witness commitment, in that order |
| `coinbase_script_sig_prefix` | Encoded BIP34 height prefix the node expects in `scriptSig` |
| `default_witness_commitment` | Convenience copy of the witness commitment (already included in `coinbase_required_outputs`) |
| `transactions` | Mempool txs for the Merkle tree |
| `bits` | Current difficulty (includes Stoic Awakening) |

`coinbase_required_outputs` is **not** optional. Every block at height > 0 requires the UTXO attestation.

---

## 3. Building the coinbase correctly

### 3.1 Actual structure (verified against mainnet blocks)

```
vin[0]:  null prevout, scriptSig = <BIP34 height push> + <pool extranonce region>
vout[0]: coinbasevalue → miner payout (worker P2WPKH/P2PKH)
vout[1]: required_outputs[0]  — UTXO attestation
                                OP_RETURN <push: height> <push: 32-byte UTXO hash>
vout[2]: required_outputs[1]  — witness commitment
                                OP_RETURN 0x24 0xaa21a9ed <32-byte witness root commitment>
nLockTime: height - 1
Witness:   for Segwit, exactly one 32-byte zero stack element on vin[0]
```

This is the order produced by the in-node template builder (`src/node/miner.cpp` — UTXO attestation is pushed onto `required_outputs` first at line 220, witness commitment second at line 240). The reference miners (`mining/miner.py`, `mining/miner.cpp`) append `coinbase_required_outputs` in array order after `vout[0]`, producing exactly the layout above.

**Verification:** dump any recent coinbase with `getrawtransaction <txid> true <blockhash>`:

```
"vout": [
  { "n": 0, "scriptPubKey": { "address": "be1q…" } },                   ← payout
  { "n": 1, "scriptPubKey": { "asm": "OP_RETURN <height> <hash>" } },   ← UTXO attestation (varies per block)
  { "n": 2, "scriptPubKey": { "asm": "OP_RETURN aa21a9ed…" } }          ← witness commitment
]
```

The hash in `vout[2]` will appear identical across empty blocks — this is correct BIP141 behaviour (the witness root commits only to coinbase witness + reserved value, which are constant when no other transactions are present). The **per-block uniqueness lives in `vout[1]`**, the UTXO attestation.

**Do not reorder `required_outputs`.** The node accepts either output position consensus-wise (the attestation is identified by content, not by index), but Stratum jobs and Merkle reconstruction depend on a stable layout. Append exactly in the order the node provides.

### 3.2 UTXO attestation (consensus)

- **Format:** `OP_RETURN <push: nHeight> <push: 32-byte HASH_SERIALIZED>`
- The height is encoded with `CScript() << nHeight` — a compact pushdata of 1–5 bytes depending on the height value (BIP34-style). For mainnet block 5000 this is `02 88 13` (push 2 bytes: little-endian 0x1388). For block 197,280 it is `03 a0 02 03`.
- The hash push is always `0x20` (push 32 bytes) + the 32-byte UTXO commitment.
- **Hash = HASH_SERIALIZED of the UTXO set *after* connecting all transactions in this block.**
- **Computed by the node.** The pool **copies `scriptPubKey` from GBT verbatim** — do **not** recompute the hash locally. The node's computation walks the parent UTXO view plus this block's tx effects; replicating that off-node is error-prone and unnecessary.
- `OP_RETURN` outputs do not enter the UTXO set, so the attestation cannot feed back into the next block's hash.

### 3.3 Witness commitment (BIP141, unchanged from Bitcoin)

- **Format:** `OP_RETURN 0x24 0xaa21a9ed <32-byte commitment>` (38-byte `scriptPubKey`)
- Computed as `SHA256d(witness_root_hash || witness_reserved_value)`
- `witness_reserved_value` is the 32-byte zero stack element on `vin[0]`'s witness
- Standard Bitcoin behaviour — Elektron does not modify this

### 3.4 Reference (Python)

Logic in [`mining/miner.py`](../mining/miner.py), function `_build_coinbase_tx()`:

1. `scriptSig` = BIP34 height encoding (+ `OP_0` if `len(scriptSig) < 2`)
2. `vout[0]` = payout
3. **Append all entries from `coinbase_required_outputs` in the order received** — first the UTXO attestation, then the witness commitment
4. Add Segwit marker / flag and the 32-byte zero witness stack on `vin[0]` (the witness commitment in `vout[2]` is consensus-required when any block tx has witness data, and is included unconditionally by the node template)
5. Recompute Merkle root from coinbase TXID + `transactions[].txid`

### 3.5 Stratum (coinb1 / coinb2)

Standard Stratum splitting:

```
coinbase = coinb1 + extranonce1 + extranonce2 + coinb2
```

**Important:** `coinb1` and `coinb2` must be split so that:

- `extranonce` only lands in `scriptSig` (after the BIP34 height push)
- **`coinb2` contains all `required_outputs` unchanged, in the order the node provided** (UTXO attestation first, witness commitment second)
- Witness data and the attestation `scriptPubKey` are **never** inside the extranonce region

Recommendation: fetch a fresh GBT template for every new block, and re-apply `coinbase_required_outputs` on every job. The UTXO attestation hash changes every block; reusing a stale template will always be rejected as `bad-utxo-attestation`.

---

## 4. Network parameters (mainnet v4.0)

| Parameter | Value |
|-----------|-------|
| Algorithm | SHA-256d (same as Bitcoin) |
| P2P port | 8333 |
| RPC port | 8332 |
| Message start | `e1 ec 7a 6e` |
| Block time target | 60 s |
| Retarget interval | 2,016 blocks (~1.4 days) |
| `PROTOCOL_VERSION` | 70017 |
| Bech32 HRP | `be` |
| Block reward (start) | 5 ELEK |
| Stoic Awakening | from height 1, after >120 s gap → `powLimit` |

Pool backend: use an **Elektron node** (not Bitcoin Core) as the template source.

---

## 5. Test checklist for pool operators

1. **Fetch GBT**
   ```bash
   elektron-cli getblocktemplate '{"rules":["segwit"]}'
   ```
   Expect `coinbase_required_outputs` with **exactly 2 entries** at any height > 0:
   - `[0]` — UTXO attestation (`6a` + height push + `20` + 32-byte hash)
   - `[1]` — witness commitment (`6a24aa21a9ed…`)

2. **Build coinbase locally** in the layout shown in §3.1 and verify the recomputed Merkle root matches your block header.

3. **Submit block**
   ```bash
   elektron-cli submitblock <hex>
   ```
   Expected: empty response = accepted.

4. **Avoid these errors**

   | Error code | Cause |
   |------------|-------|
   | `missing-utxo-attestation` | No matching `OP_RETURN <height> <32-byte hash>` output in coinbase |
   | `bad-utxo-attestation` | Hash present but does not match the expected UTXO commitment (usually a stale template) |
   | `bad-utxo-attestation-compute` | Node could not recompute the hash to verify (transient internal error — refetch template) |
   | `bad-cb-amount` | Exceeded `coinbasevalue` |
   | `bad-cb-length` | `scriptSig` too short (BIP34) |

5. **Stratum end-to-end:** ASIC finds block → pool submits to node → `getbestblockhash` changes.

6. **Sanity check on production coinbase:** after a block is accepted, run
   ```bash
   elektron-cli getrawtransaction <coinbase-txid> true <blockhash>
   ```
   and confirm `vout[1]` carries the UTXO attestation (height + hash) and `vout[2]` carries the witness commitment (`aa21a9ed…`). If these are swapped, your pool reordered `required_outputs` — fix the builder.

---

## 6. Stoic Awakening (no pool patch needed)

If **> 120 seconds** have passed since the last block, the node sets `bits` in the template to the minimum (`powLimit`). The pool forwards this `bits` value unchanged to ASICs — **no** special pool logic is required. The same template path also lowers the difficulty target Stratum advertises to workers, so small solo hardware (Bitaxe, NerdMiner) can win these slots.

---

## 7. Difficulty at chain start

### Is difficulty "at the bottom"?

**Partly — intentionally CPU-friendly, but not the absolute minimum.**

| Level | `nBits` / target | Meaning |
|-------|------------------|---------|
| **powLimit** (floor) | `007fffff0000…` | Easiest allowed difficulty; reached only via Stoic Awakening after >120 s |
| **Genesis & early blocks** | `1d7fffff` | Starting difficulty (~200× easier than Bitcoin genesis) |
| **After 2,016 blocks** | automatic | First retarget toward ~60 s block spacing |

Genesis was mined with `nBits = 0x1d7fffff` — target: **~1 minute of CPU time** for the genesis block ([`mining/GENESIS.md`](../mining/GENESIS.md)).

- **Not** permanently at `powLimit` — only when the chain stalls (Stoic Awakening).
- **Yes** — suitable for small hardware at the start.
- When significant hashrate joins, difficulty rises at the **next 2,016-block retarget** automatically.

The protocol has **no** hard "ASIC exclusion". Low starting difficulty favours CPUs and small ASICs; large farms would push difficulty up at retarget.

---

## 8. Recommendation: small miners first (operational, not consensus)

The protocol does not enforce miner size limits. For the **early network phase**, voluntary **pool-level** measures are recommended:

| Measure | Purpose |
|---------|---------|
| Launch pool publicly later | CPU solo and small community first |
| No mass Stratum for ASIC farms on day 1 | Avoid immediate hashrate hoarding |
| `getblocktemplate` local-only / API key | Controlled growth |
| Transparent announcement before pool launch | Fairness for early miners |
| Optional: worker limits / invite-only phase | Operational, not in chain code |

**Solo mining** for individuals works immediately:

```bash
python mining/miner.py
# or
elektron-cli generatetoaddress 1 "<be1q-address>"
```

No pool required; full block reward goes to your own address.

---

## 9. Migration checklist (Bitcoin pool → Elektron)

- [ ] Elektron node (v4.0+) as backend, fresh chain
- [ ] GBT: parse `coinbase_required_outputs` and include both entries in coinbase, **in the order received** (UTXO attestation first, witness commitment second)
- [ ] Coinbase layout: `vout[0]` payout, `vout[1]` UTXO attestation, `vout[2]` witness commitment
- [ ] Set `nLockTime = height - 1`
- [ ] Recompute Merkle root with the final coinbase
- [ ] Include Segwit witness stack (one 32-byte zero element on `vin[0]`)
- [ ] Do not assume Bitcoin `powLimit` / genesis — use Elektron chain parameters
- [ ] Test with `submitblock` and `getrawtransaction` before enabling ASICs
- [ ] `PROTOCOL_VERSION` 70017 — do not use legacy Bitcoin peers as template source

---

## 10. Support & references

| Topic | File |
|-------|------|
| Reference miner (Python) | [`mining/miner.py`](../mining/miner.py) |
| Reference miner (C++) | [`mining/miner.cpp`](../mining/miner.cpp) |
| GBT output (node) | [`src/rpc/mining.cpp`](../src/rpc/mining.cpp) — see `coinbase_required_outputs` push |
| Coinbase build order in template | [`src/node/miner.cpp`](../src/node/miner.cpp) `CreateNewBlock` lines ~200–240 |
| Attestation validation | [`src/validation.cpp`](../src/validation.cpp) — `ValidateUTXOCheckpoint`, `ComputeBlockUTXOAttestationHash`, `ExtractCoinbaseUTXOAttestation` |
| Technical diff vs Bitcoin | [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md) |
| **Wallet integrators** (UTXO scan, pruning) | [`BITCOIN_CORE_DIFF.md` §9.3](BITCOIN_CORE_DIFF.md#93-wallet-software--should-understand) |

For coinbase structure questions: always compare `getblocktemplate` from your running node against the reference miner output first, and confirm the actual mined block layout with `getrawtransaction <coinbase-txid> true <blockhash>`.

**Wallet vendors:** Elektron does not require `-reindex` on pruned nodes — balances recover via UTXO-set scan at wallet load. See [`BITCOIN_CORE_DIFF.md` §2.6 / §9.3](BITCOIN_CORE_DIFF.md).
