// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include <script/verify_flags.h>
#include <uint256.h>

#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <vector>

namespace Consensus {

/**
 * A buried deployment is one where the height of the activation has been hardcoded into
 * the client implementation long after the consensus change has activated. See BIP 90.
 * Consensus changes for which the new rules are enforced from genesis are not listed here.
 */
enum BuriedDeployment : int16_t {
    // buried deployments get negative values to avoid overlap with DeploymentPos
    DEPLOYMENT_HEIGHTINCB = std::numeric_limits<int16_t>::min(),
    DEPLOYMENT_CLTV,
    DEPLOYMENT_DERSIG,
    DEPLOYMENT_CSV,
    // SCRIPT_VERIFY_WITNESS is enforced from genesis, but the check for downloading
    // missing witness data is not. BIP 147 also relies on hardcoded activation height.
    DEPLOYMENT_SEGWIT,
};
constexpr bool ValidDeployment(BuriedDeployment dep) { return dep <= DEPLOYMENT_SEGWIT; }

enum DeploymentPos : uint16_t {
    DEPLOYMENT_TESTDUMMY,
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in deploymentinfo.cpp
    // Removing an entry may require bumping MinBIP9WarningHeight.
    MAX_VERSION_BITS_DEPLOYMENTS
};
constexpr bool ValidDeployment(DeploymentPos dep) { return dep < MAX_VERSION_BITS_DEPLOYMENTS; }

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit{28};
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime{NEVER_ACTIVE};
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout{NEVER_ACTIVE};
    /** If lock in occurs, delay activation until at least this block
     *  height.  Note that activation will only occur on a retarget
     *  boundary.
     */
    int min_activation_height{0};
    /** Period of blocks to check signalling in (usually retarget period, ie params.DifficultyAdjustmentInterval()) */
    uint32_t period{2016};
    /**
     * Minimum blocks including miner confirmation of the total of 2016 blocks in a retargeting period,
     * which is also used for BIP9 deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t threshold{1916};

    /** Constant for nTimeout very far in the future. */
    static constexpr int64_t NO_TIMEOUT = std::numeric_limits<int64_t>::max();

    /** Special value for nStartTime indicating that the deployment is always active.
     *  This is useful for testing, as it means tests don't need to deal with the activation
     *  process (which takes at least 3 BIP9 intervals). Only tests that specifically test the
     *  behaviour during activation cannot use this. */
    static constexpr int64_t ALWAYS_ACTIVE = -1;

