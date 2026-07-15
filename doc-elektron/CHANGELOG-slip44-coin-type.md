# Changelog: SLIP-44 Coin Type Registration (`ELEK` = 1370)

**Status:** Implemented 2026-07-15, planned for the next release after v4.0.3.
Mainnet wallets created from this point onward derive descriptors under
Elektron Net's own registered SLIP-44 coin type, **1370** (symbol `ELEK`),
instead of reusing Bitcoin's coin type `0'`. Testnet/regtest are unaffected.
This is a forward-only change to wallet key derivation, not a consensus
change — it does not touch the chain, block validation, or address format.
**Related:** `doc-elektron/guideline-wallet-integration.md` §3.1, §6, §7
(the ambiguity this resolves), `WHITEPAPER.md` §2, §4.5, §6,
`contrib/wallettools/migrate-cointype.py` (the migration tool this file
documents the use of).

This file records *why* and *how* the coin type was registered and rolled
out, in the spirit of `CHANGELOG-stoic-awakening-retirement.md`.

---

## 2026-07-15

### The problem

The node's descriptor wallet (`GenerateWalletDescriptor()` in
`src/wallet/walletutil.cpp`) hardcoded mainnet derivation at BIP44 coin
type `0'` — the same index Bitcoin itself uses. This was flagged as an
open, undocumented ambiguity in `doc-elektron/guideline-wallet-integration.md`
(§3.1, §6 Phase 0, §7 Open Question #2): no wallet vendor could safely fix
a derivation path without first knowing whether Elektron Net intended to
register its own coin type or deliberately reuse Bitcoin's.

This carried no risk to funds while only the node's own built-in wallet
existed. The risk is entirely forward-looking: any future wallet
implementation offering seed-based recovery, built against the "official"
SLIP-44 registry entry for Elektron Net, would only scan the registered
coin type — silently missing balances derived under `0'` if that number
were never actually reserved for us. Coin type `0'` also has no
registry-level connection to Elektron Net at all, so nothing prevents a
different, unrelated project from also using it, or a Elektron-Net-aware
wallet from being unable to disambiguate.

### Decision

Register a dedicated SLIP-44 coin type rather than continue reusing
Bitcoin's `0'`, made directly with the user, 2026-07-15. Verified against
the upstream registry (`satoshilabs/slips`, `slip-0044.md`) that the range
1349–1396 was entirely unassigned; **1370** was chosen and submitted via
pull request against `satoshilabs/slips` (symbol `ELEK`, hardened path
component `0x80000000 + 1370 = 0x8000055A`). The coin type is usable
immediately in our own software regardless of upstream PR merge timing,
since it only governs our own wallet's derivation, not any shared
registry-dependent behavior.

No sweep, no forced fund movement, and no rescan of existing wallets was
required or performed as part of this decision — see "What does *not*
change" below.

### Implementation

**`src/wallet/walletutil.cpp`, `GenerateWalletDescriptor()`:** the mainnet
branch of the coin-type selection changed from `desc_prefix += "/0h"` to
`desc_prefix += "/1370h"`. The testnet/regtest branch (`/1h`, SLIP-44's
generic "testnet for all coins" convention) is untouched. This function
runs once per `(OutputType, internal)` pair, only at wallet-creation time
(`SetupDescriptorGeneration` in `scriptpubkeyman.cpp` skips it once a
descriptor already exists for that slot) — so this change affects **only
wallets created from now on**.

**Unit tests** (`src/wallet/test/scriptpubkeyman_tests.cpp`):
`GenerateWalletDescriptorCoinTypeMainnet` (mainnet descriptors now contain
`/1370h/`) and `GenerateWalletDescriptorCoinTypeRegtest` (regtest
descriptors still contain `/1h/`, not `/1370h/`).

**Migration tool for already-existing wallets**
(`contrib/wallettools/migrate-cointype.py`): a dual-track migration, not a
sweep. For a given wallet, it reads the wallet's current active
descriptors via `listdescriptors true`, derives sibling descriptors at
coin type 1370' from the same embedded master key (only the coin-type path
segment changes), and adds them via `importdescriptors` with
`"active": true`. The previously active `0'` descriptors are left in the
wallet exactly as they were — not deleted, not deactivated in a way that
stops tracking them, just no longer the default for `getnewaddress`. The
script is idempotent (already-1370' slots are skipped) and does not touch
descriptors it doesn't recognize as the standard default-wallet shape
(e.g. manually imported multisig descriptors).

### What does *not* change

- **Address format is unaffected.** The Bech32 HRP (`be1q…`/`be1p…`,
  `bech32_hrp` in `src/kernel/chainparams.cpp`) is a separate, independent
  network parameter and has nothing to do with BIP44 coin type. Existing
  addresses look exactly the same before and after this change.
- **No existing wallet or balance is touched automatically.** Wallets
  created before this change keep working exactly as before; their `0'`
  descriptors remain valid and spendable indefinitely, with or without
  ever running the migration script.
- **No funds move.** The migration script only adds descriptors; it never
  constructs, signs, or broadcasts a transaction.
- **Testnet, regtest** keep coin type `1'`, untouched by this change.
- **The node's RPC/wallet interface** gained no new RPC — the migration
  tool is built entirely from existing `listdescriptors`/`getdescriptorinfo`/
  `importdescriptors` RPCs.

### Operator impact

New wallets created after this change need no action — they use coin type
1370' from the start. For the project's own already-existing wallets
(created during the earlier testing phase under coin type `0'`), run:

```
contrib/wallettools/migrate-cointype.py --wallet <name>
```

against each one to add the 1370' descriptors as active, going forward.
Back up the wallet again afterward (a `wallet.dat` copy, or simply note
the coin type change if backing up by seed — the same seed already covers
both derivation paths).

Any wallet vendor building seed-based recovery for Elektron Net **must**
scan both `1370'` and the legacy `0'` path to find all funds for wallets
that predate this change — documented as a MUST-level requirement in
`doc-elektron/guideline-wallet-integration.md` §3.1.
