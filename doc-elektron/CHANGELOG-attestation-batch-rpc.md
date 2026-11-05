# Changelog: `getattestationbatch` RPC

**Status:** Implemented on the branch `photonpool`, not yet released.
**Related:** `guideline-attestation-batch-rpc.md` (interface, limits, tests), `BITCOIN_CORE_DIFF.md` §3.3 and §6, `CHANGELOG-muhash-attestation.md`.

A dated log of what the change adds and what it does not touch.

---

## 2026-11-05

**New RPC `getattestationbatch`** (`src/rpc/mining.cpp`, category `mining`). Builds one block template for the current tip and returns, for every requested coinbase variant (up to 4096 per call), the UTXO attestation and the finished coinbase. Mining pools with many payout scripts need one call per tip instead of one `getblocktemplate` call per payout script.

**New functions** in `src/validation.cpp`: `ComputeAttestationBaseState()` (the tip's MuHash accumulator with the non-coinbase transactions of a candidate block applied, with the same rules as the incremental path of `ComputeBlockUTXOAttestationHash()`) and `AttestationForCoinbase()` (the attestation of one stripped coinbase on top of that state). Both are called only by the new RPC.

**Argument conversion** for the command line client in `src/rpc/client.cpp` (`getattestationbatch` takes one JSON object).

**What is not changed.** Only lines are added; no existing function, RPC, consensus rule, block validation, activation height or option is modified. A block assembled from the response is validated by the regular path (`submitblock`) like any other block, and a node without this RPC accepts it.

**No option.** The RPC is always available and does no work unless it is called. Use the normal RPC access control (cookie or `rpcauth`, `rpcwhitelist`, do not expose the RPC port).

**Limits** (all refused with an RPC error before any work is done): 1 to 4096 candidates; at most 1000 outputs per list and 10,000 bytes per script; at most 400,000 outputs and 8 MiB of scripts per request; a coinbase that would need more than half of the block weight; values are checked against the remaining block reward before they are added. At most two calls run at the same time, further calls fail at once with "busy". `cs_main` is held only while the template and the attestation base state are taken.

**Tests.** `test/functional/rpc_getattestationbatch.py`: blocks built from the response (single output, remainder output, tail outputs, many outputs with a scriptSig suffix, a template with a dependent transaction pair) are accepted by a second node that only validates, both nodes end with the same tip and the same UTXO set hash, a block with an altered attestation is rejected, invalid requests and the largest batch are covered. Registered in `test/functional/test_runner.py`.

**Sanitizers.** The functional test and the neighbouring attestation test ran against a node built with `-DSANITIZERS=address,undefined` without a report (leak detection off).

**Known open points.** Independent review; measurement of the `cs_main` hold time and CPU cost on a mainnet sized UTXO set; the helper `ComputeAttestationBaseState()` repeats the rules of the consensus path instead of sharing code with it, so a later change of those rules has to be mirrored there (the functional test is the guard).
