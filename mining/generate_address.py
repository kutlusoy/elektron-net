#!/usr/bin/env python3
"""
Generate Elektron Net addresses and write all credentials to a text file.

Pure Python implementation — no OpenSSL or external packages required.
Derives P2PKH + P2WPKH addresses with the Elektron Net prefix
(P2PKH=0x00, Bech32 HRP="be").

Usage:
    python generate_address.py
    python generate_address.py --count 5 --output my_miner_wallet.txt
    python generate_address.py --seed 95402f1dffe959ef95c0c403341e610e85d31bb7adf06ff7fe29ea6e142c30f7
"""

import argparse
import hashlib
import os
import sys
import time

# ---------------------------------------------------------------------------
# Elektron Net constants
# ---------------------------------------------------------------------------
P2PKH_PREFIX = bytes([0x00])
BECH32_HRP = "be"
WIF_PREFIX = bytes([0x80])  # uncompressed

ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"

# ---------------------------------------------------------------------------
# secp256k1 parameters (pure Python)
# ---------------------------------------------------------------------------
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
GX = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
GY = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8


def _modinv(a, m=P):
    return pow(a % m, m - 2, m)


def _point_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2 and y1 != y2:
        return None
    if p1 == p2:
        lam = (3 * x1 * x1 * _modinv(2 * y1)) % P
    else:
        lam = ((y2 - y1) * _modinv(x2 - x1)) % P
    x3 = (lam * lam - x1 - x2) % P
    y3 = (lam * (x1 - x3) - y1) % P
    return (x3, y3)


def _scalar_mul(k, point=(GX, GY)):
    result = None
    addend = point
    while k:
        if k & 1:
            result = _point_add(result, addend)
        addend = _point_add(addend, addend)
        k >>= 1
    return result


def _bytes_from_int(x, length=32):
    return x.to_bytes(length, "big")


def _int_from_bytes(b):
    return int.from_bytes(b, "big")


# ---------------------------------------------------------------------------
# Key generation (pure Python secp256k1)
# ---------------------------------------------------------------------------
def generate_keypair(seed_hex: str = None):
    """Generate a secp256k1 keypair. If seed_hex is given, use it directly."""
    if seed_hex:
        priv = bytes.fromhex(seed_hex)
        if len(priv) != 32:
            raise ValueError("Seed must be 64 hex characters (32 bytes)")
    else:
        priv = os.urandom(32)

    priv_int = _int_from_bytes(priv)
    if priv_int == 0 or priv_int >= N:
        # Retry on edge cases (vanishingly unlikely)
        return generate_keypair(seed_hex=seed_hex)

    pub_point = _scalar_mul(priv_int)
    pub_x, pub_y = pub_point
    pub = bytes([0x04]) + _bytes_from_int(pub_x) + _bytes_from_int(pub_y)

    # Derive addresses
    sha256_pub = hashlib.sha256(pub).digest()
    ripemd160_pub = hashlib.new("ripemd160", sha256_pub).digest()

    wif = b58check(WIF_PREFIX + priv)
    p2pkh = b58check(P2PKH_PREFIX + ripemd160_pub)

    p2wpkh_data = [0] + convertbits(ripemd160_pub, 8, 5)
    p2wpkh = bech32_encode(BECH32_HRP, p2wpkh_data)

    return {
        "priv_hex": priv.hex(),
        "wif": wif,
        "pub_hex": pub.hex(),
        "p2pkh": p2pkh,
        "p2wpkh": p2wpkh,
    }


# ---------------------------------------------------------------------------
# Base58 / Bech32 helpers
# ---------------------------------------------------------------------------
def b58encode(data: bytes) -> str:
    num = int.from_bytes(data, "big")
    result = ""
    while num > 0:
        num, rem = divmod(num, 58)
        result = ALPHABET[rem] + result
    for b in data:
        if b == 0:
            result = "1" + result
        else:
            break
    return result


def b58check(data: bytes) -> str:
    checksum = hashlib.sha256(hashlib.sha256(data).digest()).digest()[:4]
    return b58encode(data + checksum)


