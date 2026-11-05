# Elektron Net: `getattestationbatch` RPC Guideline

- **Version:** 0.1 (draft)
- **Date:** November 5, 2026
- **Audience:** Node operators, mining pool developers
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) - `src/rpc/mining.cpp` (`getattestationbatch`), `src/validation.cpp` (`ComputeAttestationBaseState`, `AttestationForCoinbase`), `test/functional/rpc_getattestationbatch.py` - treat as ground truth for anything the doc references
- **Consumer:** [`elektron-net-photonpool`](https://github.com/kutlusoy/elektron-net-photonpool)
- **See also:** [`CHANGELOG-attestation-batch-rpc.md`](./CHANGELOG-attestation-batch-rpc.md), [`CHANGELOG-muhash-attestation.md`](./CHANGELOG-muhash-attestation.md)

---

## 1. Purpose

Every coinbase of this chain carries a UTXO attestation. A pool that pays several payout scripts needs one attested coinbase per payout script and per tip. `getblocktemplate` builds one coinbase per call, so a pool with hundreds of payout scripts would need hundreds of calls after every new tip.

`getattestationbatch` builds one block template for the current tip and returns, for every requested coinbase variant, the attestation and the finished coinbase. The transactions of the template are applied to the UTXO accumulator once; each variant then only adds its own outputs.

The RPC does not change consensus, block validation or any existing RPC. A block assembled from its response is validated by the normal path (`submitblock`) like any other block, and every node accepts it without having this RPC.

## 2. Availability

The RPC is always available and costs nothing while nobody calls it: no background work runs. It is only usable from the MuHash attestation activation height on and not during initial block download. It is a mining RPC like `getblocktemplate`; limit who may call it with the usual RPC access control (authenticate with cookie or `rpcauth`, do not expose the RPC port beyond the pool host, use `rpcwhitelist` to limit a pool user to the methods it needs).

## 3. Request and response

Request: `{"candidates": [{"outputs": [{"scriptPubKey": hex, "value": leptons?}], "tail_outputs": [{"scriptPubKey": hex, "value": leptons?}], "script_sig_suffix": hex?}]}`

- `outputs`: payout outputs. At most one output of a candidate may omit its value; it receives the remainder of the block reward.
- `tail_outputs`: outputs after the witness commitment (value defaults to 0).
- `script_sig_suffix`: bytes appended to the coinbase scriptSig prefix (extranonce). The scriptSig must stay between 2 and 100 bytes.

Response: the template fields (`tip`, `height`, `version`, `bits`, `target`, `curtime`, `mintime`, `coinbasevalue`, coinbase fields, `witness_commitment_script`, `transactions`) and `candidates[]` with `attestation_script`, `coinbase_txid` and `coinbase_hex`.

## 4. Limits

- 1 to 4096 candidates per call, at most 1000 outputs per list, scripts up to 10,000 bytes.
- At most 400,000 outputs and 8 MiB of scripts over the whole request.
- A coinbase that would need more than half of the block weight is refused.
- Values are checked against the remaining block reward before they are added.
- At most two calls run at the same time; further calls fail immediately with "busy".
- `cs_main` is held only while the template and the attestation base state are taken. The per-candidate work runs on copies without the lock.

## 5. Tests

`test/functional/rpc_getattestationbatch.py` starts two nodes, one that calls the RPC and one that only validates. Blocks assembled from the response (single output, remainder, tail outputs, many outputs with a scriptSig suffix, a template with a dependent transaction pair) must be accepted by the node that only validates, and both nodes must end with the same tip and the same UTXO set hash. A block with an altered attestation must be rejected. Invalid requests, the limits and the largest allowed batch are covered as well.

## 6. Checklist

- [ ] Independent review of the RPC and the two helper functions.
- [x] Sanitizer run (ASan, UBSan) of the functional test: built with `-DSANITIZERS=address,undefined` (RelWithDebInfo, `-O1`, wallet off), `rpc_getattestationbatch.py` and `feature_utxo_attestation_intra_block_chain.py` passed, `ASAN_OPTIONS=detect_leaks=0`, no report. The same node also passed the limit, acceptance and end-to-end tests of `elektron-net-photonpool` (4096 candidates, parallel calls, blocks of a real pool accepted by a node without the RPC). Not covered: leak detection and a coverage guided fuzzer.
- [ ] Measurement of the `cs_main` hold time and CPU cost on a mainnet sized UTXO set.
- [ ] Decision whether `ComputeAttestationBaseState` should share code with `ComputeBlockUTXOAttestationHash` instead of repeating its rules.
