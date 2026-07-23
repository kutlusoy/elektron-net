#!/usr/bin/env python3
# Copyright (c) 2026-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Dual-track SLIP-44 coin type migration for existing Elektron Net wallets.

Wallets created before Elektron Net registered its own SLIP-44 coin type
(1370, symbol ELEK) derive mainnet addresses under Bitcoin's coin type
(0'). New wallets created by the node now default to 1370' (see
src/wallet/walletutil.cpp), but that only takes effect at wallet-creation
time -- it does not change already-existing wallets.

This script adds coin type 1370' descriptors to an existing wallet
*alongside* its existing 0' descriptors, and makes the 1370' ones active
for future `getnewaddress` calls. The 0' descriptors are left untouched:
they stay fully tracked and spendable, nothing is deleted, and no funds
are moved. See doc-elektron/CHANGELOG-slip44-coin-type.md for the full
rationale.

This only migrates descriptors that are currently the wallet's *active*
descriptors under coin type 0'. It is idempotent: descriptors already on
1370' are left alone, and it can be re-run safely.

Usage:
    ./migrate-cointype.py --wallet mywallet
    ./migrate-cointype.py --url http://127.0.0.1:8332 --user rpcuser --password rpcpass --wallet mywallet
"""

import argparse
import base64
import json
import re
import sys
import urllib.request

CHECKSUM_RE = re.compile(r"#[a-z0-9]{8}$")

OLD_COIN_TYPE = 0
NEW_COIN_TYPE = 1370

# Matches the coin-type segment of a descriptor path built by
# GenerateWalletDescriptor(): .../<purpose>h/<coin_type>h/<account>h/...
COIN_TYPE_RE = re.compile(r"(/(?:44|49|84|86)h)/(\d+)h(/\d+h/[01]/\*)")


class RpcClient:
    def __init__(self, url, user, password):
        self.url = url
        credentials = base64.b64encode(f"{user}:{password}".encode()).decode()
        self.headers = {
            'Content-Type': 'application/json',
            'Authorization': f'Basic {credentials}',
        }

    def call(self, method, *params):
        payload = json.dumps({
            'jsonrpc': '2.0',
            'id': 1,
            'method': method,
            'params': list(params),
        }).encode()
        req = urllib.request.Request(self.url, data=payload, headers=self.headers)
        with urllib.request.urlopen(req, timeout=30) as resp:
            result = json.loads(resp.read().decode())
            if 'error' in result and result['error'] is not None:
                raise RuntimeError(result['error'])
            return result['result']


def rewrite_coin_type(desc_str, new_coin_type):
    """Return desc_str with its coin-type path segment replaced, or None if
    the descriptor doesn't match the expected default-wallet shape."""
    match = COIN_TYPE_RE.search(desc_str)
    if not match:
        return None
    prefix, old_coin_type, suffix = match.groups()
    new_desc = (
        desc_str[:match.start()]
        + prefix + f"/{new_coin_type}h" + suffix
        + desc_str[match.end():]
    )
    return new_desc, int(old_coin_type)


def main():
    parser = argparse.ArgumentParser(description="Migrate an Elektron Net wallet to SLIP-44 coin type 1370 (dual-track, non-destructive)")
    parser.add_argument('--url', default='http://127.0.0.1:8332', help='RPC URL (without /wallet/<name>)')
    parser.add_argument('--user', default='user', help='RPC username')
    parser.add_argument('--password', default='password', help='RPC password')
    parser.add_argument('--wallet', required=True, help='Wallet name to migrate')
    parser.add_argument('--dry-run', action='store_true', help='Only print what would change, import nothing')
    args = parser.parse_args()

    wallet_url = args.url.rstrip('/') + '/wallet/' + args.wallet
    rpc = RpcClient(wallet_url, args.user, args.password)

    print(f"Wallet:    {args.wallet}")
    print(f"RPC:       {wallet_url}")
    print(f"Migrating: coin type {OLD_COIN_TYPE}' -> {NEW_COIN_TYPE}' (dual-track, old descriptors kept active-free but untouched)\n")

    descriptors = rpc.call('listdescriptors', True)['descriptors']

    to_import = []
    skipped = []
    for d in descriptors:
        if not d.get('active'):
            continue
        desc_str = d['desc']
        if 'xprv' not in desc_str:
            skipped.append((desc_str, "watch-only (no private key material), cannot derive sibling descriptor"))
            continue

        result = rewrite_coin_type(desc_str, NEW_COIN_TYPE)
        if result is None:
            skipped.append((desc_str, "does not match the standard default-wallet descriptor shape"))
            continue
        new_desc_str, found_coin_type = result

        if found_coin_type == NEW_COIN_TYPE:
            skipped.append((desc_str, f"already on coin type {NEW_COIN_TYPE}'"))
            continue
        if found_coin_type != OLD_COIN_TYPE:
            skipped.append((desc_str, f"unexpected coin type {found_coin_type}', leaving untouched"))
            continue

        # The rewritten coin type invalidates the original trailing checksum;
        # strip it so getdescriptorinfo computes a fresh, correct one.
        new_desc_str = CHECKSUM_RE.sub('', new_desc_str)
        info = rpc.call('getdescriptorinfo', new_desc_str)
        to_import.append({
            'desc': info['descriptor'],
            'active': True,
            'internal': d.get('internal', False),
            'timestamp': 'now',
            'range': [0, 999],
        })

    for desc_str, reason in skipped:
        print(f"SKIP: {desc_str}\n      ({reason})")

    if not to_import:
        print("\nNothing to migrate.")
        return

    print(f"\n{len(to_import)} descriptor(s) to add as active, coin type {NEW_COIN_TYPE}':")
    for entry in to_import:
        print(f"  {entry['desc']}")

    if args.dry_run:
        print("\n--dry-run set, not importing anything.")
        return

    results = rpc.call('importdescriptors', to_import)
    ok = 0
    for entry, result in zip(to_import, results):
        if result.get('success'):
            ok += 1
        else:
            print(f"FAILED: {entry['desc']}\n        {result.get('error')}")

    print(f"\nImported {ok}/{len(to_import)} descriptor(s).")
    print("Old coin-type-0' descriptors are untouched and remain fully spendable.")
    print("IMPORTANT: back up this wallet again (wallet.dat copy, or note the coin type change if you back up by seed).")


if __name__ == '__main__':
    sys.exit(main())
