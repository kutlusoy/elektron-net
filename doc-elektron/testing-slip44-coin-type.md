# Manual Test Log: SLIP-44 Coin Type 1370 (v4.0.4, real mainnet)

**Purpose:** Track the manual, real-mainnet verification of the SLIP-44 coin type
1370 (ELEK) change (`slip-44` branch, v4.0.4) before/alongside merging it. Once
these are confirmed, their results feed a "Tested scenarios" section in
[`howto-coinsweep-legacy-wallet.md`](howto-coinsweep-legacy-wallet.md), so
users know upfront which paths have actually been exercised on the live
network rather than only reasoned about from the source.

**Setup:** Two client instances on the same machine, separate datadirs/ports,
both connected to real mainnet (no `-connect=0`):
- **Old client** — pre-1370 build (e.g. v4.0.3), holds a pre-existing coin
  type 0' wallet.
- **New client** — built from `slip-44` (v4.0.4), holds two wallets: a
  freshly created one (coin type 1370' by default) and a restored copy of an
  old coin type 0' wallet.

Each row: what's being tested, why it matters, what a pass looks like.

**Result: all 8 cases confirmed passing, 2026-07-15, by the user against a
real v4.0.4 build on real mainnet.** Folded into a "Tested scenarios"
section in `howto-coinsweep-legacy-wallet.md` — see below.

| ID | Test case | Expected result | Status |
|---|---|---|---|
| TC1 | Fresh build sanity: create a new wallet in the new client, check `listdescriptors` in the Debug Console. | Active descriptors show `/1370h/` in their path, not `/0h/`. | **Confirmed** |
| TC2 | Send minimal ELEK: old client's 0' wallet → new client's new 1370' wallet address, on real mainnet. | Transaction confirms normally (~60s block time); new wallet's balance updates. | **Confirmed** |
| TC3 | Send back: new client's new 1370' wallet → an address in the old client's wallet. | Spend succeeds — confirms 1370' derivation works for *signing*, not just receiving. | **Confirmed** |
| TC4 | Check both TC2 and TC3 transactions on the `elektron-net-mempool` explorer. | Both show up like any ordinary transaction; nothing coin-type-specific visible or different. | **Confirmed** |
| TC5 | Restore an existing 0' wallet backup into the new (v4.0.4) client via **File → Restore Wallet…**. | Wallet loads; balance/UTXOs recognized automatically; `getnewaddress`/`listdescriptors` on this wallet still show `0'`, unaffected by the new default. | **Confirmed** |
| TC6 | Same-client round trip: with both wallets loaded in the new client, move funds back and forth via the **Wallet:** selector (old-restored ↔ new). | Both directions succeed; the receiving wallet shows the transaction at 0 confirmations immediately (same node, shared mempool), then confirms after ~60s. | **Confirmed** |
| TC7 | Cross-version P2P test: old client and new client as peers on the same mainnet; send in all combinations — old-client-wallet ↔ new-client-old-wallet, old-client-wallet ↔ new-client-new-wallet. | No P2P incompatibility of any kind; all combinations settle normally, since this is a wallet-only change with no consensus/protocol-version impact. | **Confirmed** |
| TC8 | UTXO-scan-without-history: back up a wallet at 0 balance, fund an address from it afterward (in the old client), then import that *pre-funding* backup into the new client, which has no record of the transaction. | New client recognizes and credits the funds automatically once synced, purely via `ScanUTXOSet` against the current UTXO set — no rescan or manual re-import of the address needed. | **Confirmed** |

## Live mainnet evidence

Recorded for the record — publicly verifiable on any block explorer synced
to this chain.

**Addresses involved:**

| Role | Address |
|---|---|
| Coin type 0', old client (v4.0.3) | `be1qc95ra8pcfpqgegjt7mt6tt70ud48ytmpw6nwlp` |
| Coin type 0', same wallet restored into new client (v4.0.4) | `be1qufgz4h2f7ar3ueg3f65vn0umjznv6tzzqyqs9g` |
| Coin type 1370', new wallet on new client (v4.0.4) | `be1qk24yv5cyhywx30perc7j7nelv0nx7zxjnkx3ut` |

**Block heights containing the test transactions:** 109874, 109875, 109878,
109895, 109896.
