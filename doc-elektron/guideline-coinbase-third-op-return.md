# Elektron Net - Coinbase Third OP_RETURN Guideline

- **Version:** 0.2 (draft, forward-looking idea, not scheduled for implementation)
- **Date:** July 19, 2026
- **Audience:** Core protocol developers, mining pool operators (`elektron-net-pool`, `-ppool`), wallet integrators
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) - `src/node/miner.cpp` (`BlockAssembler::CreateNewBlock()`), `src/validation.cpp` (`ExtractCoinbaseUTXOAttestation()`, `ValidateUTXOCheckpoint()`, `GenerateCoinbaseCommitment()`), `src/consensus/validation.h` (`GetWitnessCommitmentIndex()`) - treat as ground truth for anything this doc references
- **See also:** [`BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](https://github.com/kutlusoy/elektron-net/blob/main/WHITEPAPER.md), [`mining-pool-integration.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/mining-pool-integration.md), [`guideline-wallet-integration.md`](./guideline-wallet-integration.md)

---

## 1. Status of This Document

This is a **future-idea note**, not a design that has been approved or scheduled. It records a question raised during project discussion - whether a third, non-consensus-critical `OP_RETURN` output could be added to the coinbase transaction (for example for free-form miner/pool messages, or a protocol/miner identification tag) alongside the two outputs that already exist today. The intent of writing this down now is so the analysis is not lost, and so that anyone picking this idea up later (mining pool team, wallet team, or core protocol) starts from verified facts about the current code rather than re-deriving them. **No code change is proposed or required by this document.**

## 2. Current State: The Two Existing Coinbase OP_RETURN Outputs

`BlockAssembler::CreateNewBlock()` in `src/node/miner.cpp` actually produces **two different orderings**, depending on which of its two outputs is consumed - this was confirmed by reading the current function in full, not just a partial summary, after a real explorer screenshot surfaced the discrepancy below.

**Path A - the coinbase transaction object itself (`pblock->vtx[0]`)**, used directly by in-node/solo mining (`generatetoaddress`):

1. `vout[0]` - the block reward output.
2. `vout[1]` - the **witness commitment**, pushed by `ChainstateManager::GenerateCoinbaseCommitment()` (called first, before the attestation block). Standard Bitcoin Core mechanism, unmodified: `OP_RETURN 0x24 0xaa21a9ed <32-byte hash>`.
3. `vout[2]` - the **UTXO attestation**, an Elektron Net-specific addition, pushed second via `tx.vout.push_back(out)` inside the `if (nHeight > 0)` block. Format: `OP_RETURN <height> <32-byte UTXO set hash>`, computed by `ComputeBlockUTXOAttestationHash()` (`src/validation.cpp:2378-2420`).

So on this path the order is **reward, witness commitment, attestation**.

**Path B - the separate `coinbase_tx.required_outputs` field**, exposed via GBT as `coinbase_required_outputs` and consumed by every external miner and every pool (`mining/miner.py`, `mining/miner.cpp`, `elektron-net-pool`, `elektron-net-ppool`):

1. Inside the same `if (nHeight > 0)` attestation block, `coinbase_tx.required_outputs.push_back(out)` runs first, adding the **attestation** as the first entry.
2. Only afterwards, near the end of `CreateNewBlock()`, `coinbase_tx.required_outputs.push_back(final_coinbase->vout[witness_index])` appends the **witness commitment** as the second entry.

So `coinbase_required_outputs[0]` is the attestation and `coinbase_required_outputs[1]` is the witness commitment - the reverse of Path A. `doc-elektron/mining-pool-integration.md` documents and requires exactly this: pools build their own coinbase as `vout[0]` = payout, then `coinbase_required_outputs` appended verbatim in array order, giving a final on-chain order of **reward, attestation, witness commitment**.

**Practical consequence:** since essentially all real hashrate mines via GBT (pools and the reference `miner.py`/`miner.cpp`), essentially every real block on the chain has the Path B order - attestation immediately after the reward, witness commitment last. A block explorer showing "UTXO Attestation" before "Witness Commitment" for a real transaction is showing the normal, expected case, not an anomaly. Path A's reward-then-commitment-then-attestation order is real but is only produced by in-node solo mining, which is a minority of blocks in practice. Both orders are equally consensus-valid (see Section 3).

## 3. How Each Existing OP_RETURN Is Located at Validation Time

The two outputs are found by two different scanning strategies, and this difference is the central fact this whole guideline is about:

- **Witness commitment** - `GetWitnessCommitmentIndex()` (`src/consensus/validation.h:147-162`) loops over **all** `vout` entries, matches by the 6-byte magic prefix `OP_RETURN 0x24 0xaa 0x21 0xa9 0xed`, and keeps the **last** match (no early `break`).
- **UTXO attestation** - `ExtractCoinbaseUTXOAttestation()` (`src/validation.cpp:2423-2459`) loops over `vout` entries, decodes each `OP_RETURN` payload as `<height><32 bytes>`, and **returns immediately on the first output** whose decoded height equals the block's height (`src/validation.cpp:2453-2457`, `return attestation_hash;`). There is no uniqueness check: if more than one output in the coinbase happens to decode to a plausible `<height><32 bytes>` shape for the current height, only the first one found is ever considered, and validation never notices the second.

