# Fix Report: Wallets Recovered by v4.0.5 Cannot Be Loaded Again

- **Version:** 0.1 (draft)
- **Date:** October 27, 2026
- **Audience:** Elektron Net core developers, wallet vendors, users who keep wallet backups (`wallet.dat`) without a seed phrase
- **Reference implementation:** [`elektron-net`](https://github.com/kutlusoy/elektron-net) `src/wallet/walletdb.cpp` (`LoadTxRecords()`, `WalletBatch::LoadWallet()`), `src/wallet/wallet.cpp` (`CWallet::AttachChain()`, `CWallet::CreditUTXOFromChain()`), `src/wallet/wallet.h` (`m_dropped_unreadable_txs`)
- **See also:** `doc-elektron/fix-report-wallet-utxo-recovery-real-outpoints.md`, `doc-elektron/fix-report-wallet-rescan-placeholder-index.md`, `doc-elektron/CHANGELOG-Release-v4.0.6.md`, [`elektron-net-mobile`](https://github.com/kutlusoy/elektron-net-mobile) `app/src/test/java/org/elektronnet/mobile/wallet/OldWalletHealingIntegrationTest.kt`

---

## 1. Summary

On a pruned node, v4.0.5 recovers the outputs of a wallet from the UTXO set and stores each of them in a transaction that has no inputs. Such a transaction cannot be read back: the input count 0 is read as the marker of the witness format, and the read fails with `DataStream::read(): end of data`. The wallet then fails to load with `Wallet corrupted`, in v4.0.5 as well as in v4.0.6. This hits every wallet file (and every backup of it) that was written after such a recovery, which is the only way to open a backup that has no seed phrase.

v4.0.6 already writes the recovered outputs into a partial transaction with one real input (see `fix-report-wallet-utxo-recovery-real-outpoints.md`), which can be read. It did not open files written by v4.0.5.

## 2. Fix

- `LoadTxRecords()` catches the read error of a transaction record whose input count is 0, drops the record, and remembers its transaction id. Any other unreadable record still fails the load as before, so real corruption is not hidden.
- `WalletBatch::LoadWallet()` erases the dropped records from the wallet file, so the next load does not meet them again. Records are only erased when the rest of the file loaded without errors.
- `CWallet::AttachChain()` credits the outputs again from the UTXO set when records were dropped (`ScanUTXOSet()`). During initial block download the existing deferred scan after the snapshot activation covers it.
- The wallet file is changed in place once: the unreadable records are gone and the outputs come back as readable partial transactions. Later loads read the file as any other. A backup file is not changed, a restore from it repairs the restored copy.

Only the outputs are restored. The pruned transaction history cannot be restored by any node.

## 3. Tests

`OldWalletHealingIntegrationTest` (`elektron-net-mobile`, needs `ELEKTROND_BIN_OLD` and `ELEKTROND_BIN`) writes the wallets with a v4.0.5 node and opens them with the new node:

- [x] the wallet file in place, then again after unload and load (nothing is dropped twice);
- [x] a backup taken right after the recovery, restored with `restorewallet`;
- [x] a stale backup (taken before a new coin arrived and an old coin was spent), restored with `restorewallet`;
- [x] balance and outpoints equal those of the original wallet in all cases, the healed wallet signs and sends, and it loads again afterwards;
- [x] a record with a non-zero input count (real corruption) is still refused with `Wallet corrupted`, and a record in the placeholder shape is dropped (checked by editing a wallet file).
- [ ] The wallet unit tests and the functional tests of the node were not run for this change (the unit tests are not built, and the upstream functional tests assume a 50 coin block subsidy).
