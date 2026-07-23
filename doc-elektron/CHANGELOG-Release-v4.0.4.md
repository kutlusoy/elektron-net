# Changelog: Release v4.0.4

**Status:** Shipped 2026-07-23, built from branch `slip-44`.
**Related:** `CHANGELOG-slip44-coin-type.md` (wallet coin type detail),
`fix-report-utxo-attestation-intra-block-chain.md` (UTXO attestation fixes
detail), `WHITEPAPER.md`, `BITCOIN_CORE_DIFF.md`.

This file is a release-level summary of everything shipped in v4.0.4, in
the spirit of `CHANGELOG-muhash-attestation.md`. See the related files
above for the full background and rationale behind each change.

---

## Wallet: SLIP-44 coin type registration

Elektron Net now has its own registered SLIP-44 coin type, **1370** (symbol
`ELEK`). New mainnet wallets derive descriptors under this coin type
instead of reusing Bitcoin's coin type `0'`. Testnet and regtest are
unaffected. This is a forward-only change to wallet key derivation, not a
consensus change; it does not touch the chain, block validation, or
address format.

- `contrib/wallettools/migrate-cointype.py`: migration helper so existing
  wallets can move funds to the new derivation path, plus a regression
  test suite for it.
- Documented in the whitepaper and the wallet integration guideline, with
  a GUI how-to for moving coins to a coin-type-1370 wallet and example
  derivation paths for multi-coin wallets.
- Manually verified against real mainnet: 8 test cases logged with live
  addresses and block references.

See `CHANGELOG-slip44-coin-type.md` for the full detail.

## Node: UTXO attestation fixes

Three related fixes to the per-block UTXO attestation mechanism
(`BITCOIN_CORE_DIFF.md` section 2.2):

- **Node crash on template creation.** Building a block template could hit
  a UTXO attestation computation failure and abort the entire node
  process instead of returning a normal RPC error. Fixed in
  `src/node/interfaces.cpp`: a failed template creation now returns
  cleanly instead of crashing.
- **Root cause: intra-block dependent transactions.** UTXO attestation
  computation failed for any block containing a transaction that spends
  another transaction's output within that same block, an ordinary
  unconfirmed-chain/CPFP shape, not an error case. Fixed in
  `src/validation.cpp`. This is a consensus rule change, so it is
  height-gated behind a new `IntraBlockAttestationFixActivationHeight`
  parameter and activates on **mainnet at block 170000**, giving miners
  lead time to upgrade before it takes effect. Below that height, every
  node (upgraded or not) keeps validating exactly as before.
- **Stale attestation in `generateblock`.** The `generateblock` RPC built
  its coinbase attestation before the caller-supplied transactions were
  appended, so the attestation went stale and any call with extra
  transactions failed validation. Fixed in `src/rpc/mining.cpp` and
  `src/validation.cpp` (new `RegenerateUTXOAttestation()` helper); not
  height-gated, since it only changes local RPC behavior, not what blocks
  are consensus-valid.

See `fix-report-utxo-attestation-intra-block-chain.md` for the full
root-cause analysis and test coverage.

## Networking

Added permanent seed and `addnode` entries (`seeder.eleknet.org`,
`node1.elektron-net.org`, `node2.elektron-net.org`) for more reliable
initial peer discovery.

## GUI / legal

Added a right-to-be-forgotten declaration to the About dialog and license
text, clarified that it also holds against modified clients.

## Build

Fixed the macOS default data directory still using the old Bitcoin
branding. Version bumped to 4.0.4.
