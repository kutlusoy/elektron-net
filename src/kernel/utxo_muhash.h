// Copyright (c) 2026-present The Elektron Net developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_UTXO_MUHASH_H
#define BITCOIN_KERNEL_UTXO_MUHASH_H

#include <coins.h>
#include <crypto/muhash.h>
#include <kernel/coinstats.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

namespace kernel {

/**
 * Elektron Net: incrementally-maintained MuHash commitment to the entire UTXO set.
 *
 * This mirrors the per-coin bookkeeping CoinStatsIndex already does (see
 * ApplyCoinHash/RemoveCoinHash in coinstats.cpp), but is updated synchronously from
 * ConnectBlock/DisconnectBlock and persisted alongside the chainstate itself, so it
 * can be consulted directly as a consensus value (see
 * Consensus::Params::MuhashAttestationActivationHeight) instead of being an optional,
 * best-effort local index.
 */
class UTXOMuHashState
{
private:
    MuHash3072 m_muhash;

public:
    UTXOMuHashState() noexcept = default;

    void AddCoin(const COutPoint& outpoint, const Coin& coin) { ApplyCoinHash(m_muhash, outpoint, coin); }
    void RemoveCoin(const COutPoint& outpoint, const Coin& coin) { RemoveCoinHash(m_muhash, outpoint, coin); }

    //! Finalize into a 32-byte hash. Operates on a copy, so the running
    //! accumulator itself keeps accepting further Add/RemoveCoin calls.
    uint256 GetHash() const
    {
        MuHash3072 tmp{m_muhash};
        uint256 out;
        tmp.Finalize(out);
        return out;
    }

    SERIALIZE_METHODS(UTXOMuHashState, obj) { READWRITE(obj.m_muhash); }
};

} // namespace kernel

#endif // BITCOIN_KERNEL_UTXO_MUHASH_H
