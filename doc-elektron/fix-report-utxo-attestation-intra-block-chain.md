# Fix Report: UTXO Attestation Fails for Intra-Block Dependent Transactions

**Status:** Implemented and tested on branch `slip-44`, shipped in **v4.0.4**. Node crash guard and the `generateblock` fix are unconditional and active immediately. The consensus-relevant fix itself is height-gated and activates on **mainnet at height 170000**; testnet/testnet4/regtest are already active at their existing low heights for testing.
**Severity:** High. Root cause was crashing the node process outright and, independently of the crash, was rejecting a normal, expected block shape network-wide once past `MuhashAttestationActivationHeight` (137000 on mainnet).
**Affected files:** `src/validation.cpp`, `src/validation.h`, `src/node/interfaces.cpp`, `src/rpc/mining.cpp`, `src/consensus/params.h`, `src/kernel/chainparams.cpp`, `test/functional/feature_utxo_attestation_intra_block_chain.py`, `test/functional/feature_generateblock_utxo_attestation.py`
**Related:** `doc-elektron/fix-report-utxo-attestation-scalability.md` (the incremental MuHash design this bug lives inside), `doc-elektron/CHANGELOG-muhash-attestation.md`, `doc-elektron/CHANGELOG-Release-v4.0.4.md`, `BITCOIN_CORE_DIFF.md` section 2.2

---

## 1. Symptom

A faucet operator's node crashed while calling `getblocktemplate`, dropping the connection mid-request ("EOF reached") instead of returning a normal RPC error or result. `docker logs` showed a hard C++ assertion failure and process abort, followed by a container restart under `restart: unless-stopped`, which masked the underlying failure as a brief, unremarkable service blip.

Separately, and initially treated as an unrelated symptom, the same node's mempool contained several of its own payout transactions that stayed unconfirmed indefinitely, including pairs where one transaction spent another transaction's still-unconfirmed change output (an ordinary wallet-chaining pattern, not a bug in the wallet). Blocks observed on the network around that time carried only a coinbase transaction, or at most a single, non-chained transaction, never a chained pair.

## 2. Root cause

`ComputeBlockUTXOAttestationHash()` (`src/validation.cpp`) has two computation paths, selected by `MuhashAttestationActivationHeight`:

- **Legacy path** (pre-activation, or always on mainnet before height 137000): replays the block via `UpdateCoins()` against a real `CCoinsViewCache`, which is updated after every transaction it processes. A later transaction spending an earlier one's output in the same block resolves correctly, because the cache already has that earlier transaction's output by the time the later one is checked.
- **Incremental path** (post-activation): clones the persistent MuHash accumulator and applies only this block's coin changes, using a separate `lookup_view` built once from the confirmed chain tip, to look up each transaction's spent inputs.

Before this fix, the incremental path never updated `lookup_view` with each transaction's own outputs as it iterated the block. It only fed the MuHash accumulator (`candidate`) itself. A later transaction spending an earlier one's output in the same block would look up a coin that only exists in `candidate`, not in `lookup_view`, see it as spent, and the function returned `std::nullopt`.

This is not a rare shape. It is the normal result of CPFP or any wallet that reuses its own unconfirmed change, which is exactly what happened here: the faucet's own payout chaining produced blocks where the assembler naturally wanted to include both a parent and its child transaction together, since that is how CPFP fee-bumping is supposed to work.

`ComputeBlockUTXOAttestationHash()` is called from two places, and the `std::nullopt` return had a different, equally serious consequence in each:

- **Mining** (`src/node/miner.cpp`, `CreateNewBlock()`, around line 211): logs the failure and returns `nullptr` for the whole block template.
- **Validation** (`src/validation.cpp`, `ConnectBlock()` via `ValidateUTXOCheckpoint()`): the block is rejected as consensus-invalid (`bad-utxo-attestation-compute`).

Neither of these is limited to the node that happens to be mining. `ConnectBlock()` runs for every block on every full node, whether it was mined locally or received from a peer, so this affected the whole network's ability to accept a block containing an intra-block dependent-transaction pair, not just the faucet's own node.

### 2.1 Why the node crashed, not just rejected the template

`MinerImpl::createNewBlock()` (`src/node/interfaces.cpp`) passed `CreateNewBlock()`'s result straight into `BlockTemplateImpl`'s constructor without checking it for null:

```cpp
return std::make_unique<BlockTemplateImpl>(assemble_options, BlockAssembler{...}.CreateNewBlock(), m_node);
```

`BlockTemplateImpl`'s constructor asserts its `block_template` argument is non-null:

```cpp
explicit BlockTemplateImpl(...) : ...
{
    assert(m_block_template);
}
```

A failed `CreateNewBlock()` call (returning `nullptr`, exactly the case described in section 2) therefore hit this `assert()` and called `abort()`, taking down the entire node process, not just the RPC request. This is the crash observed live.

## 3. The fix

