# How-To: Move Coins From an Old (Coin Type 0) Wallet to a New (Coin Type 1370) Wallet

**Audience:** Regular users of the `elektron-qt` graphical wallet. No command line needed.
**Related:** [`CHANGELOG-slip44-coin-type.md`](CHANGELOG-slip44-coin-type.md) (the technical background), [`guideline-wallet-integration.md`](guideline-wallet-integration.md) §3.1.

---

> ## ⚠️ Before you do anything: back up your old wallet file
>
> Your existing wallet (created before this update) still works perfectly and
> your coins are **completely safe** in it — nothing about this process is
> urgent or required. But before you touch anything, make a backup of your
> current wallet and **keep that backup forever**, even after you've moved
> your coins to a new wallet. Do not delete your old wallet file at any
> point in this guide, and do not delete it afterward either. See step 1.

---

## Why this exists (short version)

Elektron Net now has its own official derivation number (SLIP-44 coin type
**1370**) instead of reusing Bitcoin's. Wallets created **from now on**
automatically use it. Your **existing** wallet was created before this and
keeps working exactly as before — it is not broken, nothing expires, and you
are not required to do anything. This guide is only for people who *want* to
consolidate their coins into a new-style wallet, for example to keep
everything under one consistent identity going forward.

---

## Step 1 — Back up your current wallet (do this first, always)

1. Open your wallet in `elektron-qt`.
2. Menu **File → Backup Wallet…**
3. Save the backup file to a safe location — ideally somewhere outside your
   computer too (USB stick, external drive). Give it a name you'll
   recognize later, e.g. `elektron-wallet-backup-2026-07-15.dat`.
4. Additionally, locate your actual wallet data folder and keep a copy of it
   as well (belt and suspenders — the in-app backup above is normally
   enough, but a raw folder copy costs nothing and protects against more
   scenarios). The default location depends on your operating system:

   | OS | Default wallet data folder |
   |---|---|
   | Windows | `%LOCALAPPDATA%\Elektron\wallets\<walletname>\` (or `%APPDATA%\Elektron\` if you've had the app installed since before an older version) |
   | Linux | `~/.elektron/wallets/<walletname>/` |
   | macOS | `~/Library/Application Support/Elektron/wallets/<walletname>/` — if you installed before this fix and that folder doesn't exist, check `~/Library/Application Support/Bitcoin/wallets/<walletname>/` instead; the app keeps using that older path automatically for installs that already had one. |

   Copy the whole `<walletname>` folder (it contains the wallet database
   plus a `.walletlock` file) somewhere safe.

**Never delete this backup**, even after finishing this guide. If your
mining payouts or any other automated payment is still configured to pay
into your old wallet's address, or if anything looks wrong later, this
backup is how you get back to exactly where you are right now.

---

## Step 2 — Create your new wallet

1. Menu **File → Create Wallet…**
2. Give it a clear name, e.g. `wallet-1370`, so you can tell it apart from
   your old one.
3. Leave the other options at their defaults unless you have a specific
   reason to change them (e.g. a passphrase, which is recommended).
4. Click **Create**.

This new wallet automatically uses the new coin type (1370) — there is
nothing to configure.

---

## Step 3 — Get a receiving address from the new wallet

1. Make sure the new wallet (`wallet-1370`) is the one currently selected.
   If you have more than one wallet open, use the **Wallet:** dropdown in
   the toolbar to switch, or **File → Open Wallet**.
2. Go to the **Receive** tab.
3. Click **Create new receiving address**.
4. Copy the address shown (starts with `be1q…`). You'll paste this in the
   next step.

---

## Step 4 — Send your coins from the old wallet to the new address

1. Switch back to your **old** wallet using the **Wallet:** dropdown (or
   **File → Open Wallet**).
2. Go to the **Send** tab.
3. Paste the new wallet's address (from Step 3) into the **Pay To** field.
4. To move your *entire* balance in one go:
   - Click **Use available balance**.
   - Tick **Subtract fee from amount** (so the network fee comes out of the
     amount being sent, and you don't need spare funds left over to cover
     it).
5. Double-check the address is correct — sending to the wrong address
   cannot be undone.
6. Click **Send**, review the confirmation dialog carefully, and confirm.

Blocks arrive roughly every 60 seconds on Elektron Net, so confirmation is
typically fast.

---

## Step 5 — Verify the coins arrived

1. Switch to the new wallet (`wallet-1370`) again via the **Wallet:**
   dropdown.
2. Check the **Overview** or **Transactions** tab — your incoming payment
   should appear, first as unconfirmed and then confirmed shortly after.

You're done. Your new wallet now holds the funds under coin type 1370, and
going forward you can use it as your main wallet.

---

## Reminder

- Keep the backup from **Step 1** indefinitely. It never expires and costs
  nothing to keep.
- Your old wallet still works and can still receive funds even after this —
  if you expect any further incoming payments to your old address(es) (for
  example already-configured mining payouts), leave the old wallet
  installed and just repeat Step 4 again later, or update those payouts to
  point at your new address.
- If anything about this process is unclear or something looks wrong,
  **stop and don't send anything** — your coins are safe sitting in the old
  wallet for as long as you need.
