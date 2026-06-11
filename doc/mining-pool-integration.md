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
      "scriptPubKey": "6a24aa21a9ed..."
    },
    {
      "value": 0,
      "scriptPubKey": "6a04..."
    }
  ],
  "default_witness_commitment": "6a24aa21a9ed..."
}
```

### Required fields for the pool

| Field | Meaning |
|-------|---------|
| `height` | BIP34 height in `scriptSig` |
| `coinbasevalue` | Max coinbase payout (block reward + fees) in **leptons** |
| `coinbase_required_outputs` | **Mandatory outputs** — witness commitment + UTXO attestation |
| `default_witness_commitment` | Legacy fallback for witness (Segwit) only |
| `transactions` | Mempool txs for the Merkle tree |
| `bits` | Current difficulty (includes Stoic Awakening) |

`coinbase_required_outputs` is **not** optional. Every block (height > 0) requires the UTXO attestation.

---

## 3. Building the coinbase correctly

### 3.1 Structure

```
vin[0]:  null prevout, scriptSig = <BIP34 height> + <pool extranonce region>
vout[0]: coinbasevalue → miner payout (worker P2WPKH/P2PKH)
vout[1]: required_outputs[0]  — typically witness commitment (OP_RETURN 0x24 0xaa21…)
vout[2]: required_outputs[1]  — UTXO attestation (OP_RETURN <height> <32-byte hash>)
nLockTime: height - 1
Witness:   for Segwit, exactly one 32-byte zero stack element
```

Preserve the order of `required_outputs` **from the node** — do not reorder.

### 3.2 UTXO attestation (consensus)

- Format: `OP_RETURN <height(4 B)> <HASH_SERIALIZED(32 B)>`
- Hash = UTXO set **after** connecting all block transactions
- Computed by the node; the pool **copies** `scriptPubKey` from GBT — do not hash locally
- `OP_RETURN` outputs are not added to the UTXO set (no feedback loop)

### 3.3 Reference (Python)

Logic in [`mining/miner.py`](../mining/miner.py), function `_build_coinbase_tx()`:

1. `scriptSig` = BIP34 height (+ `OP_0` if `len(scriptSig) < 2`)
2. `vout[0]` = payout
3. Append all entries from `coinbase_required_outputs`
4. Segwit marker + witness stack if witness commitment is present
5. Recompute Merkle root from coinbase TXID + `transactions[].txid`

### 3.4 Stratum (coinb1 / coinb2)

Standard Stratum splitting:

```
coinbase = coinb1 + extranonce1 + extranonce2 + coinb2
```

**Important:** `coinb1` and `coinb2` must be split so that:

- `extranonce` only lands in `scriptSig` (after BIP34 height)
- **`coinb2` contains all `required_outputs` unchanged**
- Witness data and attestation are **not** in the extranonce region

Recommendation: fetch a fresh GBT template for every new block; re-apply `coinbase_required_outputs` on every job (the hash changes per block).

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
   elektrond getblocktemplate '{"rules":["segwit"]}'
   ```
   → `coinbase_required_outputs` with ≥ 1 entry (from height 1, typically 2: witness + attestation)

2. **Build coinbase locally** (like `miner.py`) and verify Merkle root

3. **Submit block**
   ```bash
   elektrond submitblock <hex>
   ```
   Expected: empty response = accepted

4. **Avoid these errors**

   | Error code | Cause |
   |------------|-------|
   | `missing-utxo-attestation` | Attestation missing from coinbase |
   | `bad-utxo-attestation` | Wrong hash or wrong height |
   | `bad-cb-amount` | Exceeded `coinbasevalue` |
   | `bad-cb-length` | `scriptSig` too short (BIP34) |

5. **Stratum end-to-end:** ASIC finds block → pool submits to node → `getbestblockhash` changes

---

## 6. Stoic Awakening (no pool patch needed)

If **> 120 seconds** have passed since the last block, the node sets `bits` in the template to **minimum** (`powLimit`). The pool forwards this `bits` unchanged to ASICs — **no** special pool logic required.

---

## 7. Difficulty at chain start

### Is difficulty “at the bottom”?

**Partly — intentionally CPU-friendly, but not the absolute minimum.**

| Level | `nBits` / target | Meaning |
|-------|------------------|---------|
| **powLimit** (floor) | `007fffff0000…` | Easiest allowed difficulty; Stoic Awakening after >120 s |
| **Genesis & early blocks** | `1d7fffff` | Starting difficulty (~200× easier than Bitcoin genesis) |
| **After 2,016 blocks** | automatic | First retarget toward ~60 s block spacing |

Genesis was mined with `nBits = 0x1d7fffff` — target: **~1 minute of CPU time** for the genesis block ([`mining/GENESIS.md`](../mining/GENESIS.md)).

- **Not** permanently at `powLimit` — only when the chain stalls (Stoic Awakening)
- **Yes** — suitable for small hardware at the start
- When significant hashrate joins, difficulty rises at the **next 2,016-block retarget** automatically

The protocol has **no** hard “ASIC exclusion”. Low starting difficulty favors CPUs; large farms would push difficulty up at retarget.

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
- [ ] GBT: parse `coinbase_required_outputs` and include in coinbase
- [ ] Set `nLockTime = height - 1`
- [ ] Recompute Merkle root with new coinbase
- [ ] Do not forget Segwit witness stack (32 zero bytes)
- [ ] Do not assume Bitcoin `powLimit` / genesis — use Elektron chain parameters
- [ ] Test with `submitblock` before enabling ASICs
- [ ] `PROTOCOL_VERSION` 70017 — do not use legacy Bitcoin peers as template source

---

## 10. Support & references

| Topic | File |
|-------|------|
| Reference miner (Python) | [`mining/miner.py`](../mining/miner.py) |
| Reference miner (C++) | [`mining/miner.cpp`](../mining/miner.cpp) |
| GBT output (node) | [`src/rpc/mining.cpp`](../src/rpc/mining.cpp) |
| Attestation validation | [`src/validation.cpp`](../src/validation.cpp) |
| Technical diff vs Bitcoin | [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md) |
| **Wallet integrators** (UTXO scan, pruning) | [`BITCOIN_CORE_DIFF.md` §9.3](BITCOIN_CORE_DIFF.md#93-wallet-software--should-understand) |

For coinbase structure questions: always compare `getblocktemplate` from your running node against `miner.py` output first.

**Wallet vendors:** Elektron does not require `-reindex` on pruned nodes — balances recover via UTXO-set scan at wallet load. See [`BITCOIN_CORE_DIFF.md` §2.6 / §9.3](BITCOIN_CORE_DIFF.md).