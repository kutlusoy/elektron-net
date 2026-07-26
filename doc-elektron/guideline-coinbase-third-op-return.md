# Elektron Net - Coinbase Third OP_RETURN Guideline

- **Version:** 1.0 (decided design, pool-side implementation not yet built)
- **Date:** July 26, 2026 (draft: July 19, 2026)
- **Audience:** Core protocol developers, mining pool operators/developers (`elektron-net-pool`, `-ppool`), wallet integrators
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) - `src/node/miner.cpp` (`BlockAssembler::CreateNewBlock()`), `src/validation.cpp` (`ExtractCoinbaseUTXOAttestation()`, `ValidateUTXOCheckpoint()`, `GenerateCoinbaseCommitment()`), `src/consensus/validation.h` (`GetWitnessCommitmentIndex()`) - treat as ground truth for anything this doc references
- **Consumer:** [`elektron-net-pool`](https://github.com/kutlusoy/elektron-net-pool), [`elektron-net-ppool`](https://github.com/kutlusoy/elektron-net-ppool) - `src/models/MiningJob.ts` (coinbase construction); [`elektron-net-electrum`](https://github.com/kutlusoy/elektron-net-electrum) - `electrum/payment_identifier.py`, `electrum/gui/qt/send_tab.py` (Section 8)
- **See also:** [`BITCOIN_CORE_DIFF.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/BITCOIN_CORE_DIFF.md), [`WHITEPAPER.md`](https://github.com/kutlusoy/elektron-net/blob/main/WHITEPAPER.md), [`mining-pool-integration.md`](https://github.com/kutlusoy/elektron-net/blob/main/doc-elektron/mining-pool-integration.md), [`guideline-wallet-integration.md`](./guideline-wallet-integration.md)

- Requirement-level words follow standard usage: **MUST** = mandatory, **SHOULD** = strongly recommended, **MAY** = optional.

---

## 1. Status of This Document

**v1.0 update:** this document has moved from an open, forward-looking idea (v0.3) to a **decided design**. The question was: could a third, non-consensus-critical `OP_RETURN` output be added to the coinbase transaction (for example for a free-form pool-operator message) alongside the two outputs that already exist today, without breaking `bad-utxo-attestation` validation. The answer, confirmed by re-reading the current consensus code plus a dedicated cross-repo review (Section 6), is yes, under the conditions in Section 5.

**What is decided as of this version:**

- The third output is **informational only**, produced **pool-side** (`elektron-net-pool`, `elektron-net-ppool`), never node-side. There is **no consensus rule change** and no change to `ExtractCoinbaseUTXOAttestation()` or any other validation code.
- The content is **free-form text**, configurable per pool operator (for example a pool name or tag), not a fixed or versioned TLV format.
- The requirements in Section 5 are now binding (MUST/SHOULD) for any future implementation, not merely "if this is pursued" as in v0.3.

**What is explicitly out of scope for this version:** the actual pool-side code (the TypeScript change in `MiningJob.ts` for both pools, the configuration surface for the operator-supplied text, and the accompanying spec/test updates) is **not part of this revision**. This document only records the decided design; implementation is separate, future work, tracked via the checklist in Section 6.

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

**Practical consequence:** since essentially all real hashrate mines via GBT (pools and the reference `miner.py`/`miner.cpp`), essentially every real block on the chain has the Path B order - attestation immediately after the reward, witness commitment last. A block explorer showing "UTXO Attestation" before "Witness Commitment" for a real transaction is showing the normal, expected case, not an anomaly. Path A's reward-then-commitment-then-attestation order is real but is only produced by in-node solo mining, which is a minority of blocks in practice. Both orders are equally consensus-valid (see Section 3). This is also the path relevant to the pool-side output from Section 5: on Path B it lands at `vout[3]`, after both existing required outputs.

## 3. How Each Existing OP_RETURN Is Located at Validation Time

The two outputs are found by two different scanning strategies, and this difference is the central fact this whole guideline is about:

- **Witness commitment** - `GetWitnessCommitmentIndex()` (`src/consensus/validation.h:147-162`) loops over **all** `vout` entries, matches by the 6-byte magic prefix `OP_RETURN 0x24 0xaa 0x21 0xa9 0xed`, and keeps the **last** match (no early `break`).
- **UTXO attestation** - `ExtractCoinbaseUTXOAttestation()` (`src/validation.cpp:2423-2459`) loops over `vout` entries, decodes each `OP_RETURN` payload as `<height><32 bytes>`, and **returns immediately on the first output** whose decoded height equals the block's height (`src/validation.cpp:2453-2457`, `return attestation_hash;`). There is no uniqueness check: if more than one output in the coinbase happens to decode to a plausible `<height><32 bytes>` shape for the current height, only the first one found is ever considered, and validation never notices the second.

Neither lookup checks the total number of outputs or requires a coinbase output count of exactly two `OP_RETURN`s. Both are content-addressed, not position-addressed. This is what makes adding a third `OP_RETURN` possible **without a consensus rule change** in principle - and this v1.0 decision relies on exactly that: no change to either function above.

Separately, `OP_RETURN` outputs are never added to the UTXO set (`IsUnspendable()`, unmodified from Bitcoin Core), so a third `OP_RETURN`, wherever placed, cannot itself change the recomputed UTXO-set hash that `ValidateUTXOCheckpoint()` compares against. The entire risk described in Section 4 is confined to which output gets *read* as the attestation, not to the hash computation itself.

## 4. The Risk: How a Third OP_RETURN Could Trigger `bad-utxo-attestation`

`ValidateUTXOCheckpoint()` (`src/validation.cpp:2468-2504`, called from `ConnectBlock` at `src/validation.cpp:3027`) rejects a block with `bad-utxo-attestation` (`src/validation.cpp:2494-2496`) whenever the hash extracted from the coinbase does not match the freshly recomputed UTXO set hash. Given the first-match scanning behavior in Section 3, a third `OP_RETURN` output becomes a real (if narrow) risk under this specific condition:

- The new output is placed **before** the real attestation output in `vout` order (i.e., at index 1 or 2, pushing the real attestation further back), **and**
- Its raw payload happens to be structurally interpretable as `<CScriptNum matching the current height><32 bytes>` (i.e. two data pushes after `OP_RETURN`, second one exactly 32 bytes).

If both hold, `ExtractCoinbaseUTXOAttestation()` returns the wrong 32 bytes as "the" attestation, the recomputed hash will not match it, and an otherwise entirely valid block is rejected as consensus-invalid.

This risk is smaller in practice than it might first appear, given Section 2: on the GBT/pool path that produces essentially all real blocks, the attestation already sits at `vout[1]`, immediately after the payout - about as early as it can be. A third output built the same way pools already build `coinbase_required_outputs` today (append-only, verbatim, never reordered - see `mining-pool-integration.md` §3.1/§3.6) would naturally land at `vout[3]`, after both existing outputs, without any special-casing required. Section 5 makes this the binding design, not just a favorable default.

## 5. Design Decision: Pool-Side, Informational, No Consensus Change (Decided July 26, 2026)

The third `OP_RETURN` is a **pool feature**, not a protocol feature. `elektron-net-pool` and `elektron-net-ppool` already build the coinbase by appending `coinbase_required_outputs` verbatim (`vout[1]`, `vout[2]`) and then stopping - no code path in either repo currently enforces a maximum output count, and the existing "no dev/pool fee in the coinbase" prohibition in both repos' READMEs and in `mining-pool-integration.md` §5.6 is explicitly scoped to a second **spendable payout** output (which would change the bytes the attestation hash was computed against), not to a zero-value informational `OP_RETURN`. Adding this output is therefore a pure pool-side addition on top of the existing append-only construction, appended after the loop that copies `coinbase_required_outputs`.

Requirements for the pool-side implementation, whenever it is built:

1. **MUST** append the new output strictly **after** the existing `coinbase_required_outputs` loop, i.e. always as the last coinbase output (`vout[3]` on the GBT/pool path from Section 2), never inserted at an earlier index. This guarantees the first-match attestation scan in Section 3 always finds the real attestation before it could ever reach the new output.
2. **MUST** encode the message as a **single data push** after `OP_RETURN` (`OP_RETURN <message bytes>`), not as two separate pushes. `ExtractCoinbaseUTXOAttestation()` requires two consecutive pushes after `OP_RETURN` (a height-like value, then an exact 32-byte push) to even consider an output as a candidate attestation; a single-push output fails that shape check unconditionally on the second `GetOp()` call, regardless of its byte content. This gives a structural guarantee against misinterpretation as the attestation that does not depend on avoiding a particular byte pattern.
3. **SHOULD** cap the message length at the pool layer (a sensible ordinary `OP_RETURN` size, consistent with common relay-policy conventions elsewhere in the ecosystem) and sanitize the input (for example reject or strip bytes that are not valid UTF-8 text). This is an operational safeguard, not a consensus requirement, since coinbase transactions are not subject to mempool relay-policy checks at all.
4. **SHOULD** document, as an edge case (not a runtime guard), that a message push whose length and leading bytes happen to exactly match the witness-commitment magic prefix (`0x24 0xaa21a9ed`, i.e. a 36-byte push starting with `aa21a9ed`) would be picked up as a witness-commitment candidate by the *last-match* scan in `GetWitnessCommitmentIndex()`. Since the real, node-generated commitment is always pushed after the pool's own outputs are irrelevant to that scan order (the commitment is computed and placed by the node before the pool ever sees the template) and is authoritative regardless, this has no practical failure mode for pool-supplied free text, but is worth a one-line code comment where the message output is constructed.
5. Content: **free-form text**, operator-configurable (for example via an environment variable, following the existing pattern used for `HOBBY_MINER_USER_AGENTS`/`HOBBY_MINER_DIFFICULTY` in both pools). No fixed or versioned TLV format is required for this decision; the field is purely informational.

**Explicitly decided against:** adding a uniqueness check to `ExtractCoinbaseUTXOAttestation()` (rejecting a block if more than one output matches the attestation shape). This was considered (it was v0.3's Recommendation 4) and rejected for this design: it is a consensus-level rule change with its own review, testing, and activation-height burden, and requirement 2 above (single-push encoding) already removes the structural ambiguity that check would have guarded against, without touching validation code at all. If a future, different use of a third coinbase output ever needs two data pushes (for example a structured/TLV format), this decision should be revisited at that time, not assumed to still apply.

## 6. Cross-Repo Impact Assessment

Reviewed as part of deciding this design (v0.3 Recommendation 5), across every repo in the Elektron Net project that touches coinbase construction or coinbase content:

- **`elektron-net-pool` / `elektron-net-ppool`:** the intended implementation target (Section 5). `MiningJob.ts` in both repos builds `vout[0]` = payout, then appends `coinbase_required_outputs` verbatim with no output-count limit anywhere in the code; the PPLNS split in `elektron-net-ppool` happens off-chain (`PayoutLedgerService`, `payout-scheduler.service.ts`), so its coinbase has the same single-payout-output shape as the solo pool and is affected identically by this decision.
- **`elektron-net-mempool` (block explorer):** no fixed coinbase output count or position is assumed anywhere in the reviewed code. Reward calculation sums all `vout` values generically (`vout.reduce(...)`), and `OP_RETURN` outputs carry `value = 0`, so an added output does not change the computed reward regardless of count. Payout-address extraction reads `vout[0]` specifically (unaffected by appending later outputs). There is currently no bespoke recognition of the UTXO attestation output at all (neither by index nor by content) - it simply renders through the generic per-output path today. The explorer's standardness check for multiple `OP_RETURN` outputs explicitly excludes coinbase transactions (`if (vin.is_coinbase) { ... return false; }`), which is already required for today's two coinbase `OP_RETURN`s and applies unchanged to a third. Pool-matching logic reads only the coinbase `scriptsig` and payout addresses, never the `OP_RETURN` outputs. **Conclusion: no code change required in this repo for a fourth, always-last, zero-value `OP_RETURN` to render and index correctly.**
- **`elektron-net-electrs` (Electrum-protocol indexer):** the UTXO/address-indexing visitor (`src/index.rs`, `visit_tx_out()`) already skips every output for which `script.is_op_return()` is true, generically, regardless of position or count. There is no Elektron-specific attestation-parsing code anywhere in this repo. **Conclusion: no code change required.**
- **`elektron-net-electrum` (wallet):** unaffected by construction - it never builds or parses coinbase transactions; its own, unrelated `script(OP_RETURN ...)` capability for ordinary spends is covered separately in Section 8.

No repo reviewed has code that assumes an exact coinbase output count, so an always-appended-last fourth output requires no follow-up change outside `elektron-net-pool`/`elektron-net-ppool` themselves.

## 7. Checklist

- [x] Decide whether the third output is informational-only or protocol-relevant - **informational-only** (Section 1, Section 5)
- [x] Decide the wire format at a high level - **free-form text, single data push, no versioning/TLV** (Section 5)
- [x] Decide placement - always after the existing required outputs, i.e. last (Section 5.1)
- [x] Resolve the collision risk with the witness-commitment prefix and the attestation's `<height><32 bytes>` shape - resolved structurally via single-push encoding (Section 5.2), not via byte-pattern avoidance
- [x] Decide whether `ExtractCoinbaseUTXOAttestation()` should gain a uniqueness check - **decided against** (Section 5, "Explicitly decided against")
- [x] Review impact on `elektron-net-mempool`/`-electrs` and wallet integrations - **done, no changes required** (Section 6)
- [ ] Implement the pool-side change in `elektron-net-pool` (`MiningJob.ts`, plus operator configuration and spec updates)
- [ ] Implement the same change in `elektron-net-ppool` (`MiningJob.ts`, plus operator configuration and spec updates)
- [ ] Update `mining-pool-integration.md` once the pool-side implementation exists, to describe `vout[3]` as part of the documented coinbase layout
- [ ] Decide the operator-configuration surface in each pool repo (environment variable name, default value, maximum length) at implementation time

## 8. Scope Boundary: This Document Is About the Coinbase, Not Ordinary Wallet Transactions

Everything above (Sections 2-6) is specific to the **coinbase transaction** - the first transaction of a block, built only by miners and pools via `getblocktemplate`. Ordinary wallet-to-wallet transactions never contain a witness commitment or a UTXO attestation output; those two outputs only ever exist in the coinbase. Nothing in Sections 2-6 constrains what a normal spend transaction can contain, and a message-carrying `OP_RETURN` added by a wallet to its own transaction has no interaction with the coinbase mechanics above - there is no shared position, no shared magic-byte space, and no shared validation path.

## 9. Wallet-Level Message OP_RETURN: Already Possible Today, No Code Change

Separately from the coinbase decision above, the question came up whether a wallet could let a user attach a free-form message as an extra `OP_RETURN` output on an ordinary send transaction (payment output, optional change output, plus a message output). Checked directly against the current `elektron-net-electrum` code (a fork of Electrum that carries this feature over unmodified from upstream):

- The Send tab's own inline help text already documents the syntax: *"an arbitrary on-chain script, e.g.: `script(OP_RETURN deadbeef)`"* (`electrum/gui/qt/send_tab.py:80`).
- `PaymentIdentifier.parse_output()` (`electrum/payment_identifier.py:476-489`) tries to parse a recipient line as a normal address first; if that fails, it matches the `script(...)` syntax via `RE_SCRIPT_FN` and hands the contents to `parse_script()`.
- `PaymentIdentifier.parse_script()` (`electrum/payment_identifier.py:491-500`) builds a raw script from the parsed tokens - `OP_`-prefixed words become opcodes (so `OP_RETURN` is recognized), anything else is treated as a hex data push - and the result becomes an ordinary zero-value `PartialTxOutput`.

**Concretely, today, with zero code changes:** a user can enter two recipient lines in the Send tab, e.g. a normal payment address plus `script(OP_RETURN 48656c6c6f), 0`, and the wallet builds a transaction with the payment (and any change) plus a message `OP_RETURN` output - structurally the same shape as Section 8 describes, and unrelated to the coinbase-specific mechanics in Sections 2-6.

Caveats worth naming for anyone revisiting this later: this is raw, technical input with no dedicated "attach a message" UI affordance, and the output is still subject to the node's ordinary relay-policy limits on datacarrier output size (independent of anything in this document).

## 10. Open Questions

1. ~~Is there an actual use case that justifies the added coinbase weight?~~ **Resolved:** yes, pool-operator identification/messaging, decided in Section 1/5.
2. ~~Should the field be structured (versioned TLV) from day one, or free-form text to start?~~ **Resolved:** free-form text (Section 5).
3. ~~Should this be informational-only, or should any node/indexer software ever depend on its contents?~~ **Resolved:** informational-only; Section 6 confirms `elektron-net-mempool`/`-electrs` need no changes to remain correct, and neither is expected to start parsing this output's contents as part of this decision.
4. Should `elektron-net-electrum` ever grow a dedicated "attach a message" UI affordance in the Send tab, given the underlying `script(OP_RETURN ...)` mechanism already works (Section 9)? Still open, and unrelated to the coinbase decision above.
5. New: what is the exact operator-configuration surface (environment variable name(s), default value, maximum message length) for `elektron-net-pool` and `elektron-net-ppool`? Left to the implementation step (Section 7 checklist), not decided by this document.
