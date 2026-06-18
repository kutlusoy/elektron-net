# (OBSOLETE) Hard Fork Documentation: Elektron Net v3.0.1 (Stoic Awakening)

## Document Information
- **Version:** v3.0.1
- **Codename:** Stoic Awakening
- **Type:** Mandatory Hard Fork (Consensus Change)
- **Status:** Superseded by v4.0 genesis restart (Stoic Awakening active from block 1)
- **Date:** 2026-05-10

---

## 1. Executive Summary

Following the departure of a high-hashrate miner, the Elektron Net mainnet difficulty remains artificially elevated. The standard 2,016-block retargeting window is too slow to compensate for the sudden drop in network hashrate, resulting in:

- Block discovery delays of 3--5 minutes (target: 60 seconds)
- Delayed transaction finality
- Increased risk of chain stall if additional miners disconnect

This document describes the technical implementation of **v3.0.1**, which introduces a **Dynamic Difficulty Recovery Mechanism** activated via a hard fork at a predetermined block height.

---

## 2. Problem Statement

### 2.1 Root Cause
The Elektron Net protocol (v3.0.0) uses Bitcoin's standard difficulty retargeting algorithm:
- Retargeting interval: `2,016` blocks
- Target timespan: `2,016 * 60 = 120,960` seconds (~1.4 days)
- Maximum downward adjustment per interval: `4x`

When a dominant miner abruptly leaves, the remaining network hashrate is insufficient to maintain the current difficulty. Because the retargeting only occurs every 2,016 blocks, the network can suffer from excessively long block times for days.

### 2.2 Current Impact (as of v3.0.0)
| Metric | Target | Current (v3.0.0) |
|---|---|---|
| Block Time | 60s | 180--300s |
| Transactions/Hour | ~600 | ~120--200 |
| Time to Next Retarget | Variable | Potentially >2 days |

---

## 3. Proposed Solution

### 3.1 Mechanism Overview
The v3.0.1 hard fork introduces an emergency difficulty recovery rule:

> **If the time elapsed since the last valid block exceeds `120 seconds` (2x `nPowTargetSpacing`), the next block may be mined at the minimum difficulty (`powLimit`).**

After a minimum-difficulty block is found, the difficulty immediately returns to the regular 2,016-block average calculation for the next block. This ensures:
- **Liveness:** The chain continues to progress even under drastically reduced hashrate.
- **Speed to Retarget:** The network reaches the next regular retargeting boundary much faster, allowing organic difficulty stabilization.
- **Safety:** The rule only applies outside the regular retargeting interval and only after a significant delay.

### 3.2 Activation Strategy
This is a **height-activated hard fork**, not a simple flag flip. The previous static boolean `fPowAllowMinDifficultyBlocks` has been augmented with a new height-aware parameter to ensure the rule only applies from the fork height onward.

**Why height-activation instead of a simple flag?**
- A simple flag change (`fPowAllowMinDifficultyBlocks = true`) would make the rule retroactively valid for the entire chain history.
- During a chain reorganization, old blocks that were previously valid could suddenly be reinterpreted under the new min-difficulty rule, creating consensus edge cases.
- Height-activation guarantees deterministic behavior: blocks before the fork height are validated under the old rules, blocks at/after the fork height under the new rules.

---

## 4. Technical Implementation

### 4.1 Files Modified

| File | Change Type | Description |
|---|---|---|
| `CMakeLists.txt` | Version Bump | `CLIENT_VERSION_BUILD` changed from `0` to `1` |
| `src/node/protocol_version.h` | Protocol Bump | `PROTOCOL_VERSION` bumped from `70016` to `70017`; `MIN_PEER_PROTO_VERSION` bumped to `70017` to reject v3.0.0 peers |
| `src/consensus/params.h` | New Parameter | Added `int MinDifficultyActivationHeight = -1` to `Consensus::Params` struct |
| `src/kernel/chainparams.cpp` | Configuration | Set `consensus.MinDifficultyActivationHeight = 137035` in `CMainParams` |
| `src/pow.cpp` | Logic Update | `GetNextWorkRequired()` and `PermittedDifficultyTransition()` now check activation height |
| `src/node/miner.cpp` | Logic Update | `UpdateTime()` and `WaitAndCreateNewBlock()` now respect activation height for mining templates |
| `src/test/pow_tests.cpp` | Tests | Added `min_difficulty_activation_height` test case |

