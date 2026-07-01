# Fix Report: Snapshot Bootstrap Trust Gap

**Status:** Confirmed via code review
**Severity:** Low impact today (single-miner network), high if left unfixed before the network decentralizes.
**Affected files:** `src/net_processing.cpp`, `src/net_processing.h`, `src/init.cpp`, `src/validation.cpp`
**Related:** `doc-elektron/AUDIT_PRUNING_SNAPSHOT.md`, `BITCOIN_CORE_DIFF.md` §2.2, §2.3, §9.4, `doc-elektron/fix-report-utxo-attestation-scalability.md`

---

## 1. Summary

A node bootstrapping via automatic UTXO snapshot (the normal path once the network is past the first checkpoint at height 197,280) currently accepts the UTXO hash reported by the **first peer** that responds to `GETUTXOSNAPSHOT`, without cross-checking it against other peers, and without verifying it against the checkpoint block's on-chain coinbase attestation — because that verification step is skipped whenever the checkpoint block body isn't already on local disk, which is true for essentially every fresh bootstrap.

This means a single malicious or compromised peer — or an attacker who simply wins the race to respond first — can hand a new node a fabricated but internally self-consistent UTXO state, and nothing in the current code catches it before activation.

## 2. Root Cause

### 2.1 Unconditional trust on first response (`net_processing.cpp`)

In the `UTXOSNAPSHOT` message handler:

```cpp
if (msg_type == NetMsgType::UTXOSNAPSHOT) {
    ...
    LOCK(m_snapshot_download_mutex);
    m_snapshot_peers[checkpoint_hash].insert(pfrom.GetId());
    if (!m_snapshot_downloads.count(checkpoint_hash)) {
        ...
        dl.utxo_hash = utxo_hash;   // <-- taken from whichever peer responds first
        ...
        m_snapshot_downloads[checkpoint_hash] = std::move(dl);
    }
}
```

The `if (!m_snapshot_downloads.count(...))` guard means only the *first* response for a given checkpoint sets `utxo_hash`. Later responses from other peers are tracked in `m_snapshot_peers` (used only to decide who to request data chunks from) but are **never compared** against the already-recorded hash for agreement, and disagreement is never logged or acted on.

### 2.2 On-chain verification skipped for fresh bootstrap (`init.cpp`)

In `MaybeActivateAutomaticSnapshot`:

```cpp
const CBlockIndex* checkpoint_index = chainman.m_blockman.LookupBlockIndex(metadata.m_base_blockhash);
if (checkpoint_index && checkpoint_index->nHeight > 0) {
    CBlock checkpoint_block;
    if (chainman.m_blockman.ReadBlock(checkpoint_block, *checkpoint_index)) {
        const auto attestation = ExtractCoinbaseUTXOAttestation(*checkpoint_block.vtx[0], checkpoint_index->nHeight);
        if (!attestation || *attestation != expected_hash) {
            // reject and remove snapshot
            ...
        }
        LogInfo("[snapshot] Snapshot hash verified against on-chain attestation at height %d.\n", ...);
    } else {
        LogInfo("[snapshot] Checkpoint block %s not on disk; will verify snapshot content against .hash sidecar.\n", ...);
        // <-- no verification happens here; execution falls through
    }
}

auto activation_result = chainman.ActivateSnapshot(afile, metadata, false, /*verify_assumeutxo_hash=*/false, expected_hash);
```

`ReadBlock` only succeeds if the checkpoint block body is already present in the local `blocks/` directory. For a node performing a fresh bootstrap — the scenario this entire mechanism exists for — that block has, by construction, not been downloaded yet. So the `else` branch is the common case, not the exception, and it does nothing except log a message before proceeding straight to `ActivateSnapshot()`.

### 2.3 What `ActivateSnapshot` does and doesn't check

`ActivateSnapshot` does confirm that `snapshot_start_block` is a real header, part of the best-known (heaviest) chain, via `CBlockIndexWorkComparator` and `m_best_header->GetAncestor(...)`. This is a genuine, cryptographically sound check — the header can't be forged cheaply. But it says nothing about the block's **contents**. `PopulateAndValidateSnapshot` only checks that the downloaded `.dat` content matches `expected_hash` — a value that, per §2.2, was never itself verified against the block it claims to represent.

