# Elektron Net - Coinbase Third OP_RETURN Guideline

- **Version:** 0.1 (draft, forward-looking idea, not scheduled for implementation)
- **Date:** July 19, 2026
- **Audience:** Core protocol developers, mining pool operators (`elektron-net-pool`, `-ppool`), wallet integrators
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) - `src/node/miner.cpp` (`BlockAssembler::CreateNewBlock()`), `src/validation.cpp` (`ExtractCoinbaseUTXOAttestation()`, `ValidateUTXOCheckpoint()`, `GenerateCoinbaseCommitment()`), `src/consensus/validation.h` (`GetWitnessCommitmentIndex()`) - treat as ground truth for anything this doc references
- **See also:** [`BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](https://github.com/kutlusoy/elektron-net/blob/main/WHITEPAPER.md), [`mining-pool-integration.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/mining-pool-integration.md), [`guideline-wallet-integration.md`](./guideline-wallet-integration.md)

---

## 1. Status of This Document

This is a **future-idea note**, not a design that has been approved or scheduled. It records a question raised during project discussion - whether a third, non-consensus-critical `OP_RETURN` output could be added to the coinbase transaction (for example for free-form miner/pool messages, or a protocol/miner identification tag) alongside the two outputs that already exist today. The intent of writing this down now is so the analysis is not lost, and so that anyone picking this idea up later (mining pool team, wallet team, or core protocol) starts from verified facts about the current code rather than re-deriving them. **No code change is proposed or required by this document.**

## 2. Current State: The Two Existing Coinbase OP_RETURN Outputs

`BlockAssembler::CreateNewBlock()` in `src/node/miner.cpp` builds the coinbase `vout` list in a fixed sequence:

1. `vout[0]` - the block reward output.
2. `vout[1]` - the **witness commitment**, added by `ChainstateManager::GenerateCoinbaseCommitment()` (`src/validation.cpp`, called at `src/node/miner.cpp:202`). Standard Bitcoin Core mechanism, unmodified: `OP_RETURN 0x24 0xaa21a9ed <32-byte hash>`.
3. `vout[2]` - the **UTXO attestation**, an Elektron Net-specific addition (`src/node/miner.cpp:204-227`, output pushed at line 220). Format: `OP_RETURN <height> <32-byte UTXO set hash>`, computed by `ComputeBlockUTXOAttestationHash()` (`src/validation.cpp:2378-2420`).

So today's real order is **reward, witness commitment, attestation** - the attestation is always the *last* output, not the first, contrary to an earlier assumption raised in discussion.

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

## 5. Recommendations, Should This Be Implemented Later

These are conditions for a safe implementation, not a commitment to build it:

1. **MUST** append the third `OP_RETURN` strictly **after** the existing attestation output in `vout` order (i.e., always `vout[3]`, never inserted at an earlier index). This guarantees the first-match attestation scan always finds the real attestation before it can ever reach the new output.
2. **MUST** use a distinct magic-byte prefix at the start of the new output's data push, chosen so the payload cannot be mistaken for either existing pattern (`0xaa21a9ed` for the witness commitment, or a bare `<height><32 bytes>` shape for the attestation). A short fixed ASCII tag followed by a length-prefixed free-form field is a reasonable shape to consider.
3. **SHOULD** decide up front whether the output is purely informational (no validation) or becomes part of protocol semantics (e.g., pool identification used by an explorer or pool-stats service) - this changes whether any node-side code ever needs to parse it at all.
4. **SHOULD**, if this is pursued, add an explicit uniqueness check to `ExtractCoinbaseUTXOAttestation()` (reject a block if more than one output matches the attestation shape) so correctness no longer depends on convention alone. This itself would be a consensus-level code change and needs to be treated with the same care as any other consensus rule change in this codebase.
5. **MUST** treat cross-repo impact explicitly if implemented: `elektron-net-pool`/`-ppool` (block template construction), `elektron-net-mempool`/`-electrs` (any indexing of the new output), and wallet integrations would all need to be reviewed, since none of them currently expect a third coinbase `OP_RETURN`.

## 6. Checklist (For Whoever Picks This Up Later)

- [ ] Decide whether the third output is informational-only or protocol-relevant
- [ ] Define the exact wire format (magic bytes, versioning, max length)
- [ ] Place the new output after the attestation in `vout` order (Section 5.1)
- [ ] Confirm no collision with the witness-commitment prefix or the attestation's `<height><32 bytes>` shape
- [ ] Decide whether `ExtractCoinbaseUTXOAttestation()` should gain a uniqueness check regardless (Section 5.4)
- [ ] Review impact on `elektron-net-pool`/`-ppool`, `elektron-net-mempool`/`-electrs`, and wallet integrations
- [ ] Update `BITCOIN_CORE_DIFF.md` and `mining-pool-integration.md` if this ever moves from idea to implementation

## 7. Open Questions

1. Is there an actual use case that justifies the added coinbase weight (miner/pool identification, user messages, something else), or does this stay a "nice to have" indefinitely?
2. Should the field be structured (versioned TLV) from day one, or free-form text to start?
3. Should this be informational-only, or should any node/indexer software ever depend on its contents?
