# Elektron Net - Mining Pool Integration Guide

**Version:** 4.0.1
**Date:** June 23, 2026  
**Audience:** Pool operators, Stratum backend developers  
**Reference implementation:** [`mining/miner.py`](../mining/miner.py)  
**See also:** [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](../WHITEPAPER.md) §4.3

This document combines the official documentation with concrete implementation steps for your pool (elektron-net-pool). It is written so that developers can implement it immediately.

---

## 1. Who needs to change what?

| Role | Changes required? | Effort |
|------|-------------------|--------|
| **ASIC firmware** (Antminer, Whatsminer, ...) | **No** | - |
| **ASIC operators** (worker config) | **No** | Pool URL / worker only, as usual |
| **Mining pool** (Stratum backend) | **Yes** | Coinbase construction |
| **GBT miners** (cgminer, bfgminer, custom software) | **Yes** | Read `coinbase_required_outputs` |
| **In-node mining** (`generatetoaddress`, `miner.py`) | **No** | Already compatible |

ASICs only hash block headers. The **pool builds the coinbase** (or a GBT-capable miner does). Without pool changes, all Stratum blocks are **invalid** (`missing-utxo-attestation`).

Elektron Net adds two critical consensus changes:
- Per-Block UTXO-Attestation in the Coinbase
- 60-Second Blocks + Stoic Awakening

Standard Bitcoin pool software produces invalid blocks.

---

## 2. Elektron-specific GBT fields

Standard Bitcoin `getblocktemplate` plus Elektron Net extensions:

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

**Order matters.** `coinbase_required_outputs[0]` is the **UTXO attestation**, `coinbase_required_outputs[1]` is the **witness commitment**. This is the order the node emits - pools must preserve it.

### Required fields for the pool

| Field | Meaning |
|-------|---------|
| `height` | BIP34 height for `scriptSig` |
| `coinbasevalue` | Max coinbase payout (block reward + fees) in **leptons** |
| `coinbase_required_outputs` | **Mandatory outputs** - UTXO attestation **then** witness commitment, in that order |
| `coinbase_script_sig_prefix` | Encoded BIP34 height prefix the node expects in `scriptSig` |
| `default_witness_commitment` | Convenience copy of the witness commitment (already included in `coinbase_required_outputs`) |
| `transactions` | Mempool txs for the Merkle tree |
| `bits` | Current difficulty (includes Stoic Awakening) |

`coinbase_required_outputs` is **not** optional. Every block at height > 0 requires the UTXO attestation.

---

## 3. Building the coinbase correctly

### 3.1 Actual structure (verified against mainnet blocks)

```
vin[0]:  
- prevout: null (32x00 + ffffffff)  
- scriptSig: <BIP34 height push (from coinbase_script_sig_prefix or height)> + Extranonce  
- nSequence: 0xfffffffe

vout:  
- vout[0]: Payout (coinbasevalue + Miner Script)  
- vout[1]: UTXO-Attestation (exactly from required_outputs[0])  
- vout[2]: Witness Commitment (exactly from required_outputs[1])

nLockTime: height - 1

Witness (SegWit): 32-Byte Zero on vin[0]
```

This is the order produced by the in-node template builder. The reference miners append `coinbase_required_outputs` in array order after `vout[0]`.

**Verification:** Use `getrawtransaction <txid> true <blockhash>`.

**Most important rule:** Strictly maintain the order of outputs. Copy the hash from GBT - do not calculate it yourself.

### 3.2 UTXO attestation (consensus)

- **Format:** `OP_RETURN <push: nHeight> <push: 32-byte HASH_SERIALIZED>`
- The pool copies the `scriptPubKey` verbatim from GBT.

### 3.3 Witness commitment (BIP141, unchanged from Bitcoin)

Standard format and computation.

### 3.4 Reference (Python)

See function `_build_coinbase_tx()` in `mining/miner.py`.

### 3.5 Stratum (coinb1 / coinb2)

```
coinbase = coinb1 + extranonce1 + extranonce2 + coinb2
```

**Important:** The extranonce must only land in the scriptSig. `coinb2` must contain all required_outputs unchanged in the order provided by the node.

Recommendation: Fetch a fresh GBT template for every new block.

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
| Stoic Awakening | from height 1, after >120 s gap ? `powLimit` |

Pool backend: use an **Elektron Net node** (not Bitcoin Core) as the template source.

---

## 5. Implementation Steps in elektron-net-pool

