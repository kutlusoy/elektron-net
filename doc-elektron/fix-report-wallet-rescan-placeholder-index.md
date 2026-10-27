# Fix Report: Block Rescan Crashes After a UTXO Scan Stored a Placeholder Position

- **Version:** 0.1 (draft)
- **Date:** October 27, 2026
- **Audience:** Elektron Net core developers, wallet vendors that restore seeds on pruned nodes
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) `src/wallet/wallet.cpp` (`CWallet::CreditUTXOFromChain()`, `CWallet::AddToWallet()`)
- **See also:** `doc-elektron/fix-report-wallet-utxo-recovery-real-outpoints.md`, `doc-elektron/CHANGELOG-Release-v4.0.6.md`, [`elektron-net-mobile`](https://github.com/kutlusoy/elektron-net-mobile) `app/src/test/java/org/elektronnet/mobile/wallet/UtxoRescanIntegrationTest.kt`

---

## 1. Summary

On a pruned node a wallet that restores old coins runs a UTXO scan and a block rescan (`rescanblockchain`). When the wallet already held a transaction as confirmed and a UTXO scan came across one of its outputs, `CreditUTXOFromChain()` replaced the stored position of that transaction in its block with a placeholder (`-1` in v4.0.5, `0` in v4.0.6), because the UTXO set does not know the position. The next block rescan found the same transaction in its block with the real position and called `AddToWallet()`, which asserts that the stored and the new position are equal:

```
wallet/wallet.cpp:1152: ... assertion "TxStateSerializedIndex(wtx.m_state) == TxStateSerializedIndex(state)" failed
```

The node stopped. Transactions at position 0 (coinbase) matched the placeholder by chance, so only normal payments in a block (position 1 or higher) triggered it. The fix is in the wallet code only and changes neither consensus nor the network protocol.

## 2. Reproduction (regtest)

1. Mine 110 blocks, then pay a second wallet in a block that also holds another transaction, so the payment is at position 2.
2. Import the descriptors of the second wallet into a new wallet (timestamp 0). The wallet now holds the payment with its real position.
3. `rescanutxoset` on that wallet stores the placeholder.
4. `rescanblockchain` over that block crashes the node.

## 3. Fix

- `CreditUTXOFromChain()` keeps the state of a transaction that the wallet already holds as confirmed in the same block. The placeholder is only stored for a transaction that is not yet confirmed in that block.
- `AddToWallet()` no longer asserts on a differing position. The block hash must still be equal (the assertion stays), and the position coming from the block replaces the stored one. This also heals wallets that already carry a placeholder in `wallet.dat`.

## 4. Checklist

- [x] Regression test written and failing before the fix: `UtxoRescanIntegrationTest` in `elektron-net-mobile` (needs `ELEKTROND_BIN`).
- [x] Same test passes with the fix, together with the UTXO recovery and wallet restore integration tests.
- [ ] Wallet unit tests and functional tests of the node were not run for this change.