    /** Special value for nStartTime indicating that the deployment is never active.
     *  This is useful for integrating the code changes for a new feature
     *  prior to deploying it on some or all networks. */
    static constexpr int64_t NEVER_ACTIVE = -2;
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    int nSubsidyHalvingInterval;
    /**
     * Hashes of blocks that
     * - are known to be consensus valid, and
     * - buried in the chain, and
     * - fail if the default script verify flags are applied.
     */
    std::map<uint256, script_verify_flags> script_flag_exceptions;
    /** Block height and hash at which BIP34 becomes active */
    int BIP34Height;
    uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    int BIP65Height;
    /** Block height at which BIP66 becomes active */
    int BIP66Height;
    /** Block height at which CSV (BIP68, BIP112 and BIP113) becomes active */
    int CSVHeight;
    /** Block height at which Segwit (BIP141, BIP143 and BIP147) becomes active.
     * Note that segwit v0 script rules are enforced on all blocks except the
     * BIP 16 exception blocks. */
    int SegwitHeight;
    /** Don't warn about unknown BIP 9 activations below this height.
     * This prevents us from warning about the CSV, segwit and taproot activations. */
    int MinBIP9WarningHeight;
    std::array<BIP9Deployment,MAX_VERSION_BITS_DEPLOYMENTS> vDeployments;
    /** Proof of work parameters */
    uint256 powLimit;
    /** Elektron Net: post-fix, safe value for `powLimit`, used at and after
     * PowLimitFixActivationHeight instead of the original `powLimit` above.
     * The original value violates the safety invariant
     * `powLimit * 4 * nPowTargetTimespan < 2^256` that CalculateNextWorkRequired()
     * (pow.cpp) relies on to avoid silent arith_uint256 overflow during
     * retargeting -- see doc-elektron/fix-report-powlimit-retarget-overflow.md.
     * Chosen as the original powLimit right-shifted by 12 bits (divided by
     * 4096): same leading-digit shape, smallest change that restores a real
     * (~4.3x) safety margin over the ~945x-violated invariant, keeping
     * nPowTargetSpacing/nPowTargetTimespan untouched. */
    uint256 powLimitPostFix;
    /** Block height at which the powLimit overflow fix (see powLimitPostFix
     * above) takes effect: CalculateNextWorkRequired() switches from
     * `powLimit` to `powLimitPostFix`. Height-gated, not a wall-clock flag
     * day, so all upgraded nodes switch at the exact same block regardless
     * of when each one updates. -1 = never active (default). Blocks below
     * this height retarget byte-for-byte identically to before this field
     * existed; only blocks at or above it use the corrected value. See
     * doc-elektron/fix-report-powlimit-retarget-overflow.md. */
    int PowLimitFixActivationHeight = -1;
    bool fPowAllowMinDifficultyBlocks;
    /**
      * Enforce BIP94 timewarp attack mitigation. On testnet4 this also enforces
      * the block storm mitigation.
      */
    bool enforce_BIP94;
    bool fPowNoRetargeting;
    int MinDifficultyActivationHeight = -1;
    /** Height at which the Stoic Awakening min-difficulty escape
     * (MinDifficultyActivationHeight) stops applying to new blocks, i.e. the
     * post-genesis-restart mainnet exception is retired and mainnet reverts
     * to always requiring pindexLast->nBits outside retarget boundaries, same
     * as vanilla Bitcoin mainnet. -1 = no end (escape stays active forever,
     * the historical default before this parameter existed). Blocks below
     * this height keep validating exactly as before; only blocks at or above
     * it lose access to the escape. See
     * doc-elektron/CHANGELOG-stoic-awakening-retirement.md. */
    int StoicAwakeningEndHeight = -1;
    /** Block height at which per-block UTXO attestation switches from a full
     * HASH_SERIALIZED rescan to the incrementally-maintained MuHash
     * accumulator. -1 = never active (default; used on mainnet for now). */
    int MuhashAttestationActivationHeight = -1;
    /** Block height at which ComputeBlockUTXOAttestationHash()'s incremental
     * (post-MuhashAttestationActivationHeight) path correctly resolves a
     * transaction that spends another transaction's output within the same
     * candidate block, instead of failing attestation computation for that
     * block. Below this height (or if disabled), a block containing such an
     * intra-block dependent pair keeps being treated as attestation-invalid
     * by every node, upgraded or not, so the rollout of this fix cannot
     * itself cause a chain split; only nodes already upgraded before this
     * height is reached start accepting (and can build) blocks shaped that
     * way once it hits, so this must not be lower than every honest miner
     * had a real chance to upgrade by. -1 = never active (default; must be
     * set on mainnet only once a real activation height is agreed on and
     * announced to node/miner operators). See
     * doc-elektron/fix-report-utxo-attestation-intra-block-chain.md. */
    int IntraBlockAttestationFixActivationHeight = -1;
    /** Elektron Net: interval (in blocks) between automatic UTXO checkpoint
     * snapshots (WriteAutomaticSnapshot) and the P2P snapshot-bootstrap
     * threshold (see src/validation.h's MANDATORY_PRUNE_DEPTH, the mainnet
     * default this defaults to). Lower on testnet/regtest so the checkpoint
     * cycle can actually be observed without mining 197,280 blocks. */
    unsigned int MandatoryPruneDepth = 197280;
    int64_t nPowTargetSpacing;
    int64_t nPowTargetTimespan;
    std::chrono::seconds PowTargetSpacing() const
    {
        return std::chrono::seconds{nPowTargetSpacing};
    }
    int64_t DifficultyAdjustmentInterval() const { return nPowTargetTimespan / nPowTargetSpacing; }
    /** The best chain should have at least this much work */
    uint256 nMinimumChainWork;
    /** By default assume that the signatures in ancestors of this block are valid */
    uint256 defaultAssumeValid;

    /**
     * If true, witness commitments contain a payload equal to a Bitcoin Script solution
     * to the signet challenge. See BIP325.
     */
    bool signet_blocks{false};
    std::vector<uint8_t> signet_challenge;

    int DeploymentHeight(BuriedDeployment dep) const
    {
        switch (dep) {
        case DEPLOYMENT_HEIGHTINCB:
            return BIP34Height;
        case DEPLOYMENT_CLTV:
            return BIP65Height;
        case DEPLOYMENT_DERSIG:
            return BIP66Height;
        case DEPLOYMENT_CSV:
            return CSVHeight;
        case DEPLOYMENT_SEGWIT:
            return SegwitHeight;
        } // no default case, so the compiler can warn about missing cases
        return std::numeric_limits<int>::max();
    }
};

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