### 5.1 BitcoinRpcService (bitcoin-rpc.service.ts)
- Already good: `getBlockTemplate` with coinbaseaddress.
- Make sure `coinbase_required_outputs` and `coinbase_script_sig_prefix` are always returned.

### 5.2 Job/Template Service (stratum-v1-jobs.service.ts)
- Parsing of required_outputs into IJobTemplate is already present - good.
- Implement clearJobs logic for height changes.

### 5.3 Stratum V1 Service & Coinbase Builder (stratum-v1.service.ts or MiningJob)

**This is the critical part:**

Implement a function `buildCoinbase(jobTemplate, extranonce1, extranonce2)` that exactly matches the Python reference:

- scriptSig = prefix + extranonce
- Build outputs in exact order
- Set nLockTime
- Add Witness
- Compute non-witness TXID for Merkle tree
- Correctly split coinb1/coinb2 (extranonce only in scriptSig)

### 5.4 Block Assembly & Submission
When a block is found:
- Build the final Coinbase
- Recompute Merkle Root
- Serialize the complete block as hex
- Call submitblock

### 5.5 Further Adjustments
- Ensure Bech32 HRP is 'be'
- No Dev-Fee in the Coinbase (handle off-chain)
- Refresh interval (30s + on new block)
- Monitor RPC load (because of per-miner GBT calls)

---

## 6. Test & Validation

1. Manually build a single block and test with submitblock
2. Connect a Stratum client and mine shares
3. Check a successful block with getrawtransaction (verify vout[0,1,2] are correct)
4. Compare with miner.py

**Expected errors:**
- missing-utxo-attestation
- bad-utxo-attestation (stale or wrong order)
- bad-cb-length (prefix missing)

---

## 7. Test checklist for pool operators

1. **Fetch GBT**
   ```bash
   elektron-cli getblocktemplate '{"rules":["segwit"]}'
   ```
   Expect exactly 2 entries in coinbase_required_outputs.

2. **Build coinbase locally** in the layout shown above and verify the Merkle root.

3. **Submit block**
   ```bash
   elektron-cli submitblock <hex>
   ```

4. **Avoid the errors** listed in section 6.

5. **Stratum end-to-end:** ASIC finds block ? pool submits ? getbestblockhash changes.

6. **Sanity check:** After acceptance, verify the coinbase with getrawtransaction.

---

## 8. Stoic Awakening (no pool patch needed)

If more than 120 seconds have passed since the last block, the node sets bits to the minimum (powLimit). The pool forwards this value unchanged.

---

## 9. Difficulty at chain start

### Is difficulty "at the bottom"?

**Partly - intentionally CPU-friendly, but not the absolute minimum.**

| Level | `nBits` / target | Meaning |
|-------|------------------|---------|
| **powLimit** (floor) | `007fffff0000...` | Easiest allowed difficulty; reached only via Stoic Awakening after >120 s |
| **Genesis & early blocks** | `1d7fffff` | Starting difficulty (~200x easier than Bitcoin genesis) |
| **After 2,016 blocks** | automatic | First retarget toward ~60 s block spacing |

Genesis was mined with `nBits = 0x1d7fffff`.

---

## 10. Recommendation: small miners first (operational, not consensus)

The protocol does not enforce miner size limits. For the early network phase, voluntary pool-level measures are recommended:

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

---

## 11. Migration checklist (Bitcoin pool ? Elektron Net)

- [ ] Elektron Net node (v4.0+) as backend, fresh chain
- [ ] GBT: parse coinbase_required_outputs and include both entries in the order received
- [ ] Coinbase layout: vout[0] payout, vout[1] UTXO attestation, vout[2] witness commitment
- [ ] Set nLockTime = height - 1
- [ ] Recompute Merkle root with the final coinbase
- [ ] Include Segwit witness stack (32-byte zero on vin[0])
- [ ] Use Elektron Net chain parameters
- [ ] Test with submitblock and getrawtransaction before enabling ASICs
- [ ] PROTOCOL_VERSION 70017

---

## 12. Support & references

| Topic | File |
|-------|------|
| Reference miner (Python) | [`mining/miner.py`](../mining/miner.py) |
| Reference miner (C++) | [`mining/miner.cpp`](../mining/miner.cpp) |
| GBT output (node) | `src/rpc/mining.cpp` |
| Coinbase build order | `src/node/miner.cpp` |
| Attestation validation | `src/validation.cpp` |
| Technical diff vs Bitcoin | [`BITCOIN_CORE_DIFF.md`](BITCOIN_CORE_DIFF.md) |

This document is your central implementation roadmap. Save it locally and work through it step by step.