### 4.2 Consensus Parameter

```cpp
// src/consensus/params.h
struct Params {
    // ... existing fields ...
    bool fPowAllowMinDifficultyBlocks;
    int MinDifficultyActivationHeight = -1;  // NEW: -1 = disabled, >=0 = activation height
    // ...
};
```

**Semantics:**
- `-1` (default): The height-based min-difficulty rule is disabled. Only `fPowAllowMinDifficultyBlocks` controls the behavior (used for Testnet/Regtest).
- `>= 0`: The min-difficulty recovery rule becomes active for any block at or above this height, regardless of the `fPowAllowMinDifficultyBlocks` flag.

### 4.3 Mainnet Configuration

```cpp
// src/kernel/chainparams.cpp (CMainParams constructor)
consensus.fPowAllowMinDifficultyBlocks = false;
consensus.MinDifficultyActivationHeight = 137035; // v3.0.1 hard fork: Stoic Awakening
```

**CRITICAL:** The activation height is set to **137035**. Before releasing v3.0.1 binaries, verify that this height provides sufficient notice (minimum 72 hours) from the current chain tip.

### 4.4 Difficulty Calculation Logic

#### `GetNextWorkRequired` (`src/pow.cpp`)

Outside the regular retargeting interval, the function now evaluates:

```cpp
bool allowMinDifficulty = params.fPowAllowMinDifficultyBlocks ||
    (params.MinDifficultyActivationHeight != -1 &&
     (pindexLast->nHeight + 1) >= params.MinDifficultyActivationHeight);

if (allowMinDifficulty) {
    // If the new block's timestamp is more than 2 * nPowTargetSpacing (120s)
    // then it MUST be a min-difficulty block.
    if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 2)
        return nProofOfWorkLimit;
    // ... otherwise return last non-special difficulty
}
```

#### `PermittedDifficultyTransition` (`src/pow.cpp`)

This function normally enforces the 4x difficulty adjustment limits. It now also checks the activation height:

```cpp
bool allowMinDifficulty = params.fPowAllowMinDifficultyBlocks ||
    (params.MinDifficultyActivationHeight != -1 && height >= params.MinDifficultyActivationHeight);
if (allowMinDifficulty) return true;
// ... standard 4x limit checks continue ...
```

#### Mining Template Updates (`src/node/miner.cpp`)

- **`UpdateTime`**: When constructing a block template, if the min-difficulty rule is active for the next height, updating the block timestamp may trigger a recalculation of `nBits` via `GetNextWorkRequired`.
- **`WaitAndCreateNewBlock`**: The 20-minute timeout logic that forces a new template (to allow min-difficulty mining) is now gated by the activation height.

### 4.5 P2P Protocol Changes

- **`PROTOCOL_VERSION = 70017`**: Signals support for the new consensus rules.
- **`MIN_PEER_PROTO_VERSION = 70017`**: Nodes running v3.0.0 (`PROTOCOL_VERSION <= 70016`) will be disconnected immediately upon connection attempt. This prevents v3.0.0 nodes from staying on a stale chain and relaying invalid blocks.

---

## 5. Consensus Impact

### 5.1 Hard Fork Characteristics

| Aspect | Detail |
|---|---|
| **Fork Type** | Mandatory Hard Fork |
| **Activation** | Height-based (not time-based) |
| **Non-Upgraded Nodes** | Will reject min-difficulty blocks as invalid |
| **Network Partition Risk** | High if significant hashrate remains on v3.0.0 |
| **Rollback Safety** | No. Once activated, blocks mined under the new rules cannot be re-validated by v3.0.0 nodes. |