def bech32_polymod(values):
    GEN = [0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3]
    chk = 1
    for v in values:
        b = chk >> 25
        chk = ((chk & 0x1FFFFFF) << 5) ^ v
        for i in range(5):
            chk ^= GEN[i] if ((b >> i) & 1) else 0
    return chk


def bech32_hrp_expand(hrp):
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]


def bech32_create_checksum(hrp, data):
    values = bech32_hrp_expand(hrp) + data
    polymod = bech32_polymod(values + [0, 0, 0, 0, 0, 0]) ^ 1
    return [(polymod >> (5 * (5 - i))) & 31 for i in range(6)]


def bech32_encode(hrp, data):
    combined = data + bech32_create_checksum(hrp, data)
    return hrp + "1" + "".join(BECH32_CHARSET[d] for d in combined)


def convertbits(data, frombits, tobits, pad=True):
    acc = 0
    bits = 0
    ret = []
    max_acc = (1 << (frombits + tobits - 1)) - 1
    for value in data:
        acc = ((acc << frombits) | value) & max_acc
        bits += frombits
        while bits >= tobits:
            bits -= tobits
            ret.append((acc >> bits) & ((1 << tobits) - 1))
    if pad and bits:
        ret.append((acc << (tobits - bits)) & ((1 << tobits) - 1))
    return ret


# ---------------------------------------------------------------------------
# Output writer
# ---------------------------------------------------------------------------
def write_wallet_file(path: str, keys_list: list):
    header = f"""# =============================================================================
# ELEKTRON NET WALLET — GENERATED {time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())}
# =============================================================================
# Keep this file secure. Anyone with the private keys controls the funds.
# Network: Elektron Net (P2PKH prefix=0x00, Bech32 HRP=be)
# =============================================================================

"""
    body = ""
    for idx, k in enumerate(keys_list, 1):
        body += f"""--- Address {idx} ---
Private Key (hex):  {k['priv_hex']}
Private Key (WIF):    {k['wif']}
Public Key (hex):     {k['pub_hex']}
P2PKH Address:        {k['p2pkh']}
P2WPKH Address:       {k['p2wpkh']}

"""

    footer = """# =============================================================================
# USAGE NOTES FOR MINERS
# =============================================================================
# 1. Import the WIF or hex private key into your Elektron Net wallet:
#      ./src/elektron-cli importprivkey "<WIF>"
#
# 2. Or use the address directly as the mining reward address:
#      ./src/elektron-cli generatetoaddress 1 "<P2WPKH>"
#
# 3. The P2WPKH address (be1q...) is the native SegWit format and is
#    recommended for lower fees.
#
# 4. Never share the Private Key or WIF. Store this file offline if possible.
# =============================================================================
"""
    with open(path, "w") as f:
        f.write(header + body + footer)
    print(f"Wallet file written: {os.path.abspath(path)}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Generate Elektron Net addresses with full credentials."
    )
    parser.add_argument(
        "--count", type=int, default=1,
        help="Number of addresses to generate (default: 1)"
    )
    parser.add_argument(
        "--output", type=str, default=None,
        help="Output text file (default: wallet_<timestamp>.txt)"
    )
    parser.add_argument(
        "--seed", type=str, default=None,
        help="Optional 32-byte hex seed for deterministic key generation. "
             "If omitted, a random key is generated."
    )
    args = parser.parse_args()

    if args.count < 1:
        print("Count must be at least 1.")
        sys.exit(1)

    if args.output is None:
        ts = time.strftime("%Y%m%d_%H%M%S", time.gmtime())
        args.output = f"wallet_elektron-net_{ts}.txt"

    keys = []
    print(f"Generating {args.count} Elektron Net address(es)...")
    for i in range(args.count):
        if args.seed and args.count > 1:
            # Derive multiple keys from seed + index via simple hashing
            seed_material = args.seed + format(i, "08x")
            derived_seed = hashlib.sha256(seed_material.encode()).hexdigest()
            k = generate_keypair(seed_hex=derived_seed)
        else:
            k = generate_keypair(seed_hex=args.seed)
        keys.append(k)
        print(f"  [{i+1}/{args.count}] {k['p2wpkh']}")

    write_wallet_file(args.output, keys)
    print("\nDone. Keep the output file safe!")


if __name__ == "__main__":
    main()