Three changes, addressing the crash, the actual computation bug, and a second, independently discovered defect in `generateblock`.

### 3.1 Crash guard (unconditional, not height-gated)

`MinerImpl::createNewBlock()` (`src/node/interfaces.cpp`, around line 1006) now checks `CreateNewBlock()`'s result before constructing `BlockTemplateImpl`:

```cpp
auto block_template{BlockAssembler{...}.CreateNewBlock()};
if (!block_template) return {};
return std::make_unique<BlockTemplateImpl>(assemble_options, std::move(block_template), m_node);
```

A failed template creation now surfaces as a normal "no template available" result (an RPC error via `getblocktemplate`/`generateblock`'s own null checks, already present at the RPC layer), instead of aborting the process. This has no consensus effect and needs no activation height: it only changes how a local node reacts to a failure it would otherwise have hit anyway.

### 3.2 Root-cause fix: keep `lookup_view` in sync (height-gated)

`ComputeBlockUTXOAttestationHash()`'s incremental path (`src/validation.cpp`, around line 2378) now updates `lookup_view` with each transaction's own spent and created outputs as it iterates the block, mirroring what `UpdateCoins()` already does for the legacy path:

```cpp
if (fix_active) lookup_view.SpendCoin(txin.prevout);
...
if (fix_active) lookup_view.AddCoin(COutPoint(cur_tx.GetHash(), o), std::move(new_coin), false);
```

A later transaction spending an earlier one's output in the same block now resolves correctly instead of looking like a missing or already-spent coin.

This is a consensus rule change: a block shape that every node previously rejected as invalid becomes valid. Applying it unconditionally the moment the code deploys would let an upgraded node accept a block that a not-yet-upgraded node still rejects, a chain split risk the moment adoption is uneven. It is therefore gated behind a new `Consensus::Params::IntraBlockAttestationFixActivationHeight` (`src/consensus/params.h`) and `IsIntraBlockAttestationFixActive()` (`src/validation.h`, around line 124), following the same pattern already used for `MuhashAttestationActivationHeight` and `StoicAwakeningEndHeight`. Below the activation height, `fix_active` is false and every node, upgraded or not, computes it exactly as before.

Heights (`src/kernel/chainparams.cpp`): mainnet **170000** (chosen with the operator directly; mainnet tip was approximately 147000 at the time, and the network currently has only one other independent miner besides the operator's own node, who upgrades reliably, so the coordination risk is lower than for prior consensus changes in this project, though the operator opted for a wider margin than the minimum anyway). Testnet/testnet4 **260**, regtest **110** (kept above `COINBASE_MATURITY`, 100, so a functional test can spend a real matured coinbase output on both sides of the gate).

### 3.3 Persistent MuHash accumulator: unaffected

`ConnectBlock()`'s commit of the block's coin changes to the persistent `m_utxo_muhash` accumulator (`src/validation.cpp`, the loop that runs after `WriteBlockUndo` succeeds) uses `txundo.vprevout[j]`, undo data already recorded by the ordinary `UpdateCoins()` pass earlier in the same function. That pass has never had this bug; it is the same code path the legacy attestation path in section 2 relies on. The persistent accumulator that every future block's attestation is built from was therefore never at risk of silent corruption from this bug, before or after the fix. Confirmed by code inspection, not just by testing: this commit loop does not use `lookup_view` or any code touched by section 3.2 at all.

### 3.4 Separate defect found and fixed: stale attestation in `generateblock`

While testing section 3.2, `generateblock` (`src/rpc/mining.cpp`) was found to fail independently, with a hash mismatch (`bad-utxo-attestation`) rather than the `nullopt` case above, for **any** appended transaction, dependent pair or not.

`generateblock` builds its coinbase via an initial `createNewBlock()` call with `use_mempool=false`, producing a block containing only the coinbase, then appends the caller-supplied transactions afterward. The attestation `CreateNewBlock()` embeds in that first call reflects a coinbase-only block. `RegenerateCommitments()`, already called after the transactions are appended, only redoes the witness commitment and merkle root; it never touched the attestation, so it stayed stale relative to the block's real, final content.

Fixed with a new `RegenerateUTXOAttestation()` (`src/validation.cpp`/`.h`, around line 2477), mirroring the attestation-embedding step already inside `CreateNewBlock()`: strips the stale attestation output and recomputes and re-embeds one against the block's actual final content, recomputing the merkle root again since changing the coinbase changes its own txid. Called from `generateblock`'s handler (`src/rpc/mining.cpp`, around line 403) right after the caller-supplied transactions are appended, before `TestBlockValidity`.

This defect is unrelated to section 2 and not height-gated: it only changes local RPC behavior (whether `generateblock` produces a valid block at all), not what blocks are consensus-valid, so there is no chain-split consideration. `generateblock` is a manual/testing RPC; production mining in this project uses `getblocktemplate` and `submitblock`, which are unaffected by this specific defect.

## 4. Coinbase output ordering: verified not a factor

The pool-integration contract (`doc-elektron/mining-pool-integration.md` section 3.1) documents `coinbase_required_outputs` in the order `[attestation, witness commitment]`, which pools append starting at `vout[1]`. The node's own internal build order (`src/node/miner.cpp`, `CreateNewBlock()`) is the reverse: witness commitment first (via `GenerateCoinbaseCommitment()`), attestation second. Both orderings are safe, because attestation identification is content-based, not position-based: `IsCoinbaseUTXOAttestationOutput()`/`StripAttestationOutput()` (`src/validation.cpp`) scan `vout` for the pattern `OP_RETURN <height> <32-byte hash>` wherever it appears, and a witness commitment's distinct byte pattern (`OP_RETURN 0x24 aa21a9ed <32 bytes>`) cannot match it. Neither this fix nor the coinbase construction it touches assumes a fixed output position.

## 5. Testing

All testing was done locally against a debug build (`-DENABLE_WALLET=ON -DBUILD_TESTS=ON -DENABLE_IPC=OFF`) on regtest.

### 5.1 Regression baseline

Unit suites (`elektron_simulation_tests`, `validation_tests`, `miner_tests`, `coinstatsindex_tests`, `validation_chainstatemanager_tests`, `validation_chainstate_tests`, `coins_tests`, `rpc_tests`) were run both with this change's files in place and with them swapped back to the pre-fix commit. The same failures (a subsidy-amount mismatch, several `CreateAndActivateUTXOSnapshot`-based tests, the known `prune_depth_calculation` mismatch, and `CreateNewBlock_validity`) appear identically in both runs and are specific to this sandboxed build environment (unavailable networking/assumeutxo prerequisites), not introduced by this change. `rpc_tests` alone is fully clean. `feature_muhash_attestation_activation.py` (the sibling functional test) was likewise run against both states and fails identically in both (an unrelated, pre-existing attestation-output-count mismatch in this environment).

### 5.2 New functional coverage

`test/functional/feature_utxo_attestation_intra_block_chain.py`:
- Below the fix height: a dependent-transaction pair fails cleanly (RPC error, not a crash), repeatedly, for as long as it sits in the mempool, since the block assembler keeps trying to include every valid fee-paying transaction. The node stays responsive throughout. The node is restarted with `-persistmempool=0` to discard the stuck pair and move past this phase (a real node would instead need the fix or the activation height to arrive).
- At and after the fix height: a dependent pair mines and confirms normally, and this stays correct across five further blocks, not just the one that crosses the boundary.

`test/functional/feature_generateblock_utxo_attestation.py`:
- A single ordinary transaction appended via `generateblock`, past `MuhashAttestationActivationHeight` but below the intra-block fix height, previously rejected with a hash mismatch, now produces a valid block.
- A dependent pair appended via `generateblock`, past both activation heights, also produces a valid block.

### 5.3 Additional cases checked manually, not yet folded into the committed test suite

- A five-transaction chain (A through E) all in one block: mines correctly.
- Two independent, unrelated two-transaction chains in the same block: mines correctly.
- A three-transaction chain via `generateblock`: produces a valid block.
- `invalidateblock` followed by `reconsiderblock` on a block containing a dependent pair: disconnects and reconnects cleanly, with the recomputed attestation hash identical on both the original connect and the reconnect, confirming the computation is deterministic.

### 5.4 Explicitly not tested

- Real P2P propagation between two independent, physically separate nodes (only local RPC-driven scenarios on a single node were used).
- Behavior against the project's actual, multi-year mainnet chain history (only a fresh regtest chain was used).
- Behavior under heavy load or very large blocks with many simultaneous dependent chains.

These are normal residual risk for a consensus change of this kind, mitigated by the chosen activation height's lead time and the small, known operator base on this network at the time of activation, rather than by additional testing before this release.

## 6. Checklist

- [x] Crash guard in `MinerImpl::createNewBlock()`, unconditional.
- [x] Root-cause fix in `ComputeBlockUTXOAttestationHash()`'s incremental path, height-gated.
- [x] `IntraBlockAttestationFixActivationHeight` added to `Consensus::Params`, set per network.
- [x] Mainnet activation height chosen with the operator (170000).
- [x] Persistent MuHash accumulator commit path confirmed unaffected, by inspection.
- [x] `generateblock` stale-attestation defect found and fixed independently.
- [x] Coinbase output ordering between the node's own build and the documented pool contract confirmed safe.
- [x] Unit test regression baseline compared pre-fix and post-fix.
- [x] New functional tests committed for both the root-cause fix and the `generateblock` fix.
- [x] Additional manual coverage for longer chains, independent chains, and reorg symmetry.
- [ ] Real multi-node P2P propagation test.
- [ ] Verification against real mainnet chain history rather than a fresh regtest chain.