**Net effect:** the header is authenticated; the UTXO state claimed to result from that header's block is not.

## 3. Attack Scenario

1. A new node starts IBD, syncs headers (cheap, PoW-verified, unforgeable), and reaches the point where `MaybeRequestSnapshot()` triggers a `GETUTXOSNAPSHOT` broadcast.
2. An attacking peer — or the first peer to respond, benign or not — replies with `UTXOSNAPSHOT` carrying a `utxo_hash` for a real, valid checkpoint block height.
3. The attacker serves `.dat` snapshot data via `GETSNAPSHOTDATA`/`SNAPSHOTDATA` that is internally consistent with the hash it advertised (trivial: the attacker generates both together).
4. `PopulateAndValidateSnapshot` passes (content matches `expected_hash`). `ActivateSnapshot` passes (the header is real and heaviest-chain). `MaybeActivateAutomaticSnapshot`'s on-chain check is skipped (§2.2). The forged UTXO set is activated as the node's starting state.
5. The victim node now believes an incorrect balance/UTXO state for every address, with no error surfaced.

This requires the attacker to control (or race to be first among) the peers a new node happens to connect to during that specific bootstrap window — an eclipse-style attack, not a 51% hashpower attack. It gets easier, not harder, as the network grows and a fresh node can no longer assume "the only peer I can reach is the project's own node."

## 4. Proposed Fix

Two complementary layers. **Fix A is the authoritative defense** (cryptographically grounded, no added trust assumption beyond what the design already relies on). **Fix B is defense-in-depth** for cases where Fix A can't complete immediately, and a cheap early warning signal.

### 4.1 Fix A — Fetch and verify the checkpoint block's coinbase before activating (primary fix)

**Principle:** the checkpoint block itself is small, current (within the 137-day retention window by construction — bootstrap always targets the *latest* checkpoint), and should be available from any peer that has fully synced past it. There's no reason to skip verifying it just because it isn't already on disk locally — it should be fetched specifically for this purpose.

**Concrete change in `init.cpp` / bootstrap flow:**

- Before calling `ActivateSnapshot()`, make on-chain attestation verification a **hard precondition**, not a best-effort side check. If the checkpoint block isn't on disk, actively request it — don't just log and continue.
- Reuse existing block-fetch machinery: `PeerManagerImpl::FetchBlock(peer_id, block_index)` (already implemented in `net_processing.cpp`) can request the specific checkpoint block from a peer known to have it (via `BlockRequestAllowed` / peers with sufficient `pindexBestKnownBlock` height — this is already tracked).
- Suggested flow:

```cpp
// New helper, called from MaybeActivateAutomaticSnapshot before ActivateSnapshot():
bool VerifyCheckpointAttestationBeforeActivation(
    ChainstateManager& chainman, PeerManager& peerman,
    const CBlockIndex& checkpoint_index, const uint256& expected_hash)
{
    CBlock checkpoint_block;
    if (chainman.m_blockman.ReadBlock(checkpoint_block, checkpoint_index)) {
        // Already have it locally — verify immediately as today.
        const auto attestation = ExtractCoinbaseUTXOAttestation(
            *checkpoint_block.vtx[0], checkpoint_index.nHeight);
        return attestation && *attestation == expected_hash;
    }

    // Not on disk: actively fetch it before proceeding. This blocks
    // activation of this snapshot cycle but not the node — headers sync
    // and other work continue; MaybeActivateAutomaticSnapshot will simply
    // retry on its next scheduled tick (already runs every 30s).
    // (Pseudocode — actual implementation should hook into the existing
    // block-request/response cycle asynchronously rather than blocking.)
    peerman.RequestSpecificBlock(checkpoint_index); // wraps FetchBlock() with peer selection
    return false; // not yet verified this cycle — caller must not activate
}
```

- `MaybeActivateAutomaticSnapshot` should **not call `ActivateSnapshot()`** until `VerifyCheckpointAttestationBeforeActivation` returns `true`. On `false`, it should leave the snapshot file in place, log clearly that it's pending on-chain verification, and let the next scheduled invocation (already every 30s) re-check once the block has arrived.
- Once the checkpoint block is received (via the normal `BLOCK` message path, `ProcessBlock`), a lightweight hook should re-trigger `MaybeActivateAutomaticSnapshot` immediately rather than waiting for the next 30s tick, to avoid unnecessary bootstrap delay.

