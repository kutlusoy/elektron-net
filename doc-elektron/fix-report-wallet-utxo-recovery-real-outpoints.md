# Fix Report: Wallet UTXO Recovery Lists Pruned Coins Under Fake Outpoints

- **Version:** 0.1 (draft)
- **Date:** October 20, 2026
- **Audience:** Elektron Net core developers, wallet vendors that restore seeds on pruned nodes, security reviewers
- **Authors:** Elektron Net Developers, Ali Kutlusoy ([@kutlusoy](https://x.com/kutlusoy)), Milio aka 1000mio ([@spiroflex](https://x.com/spiroflex)) 🇪🇸
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) `src/wallet/wallet.cpp` (`CWallet::ScanUTXOSet()`, `CWallet::CreditUTXOFromChain()`, `CWallet::RefreshTXOsFromTx()`, `CWallet::AddToWallet()`), `src/wallet/wallet.h`, `src/wallet/receive.cpp`, `src/wallet/spend.cpp`, `src/wallet/rpc/wallet.cpp` (`rescanutxoset`)
- **See also:** `doc-elektron/BITCOIN_CORE_DIFF.md` §2.6, `doc-elektron/CHANGELOG-Release-v4.0.6.md`, [`elektron-net-mobile`](https://github.com/kutlusoy/elektron-net-mobile) `doc-elektron/fix-report-utxo-recovery-after-pruning.md`

---

## 1. Summary

On a pruned node, `CWallet::ScanUTXOSet()` recovers wallet outputs whose funding transactions are no longer stored. Two defects made this recovery unreliable. Neither changes consensus, block validation or the network protocol. Both are fixed in the wallet code only.

| # | Defect | Effect | Status |
|---|---|---|---|
| D1 | The partial transaction holding only the recovered output is identical for outputs with equal script, value and index. The second `AddToWallet()` hits the assertion `TxStateSerializedBlockHash(wtx.m_state) == TxStateSerializedBlockHash(state)`. | Wallet load or rescan aborts. | Fixed |
| D2 | The partial transaction has a synthetic txid, so the coin was listed and selected under an outpoint that does not exist in the UTXO set. | Balance correct, payment built from the coin never reaches the mempool. | Fixed |

## 2. Fix

1. The partial transaction carries a marker version (`RECOVERED_TX_VERSION`) and one input that names the real outpoint. Equal-looking outputs stay distinct (D1) and the real outpoint is known.
2. `RefreshTXOsFromTx()` registers a recovered output in `m_txos` under the real outpoint and in `m_recovered` (real outpoint to partial txid). Coin listing, balance, coin selection, PSBT creation and signing therefore use the real `txid:vout` (D2).
3. `GetWalletTxByOutpoint()` resolves a real outpoint to its partial wallet transaction. Balance, trust checks, size estimation and change tracing use it instead of a lookup by txid.
4. The input of a partial transaction is not added to `mapTxSpends`, so it is never reported as spending the coin.
5. If a real transaction that creates the same outpoint is added later (for example by a rescan of retained blocks), the partial transaction is removed.
6. The partial transaction is stored with a confirmed state and block position 0. A position of -1 would be read back as a conflicted transaction after a wallet reload.
7. Padding outputs of the partial transaction have value 0 instead of -1, so `GetValueOut()` no longer throws.
8. `rescanutxoset` is available as RPC, so a restored wallet can be scanned on demand. It is idempotent.

Consensus rules, the mainnet activation height 500000 of the powLimit fix and the P2P protocol are untouched.

## 3. Verification

Regtest, mandatory pruning, 511 blocks, funding block below `pruneheight`:

- A blank wallet with the imported descriptors (timestamp in the future) shows balance 0, `rescanutxoset` then shows 10 ELEK and `listunspent` returns exactly the two real funding outpoints.
- A second `rescanutxoset` changes nothing.
- After `unloadwallet` and `loadwallet` the balance and outpoints are the same.
- `walletcreatefundedpsbt` and `walletprocesspsbt` produce a complete transaction accepted by `testmempoolaccept`.
- `sendtoaddress` lands in the mempool with the real outpoint as input, and after confirmation the balance equals the previous balance minus amount and fee.
- Contract check of the mobile wallet: 151 passed, 0 failed.

## 4. Compatibility

A wallet file that already holds entries from the old behavior is not rewritten automatically. Restoring the wallet from its seed on the fixed build, followed by `rescanutxoset`, produces the correct entries.

## 5. Checklist

- [x] D1 and D2 fixed in `src/wallet`
- [x] Regtest scenario of section 3 passes
- [ ] Build and run the wallet functional tests on a native toolchain
- [ ] Tag and release v4.0.6 with this fix before mainnet height 500000