### 5.2 Block Validity Matrix

| Node Version | Block < Fork Height | Block >= Fork Height (standard) | Block >= Fork Height (min-difficulty) |
|---|---|---|---|
| v3.0.0 | Valid | Valid | **Invalid** |
| v3.0.1 | Valid | Valid | Valid |

---

## 6. Operator Instructions

### 6.1 For Node Operators
1. **Upgrade** to v3.0.1 before the activation height.
2. No configuration changes are required.
3. After activation, monitor block times. They should stabilize back toward 60 seconds within a few hours.

### 6.2 For Miners
1. **Upgrade** mining software and node to v3.0.1.
2. No changes to mining logic are required; the node automatically produces min-difficulty block templates when appropriate.
3. If network hashrate is very low and block times exceed 120 seconds, the next template will automatically target `powLimit`.

### 6.3 For Exchanges / Services
1. **Upgrade** full nodes to v3.0.1.
2. Expect temporary confirmation time improvements once the fork activates.
3. No wallet or RPC changes are involved.

---

## 7. Testing

### 7.1 Unit Tests
A dedicated test case has been added to `src/test/pow_tests.cpp`:

```cpp
BOOST_AUTO_TEST_CASE(min_difficulty_activation_height)
```

**Coverage:**
- Verifies that `GetNextWorkRequired` returns standard difficulty **before** the activation height, even with a >120s delay.
- Verifies that `GetNextWorkRequired` returns `powLimit` **at** the activation height with a >120s delay.
- Verifies that `PermittedDifficultyTransition` is strict before activation and permissive at/after activation.

### 7.2 Recommended Additional Tests
Before mainnet activation, the following tests should be run:

1. **Regtest Simulation:**
   - Set `MinDifficultyActivationHeight` to a low value in a custom regtest params.
   - Mine a chain up to the activation height.
   - Introduce a >120s delay and verify the next block is accepted at min-difficulty.
   - Verify a v3.0.0-simulating node (with old rules) rejects the block.

2. **Reorg Test:**
   - Build a competing chain that crosses the fork height.
   - Ensure the reorganization is handled correctly by v3.0.1 nodes.

3. **Sync Test:**
   - Start a fresh v3.0.1 node and sync from genesis.
   - Ensure it validates all pre-fork blocks under old rules and post-fork blocks under new rules.

---

## 8. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| v3.0.0 miners refuse to upgrade | Medium | High (chain split) | Set activation height far enough in the future (min. 3--5 days). Community coordination. |
| Min-difficulty block flood | Low | Medium | Rule only applies after >120s delay; difficulty resets immediately for the next block. |
| Reorg across fork height | Low | Medium | Height-activation ensures deterministic validation; old blocks are never re-evaluated under new rules. |
| Placeholder height not updated | High | Critical | Release checklist must include updating `999999` to the real height. |

---

## 9. Release Checklist

- [ ] Set `consensus.MinDifficultyActivationHeight` in `src/kernel/chainparams.cpp` to the real target height
- [ ] Run full test suite (`make check`)
- [ ] Run functional tests (especially `feature_block` and `mining_getblocktemplate`)
- [ ] Build release binaries for Windows and Linux
- [ ] Update release notes
- [ ] Announce activation height to community with minimum 72-hour notice
- [ ] Monitor network hash rate and block times as activation approaches

---

## 10. References

- Original Announcement: `Network Upgrade Announcement: Elektron Net v3.0.1 (Stoic Awakening)`
- Bitcoin Difficulty Adjustment Algorithm (BIP-???)
- Testnet Min-Difficulty Rule (existing Bitcoin Core implementation in `src/pow.cpp`)

---

*Mathematics instead of trust. The journey continues.*

The Elektron Net Dev Team