Neither lookup checks the total number of outputs or requires a coinbase output count of exactly two `OP_RETURN`s. Both are content-addressed, not position-addressed. This is what makes adding a third `OP_RETURN` possible **without a consensus rule change** in principle - but the first-match behavior of the attestation scan is also the one sharp edge that any future implementation MUST respect.

## 4. The Risk: How a Third OP_RETURN Could Trigger `bad-utxo-attestation`

`ValidateUTXOCheckpoint()` (`src/validation.cpp:2468-2504`, called from `ConnectBlock` at `src/validation.cpp:3027`) rejects a block with `bad-utxo-attestation` (`src/validation.cpp:2494-2496`) whenever the hash extracted from the coinbase does not match the freshly recomputed UTXO set hash. Given the first-match scanning behavior in Section 3, a third `OP_RETURN` output becomes a real (if currently only theoretical) risk under this specific condition:

- The new output is placed **before** the real attestation output in `vout` order (i.e., at index 1 or 2, pushing the real attestation further back), **and**
- Its raw payload happens to be structurally interpretable as `<CScriptNum matching the current height><32 bytes>`.

If both hold, `ExtractCoinbaseUTXOAttestation()` returns the wrong 32 bytes as "the" attestation, the recomputed hash will not match it, and an otherwise entirely valid block is rejected as consensus-invalid. Free-form text payloads are extremely unlikely to accidentally satisfy this byte pattern, but the design has no structural guard against it today - it relies entirely on convention, not on a consensus check for uniqueness.

This risk is smaller in practice than it might first appear, given Section 2: on the GBT/pool path that produces essentially all real blocks, the attestation already sits at `vout[1]`, immediately after the payout - about as early as it can be. A third output built the same way pools already build `coinbase_required_outputs` today (append-only, verbatim, never reordered - see `mining-pool-integration.md` §3.1/§3.6) would naturally land at `vout[3]`, after both existing outputs, without any special-casing required. The risk described above only materializes if a future implementation deviates from that existing append-only convention.

## 5. Recommendations, Should This Be Implemented Later

These are conditions for a safe implementation, not a commitment to build it:

1. **MUST** append the third `OP_RETURN` strictly **after** the existing attestation output in `vout` order (i.e., always the last output, never inserted at an earlier index), on both Path A and Path B from Section 2. This guarantees the first-match attestation scan always finds the real attestation before it can ever reach the new output.
2. **MUST** use a distinct magic-byte prefix at the start of the new output's data push, chosen so the payload cannot be mistaken for either existing pattern (`0xaa21a9ed` for the witness commitment, or a bare `<height><32 bytes>` shape for the attestation). A short fixed ASCII tag followed by a length-prefixed free-form field is a reasonable shape to consider.
3. **SHOULD** decide up front whether the output is purely informational (no validation) or becomes part of protocol semantics (e.g., pool identification used by an explorer or pool-stats service) - this changes whether any node-side code ever needs to parse it at all.
4. **SHOULD**, if this is pursued, add an explicit uniqueness check to `ExtractCoinbaseUTXOAttestation()` (reject a block if more than one output matches the attestation shape) so correctness no longer depends on convention alone. This itself would be a consensus-level code change and needs to be treated with the same care as any other consensus rule change in this codebase.
5. **MUST** treat cross-repo impact explicitly if implemented: `elektron-net-pool`/`-ppool` (block template construction), `elektron-net-mempool`/`-electrs` (any indexing of the new output), and wallet integrations would all need to be reviewed, since none of them currently expect a third coinbase `OP_RETURN`.

## 6. Checklist (For Whoever Picks This Up Later)

- [ ] Decide whether the third output is informational-only or protocol-relevant
- [ ] Define the exact wire format (magic bytes, versioning, max length)
- [ ] Place the new output after the attestation in `vout` order on both Path A and Path B (Section 5.1)
- [ ] Confirm no collision with the witness-commitment prefix or the attestation's `<height><32 bytes>` shape
- [ ] Decide whether `ExtractCoinbaseUTXOAttestation()` should gain a uniqueness check regardless (Section 5.4)
- [ ] Review impact on `elektron-net-pool`/`-ppool`, `elektron-net-mempool`/`-electrs`, and wallet integrations
- [ ] Update `BITCOIN_CORE_DIFF.md` and `mining-pool-integration.md` if this ever moves from idea to implementation

## 7. Open Questions

1. Is there an actual use case that justifies the added coinbase weight (miner/pool identification, user messages, something else), or does this stay a "nice to have" indefinitely?
2. Should the field be structured (versioned TLV) from day one, or free-form text to start?
3. Should this be informational-only, or should any node/indexer software ever depend on its contents?