**Bandwidth note:** the checkpoint block is an ordinary 60-second block, not the full history — fetching one full block is cheap. A future optimization could add a lean message type (e.g. `GETCOINBASEATTESTATION` returning just the coinbase tx + a Merkle proof against the block's `hashMerkleRoot`) to avoid pulling the rest of the block's transactions, but this is a nice-to-have, not required for the fix.

**Fallback if the checkpoint block is genuinely unreachable from any peer:** should not happen for the *current* checkpoint by design (it's inside the retention window), but if it does (e.g. degenerate low-peer-count situations), the node should **refuse to activate** the unverified snapshot and fall back to Fix B as an explicit, loudly-logged reduced-security mode — never activate silently.

### 4.2 Fix B — Cross-check `UTXOSNAPSHOT` responses across multiple peers (defense-in-depth)

**Principle:** even before the heavier Fix A verification completes, don't commit to a single peer's claimed hash. Require agreement from several independent peers before treating a hash as a serious candidate for download and activation.

**Suggested struct change in `net_processing.cpp` (`SnapshotDownload`):**

```cpp
struct SnapshotDownload {
    uint256 checkpoint_hash;
    int checkpoint_height{0};
    uint64_t file_size{0};
    std::map<uint64_t, uint64_t> received_ranges;
    fs::path temp_path;
    fs::path final_path;
    bool completed{false};
    std::map<NodeId, std::chrono::steady_clock::time_point> last_request_time;
    std::chrono::steady_clock::time_point last_progress_time;

    // --- Fix: track per-peer reported hashes instead of trusting the first one ---
    std::map<NodeId, uint256> reported_hashes;
    uint256 utxo_hash;                 // only meaningful once hash_finalized == true
    bool hash_finalized{false};
    static constexpr size_t MIN_CONFIRMING_PEERS = 3;

    ...
};
```

**Updated `UTXOSNAPSHOT` handler logic:**

```cpp
if (msg_type == NetMsgType::UTXOSNAPSHOT) {
    int checkpoint_height;
    uint256 checkpoint_hash;
    uint256 utxo_hash;
    uint64_t file_size;
    vRecv >> checkpoint_height >> checkpoint_hash >> utxo_hash >> file_size;

    LOCK(m_snapshot_download_mutex);
    m_snapshot_peers[checkpoint_hash].insert(pfrom.GetId());

    auto& dl = m_snapshot_downloads[checkpoint_hash]; // default-constructs if absent
    dl.checkpoint_hash = checkpoint_hash;
    dl.checkpoint_height = checkpoint_height;
    dl.reported_hashes[pfrom.GetId()] = utxo_hash;

    if (dl.hash_finalized) {
        if (utxo_hash != dl.utxo_hash) {
            LogWarning("[snapshot] peer=%d reported conflicting UTXO hash for checkpoint %s "
                       "(expected %s, got %s) — ignoring this peer for this snapshot\n",
                       pfrom.GetId(), checkpoint_hash.ToString(),
                       dl.utxo_hash.ToString(), utxo_hash.ToString());
            // Optional: track a conflict counter per peer and disconnect/discourage
            // repeat offenders via Misbehaving().
        }
        return;
    }

    // Tally agreement, ideally weighted by peer network diversity (see note below).
    std::map<uint256, std::vector<NodeId>> tally;
    for (const auto& [nid, h] : dl.reported_hashes) tally[h].push_back(nid);

    for (const auto& [h, peers] : tally) {
        if (peers.size() >= SnapshotDownload::MIN_CONFIRMING_PEERS) {
            dl.utxo_hash = h;
            dl.file_size = file_size;
            dl.hash_finalized = true;
            LogInfo("[snapshot] Hash for checkpoint %s confirmed by %d peers: %s\n",
                    checkpoint_hash.ToString(), peers.size(), h.ToString());
            // proceed to initialize file paths / temp file as today
            break;
        }
    }

    if (!dl.hash_finalized) {
        LogDebug(BCLog::NET, "[snapshot] Waiting for more peer agreement on checkpoint %s "
                 "(%d/%d reports so far)\n", checkpoint_hash.ToString(),
                 dl.reported_hashes.size(), SnapshotDownload::MIN_CONFIRMING_PEERS);
    }
}
```

**Important limitation to document, not hide:** peer-count agreement alone is Sybil-able — an attacker who can open enough connections to a target node can satisfy `MIN_CONFIRMING_PEERS` by themselves. This is why Fix B is explicitly a secondary layer, not a replacement for Fix A. It's still useful because:
- It raises attacker cost from "win one race" to "control 3+ connection slots on the victim," which existing eclipse-attack mitigations (outbound peer diversity, `NetGroupManager` netgroup bucketing already used for addrman) partially defend against.
- It gives an early, cheap warning signal — a real network split or attack attempt would show up as persistent disagreement in the logs before Fix A's heavier block-fetch-and-verify even completes.

**Suggested refinement:** weight `MIN_CONFIRMING_PEERS` by netgroup/ASN diversity (reuse `NetGroupManager`, already present) rather than raw peer count, so an attacker needs distinct network origins, not just distinct connections.

### 4.3 Interaction between Fix A and Fix B

Recommended combined flow:
1. `UTXOSNAPSHOT` responses accumulate under Fix B's cross-check. Data download only starts once `MIN_CONFIRMING_PEERS` agree (cheap, fast, filters out lone bad actors early).
2. Once the `.dat` is fully downloaded and internally verified against the agreed hash (existing `PopulateAndValidateSnapshot` logic — unchanged), Fix A's checkpoint-coinbase fetch-and-verify runs as the final, authoritative gate before `ActivateSnapshot()` is called.
3. Only if both layers pass does the snapshot activate. If Fix A fails (mismatch), reject and blacklist the reported hash, re-open Fix B's tally, and try again with remaining/new peers.

### 4.4 Configuration / operational suggestions

- Add a startup flag, e.g. `-snapshotverification=strict|relaxed` (default `strict`), where `strict` requires both Fix A and Fix B to pass, and `relaxed` (for controlled test networks) allows Fix B alone or skips both with a loud warning. This avoids blocking low-peer-count development/testing while keeping the public network default safe.
- Log every rejected snapshot (hash mismatch, insufficient confirmations, checkpoint block unreachable) at `LogWarning` level with enough detail to be diagnosable, not just `LogDebug`.

## 5. Testing Recommendations

1. **Functional test — malicious first responder:** simulate a peer that responds first to `GETUTXOSNAPSHOT` with a fabricated hash + matching fake `.dat`, and a second, honest peer with the real checkpoint block available. Assert the node ends up with the correct (real) UTXO state, not the fabricated one, once Fix A's fetch-and-verify completes.
2. **Functional test — peer disagreement:** two peers report different hashes for the same checkpoint before `MIN_CONFIRMING_PEERS` is reached from either. Assert the node does not start downloading data from either until consensus is reached, and that the conflict is logged.
3. **Functional test — checkpoint block unreachable:** simulate a scenario where no peer can serve the checkpoint block body. Assert the node does **not** silently activate the unverified snapshot, and instead logs a clear warning and (per §4.4) either blocks or requires explicit `-snapshotverification=relaxed`.
4. **Regression test — low-peer-count / solo-node bootstrap:** ensure the existing single-node / early-network bootstrap path still completes successfully, either because Fix A alone succeeds against your own node, or via the `relaxed` mode.
5. **Unit test:** `ExtractCoinbaseUTXOAttestation` / `ComputeBlockUTXOAttestationHash` already have coverage per `src/test/elektron_simulation.cpp` — extend with a case feeding a mismatched hash through the new `VerifyCheckpointAttestationBeforeActivation` path to confirm it returns `false` and blocks activation.

## 6. Priority Recommendation

The chain has not yet reached the first checkpoint at height 197,280, so `MaybeRequestSnapshot()` has not activated on the live network and this path hasn't been exercised outside testing yet. This is worth being precise about, because the trigger condition is **chain height, not miner count or network size** — `MaybeRequestSnapshot()` fires for any new node whose sync gap exceeds `MANDATORY_PRUNE_DEPTH`, regardless of how many miners are on the network. That means this gap will become live the moment the chain crosses height 197,280, automatically, even in the current single-miner setup — the very first node that bootstraps fresh after that point (including a node run by the project itself, or any third party) will go through the unverified path described above. This gives a concrete, currently-known deadline rather than a vague "before it matters": **fix before the chain reaches height 197,280.**
