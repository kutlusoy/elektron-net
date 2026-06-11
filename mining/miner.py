#!/usr/bin/env python3
"""
Standalone CPU miner for Elektron Net.

Connects to a local node via RPC, fetches block templates, mines with
multiple threads, and submits solved blocks.

Elektron Net v4.0 (genesis restart):
- Stoic Awakening is active from block 1. The node may serve templates with
  minimum difficulty (powLimit) if more than 120 seconds have elapsed since
  the previous block.
- UTXO attestation outputs (every block) are included via the
  coinbase_required_outputs field in getblocktemplate. Full on-disk snapshots
  are written by nodes only at checkpoint heights (every 197,280 blocks).
"""

import argparse
import base64
import hashlib
import json
import os
import struct
import sys
import threading
import time
import urllib.request


# ---------------------------------------------------------------------------
# Bech32 / Base58 helpers (address -> scriptPubKey)
# ---------------------------------------------------------------------------
BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def _bech32_polymod(values):
    GEN = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3]
    chk = 1
    for v in values:
        b = chk >> 25
        chk = (chk & 0x1ffffff) << 5 ^ v
        for i in range(5):
            chk ^= GEN[i] if ((b >> i) & 1) else 0
    return chk


def _bech32_hrp_expand(hrp):
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]


def _bech32_verify_checksum(hrp, data):
    """Return 0 for Bech32, 1 for Bech32m, or -1 for invalid."""
    polymod = _bech32_polymod(_bech32_hrp_expand(hrp) + data)
    if polymod == 1:
        return 0  # Bech32 (BIP141)
    if polymod == 0x2bc830a3:
        return 1  # Bech32m (BIP350)
    return -1


def _bech32_convert_bits(data, from_bits, to_bits, pad=True):
    """General power-of-2 base conversion (matches generate_address.py)."""
    acc = 0
    bits = 0
    ret = []
    maxv = (1 << to_bits) - 1
    max_acc = (1 << (from_bits + to_bits - 1)) - 1
    for value in data:
        if value < 0 or (value >> from_bits):
            return None
        acc = ((acc << from_bits) | value) & max_acc
        bits += from_bits
        while bits >= to_bits:
            bits -= to_bits
            ret.append((acc >> bits) & maxv)
    if pad:
        if bits:
            ret.append((acc << (to_bits - bits)) & maxv)
    return ret


def _bech32_decode(bech):
    if any(ord(x) < 33 or ord(x) > 126 for x in bech):
        return None, None, False
    if bech.lower() != bech and bech.upper() != bech:
        return None, None, False
    bech = bech.lower()
    pos = bech.rfind('1')
    if pos < 1 or pos + 7 > len(bech):
        return None, None, False
    hrp = bech[:pos]
    data = [BECH32_CHARSET.find(x) for x in bech[pos + 1:]]
    if any(x == -1 for x in data):
        return None, None, False
    checksum_valid = _bech32_verify_checksum(hrp, data) >= 0
    return hrp, data[:-6], checksum_valid


def _base58_decode(s):
    alphabet = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
    num = 0
    for c in s:
        num = num * 58 + alphabet.index(c)
    result = []
    while num > 0:
        result.insert(0, num & 0xff)
        num >>= 8
    pad = 0
    for c in s:
        if c == '1':
            pad += 1
        else:
            break
    return bytes([0] * pad + result)


def address_to_scriptpubkey(addr):
    """Convert a bech32 or base58 address to its scriptPubKey bytes."""
    addr_lower = addr.lower()
    if addr_lower.startswith('be1') or addr_lower.startswith('tb1') or addr_lower.startswith('bcrt1'):
        hrp, data, checksum_valid = _bech32_decode(addr)
        if data is None:
            raise ValueError(f"Invalid bech32 address: {addr}")
        if not checksum_valid:
            print(f"WARNING: bech32 checksum mismatch for {addr} — using anyway")
        if len(data) < 1:
            raise ValueError(f"Invalid bech32 address (no data): {addr}")
        witness_version = data[0]
        program_chars = data[1:]
        program = _bech32_convert_bits(program_chars, 5, 8, True)
        if program is None:
            raise ValueError(f"Invalid bech32 address (convert failed): {addr}")
        # Correct the last byte if padding was used
        total_bits = len(program_chars) * 5
        last_byte_bits = total_bits % 8
        if last_byte_bits != 0:
            program[-1] >>= (8 - last_byte_bits)
        witness_program = bytes(program)
        if witness_version == 0:
            if len(witness_program) == 20:
                return bytes([0x00, 0x14]) + witness_program  # P2WPKH
            elif len(witness_program) == 32:
                return bytes([0x00, 0x20]) + witness_program  # P2WSH
        elif witness_version == 1 and len(witness_program) == 32:
            return bytes([0x51, 0x20]) + witness_program  # P2TR
    else:
        decoded = _base58_decode(addr)
        if len(decoded) != 25:
            raise ValueError(f"Invalid base58 address length: {len(decoded)}")
        prefix = decoded[0]
        if prefix in (0x00, 0x6f):  # P2PKH mainnet / testnet
            return bytes([0x76, 0xa9, 0x14]) + decoded[1:21] + bytes([0x88, 0xac])
        elif prefix in (0x05, 0xc4):  # P2SH mainnet / testnet
            return bytes([0xa9, 0x14]) + decoded[1:21] + bytes([0x87])
    raise ValueError(f"Unsupported address type: {addr}")


# ---------------------------------------------------------------------------
# Bitcoin transaction / block serialization helpers
# ---------------------------------------------------------------------------
def _write_compact_size(n):
    if n < 0xfd:
        return bytes([n])
    elif n <= 0xffff:
        return b'\xfd' + struct.pack('<H', n)
    elif n <= 0xffffffff:
        return b'\xfe' + struct.pack('<I', n)
    else:
        return b'\xff' + struct.pack('<Q', n)


def _script_num(n):
    """Serialize a number exactly like CScript::push_int64 in Bitcoin Core."""
    if n == -1:
        return bytes([0x4f])          # OP_1NEGATE
    if n == 0:
        return bytes([0x00])          # OP_0
    if 1 <= n <= 16:
        return bytes([0x50 + n])      # OP_1 .. OP_16
    neg = n < 0
    n = abs(n)
    result = []
    while n:
        result.append(n & 0xff)
        n >>= 8
    if result[-1] & 0x80:
        if neg:
            result.append(0x80)
        else:
            result.append(0x00)
    elif neg:
        result[-1] |= 0x80
    return bytes([len(result)] + result)


def _build_coinbase_tx(template, script_pubkey):
    """Build a valid coinbase transaction from a GBT template and payout script."""
    height = template['height']

    # Use the exact prefix from getblocktemplate so UTXO attestation matches.
    prefix_hex = template.get('coinbase_script_sig_prefix')
    if prefix_hex:
        script_sig = bytes.fromhex(prefix_hex)
    else:
        script_sig = _script_num(height)
        if len(script_sig) < 2:
            script_sig += bytes([0x00])  # OP_0 — bad-cb-length guard

    # --- inputs ---
    inputs = (bytes(32) +                          # prevout.hash (null)
              struct.pack('<I', 0xFFFFFFFF) +      # prevout.n
              _write_compact_size(len(script_sig)) +
              script_sig +
              struct.pack('<I', 0xFFFFFFFE))       # nSequence = MAX_SEQUENCE_NONFINAL (timelock enforced)

    # --- outputs ---
    coinbasevalue = template['coinbasevalue']
    outputs = (struct.pack('<Q', coinbasevalue) +
               _write_compact_size(len(script_pubkey)) +
               script_pubkey)

    required_outputs = template.get('coinbase_required_outputs', [])
    witness_commitment = template.get('default_witness_commitment')
    if required_outputs:
        output_count = 1
        for req in required_outputs:
            script = bytes.fromhex(req['scriptPubKey'])
            value = req.get('value', 0)
            outputs += struct.pack('<Q', value)
            outputs += _write_compact_size(len(script))
            outputs += script
            output_count += 1
        has_witness = witness_commitment is not None or any(
            len(bytes.fromhex(req['scriptPubKey'])) >= 6 and
            bytes.fromhex(req['scriptPubKey'])[0:6] == bytes([0x6a, 0x24, 0xaa, 0x21, 0xa9, 0xed])
            for req in required_outputs
        )
    else:
        has_witness = witness_commitment is not None
        output_count = 1
        if has_witness:
            wc_script = bytes.fromhex(witness_commitment)
            outputs += struct.pack('<Q', 0)
            outputs += _write_compact_size(len(wc_script))
            outputs += wc_script
            output_count = 2

    # --- assemble tx without witness first ---
    tx = struct.pack('<i', 2)          # version 2
    tx += _write_compact_size(1)       # 1 input
    tx += inputs
    tx += _write_compact_size(output_count)
    tx += outputs
    tx += struct.pack('<I', height - 1)  # nLockTime = height - 1 (Elektron Net specific)

    if has_witness:
        # Insert segwit marker + flag after version
        tx = tx[:4] + b'\x00\x01' + tx[4:]
        # Coinbase witness: exactly one 32-byte array of zeroes
        witness = (_write_compact_size(1) +   # 1 stack item per input
                   _write_compact_size(32) +   # item length = 32
                   bytes(32))
        # Insert witness before the final 4-byte locktime
        tx = tx[:-4] + witness + tx[-4:]

    # Also return the NON-WITNESS serialization for txid calculation
    tx_no_witness = struct.pack('<i', 2)
    tx_no_witness += _write_compact_size(1)
    tx_no_witness += inputs
    tx_no_witness += _write_compact_size(output_count)
    tx_no_witness += outputs
    tx_no_witness += struct.pack('<I', height - 1)

    return tx, tx_no_witness


def _double_sha256(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def _compute_merkle_root(tx_hashes):
    if not tx_hashes:
        return bytes(32)
    hashes = list(tx_hashes)
    while len(hashes) > 1:
        if len(hashes) % 2 == 1:
            hashes.append(hashes[-1])
        new_hashes = []
        for i in range(0, len(hashes), 2):
            new_hashes.append(_double_sha256(hashes[i] + hashes[i + 1]))
        hashes = new_hashes
    return hashes[0]


def _build_block_header(template, nonce, merkle_root):
    version = struct.pack('<i', template['version'])
    prev_block = bytes.fromhex(template['previousblockhash'])[::-1]
    ts = struct.pack('<I', template['curtime'])
    bits = struct.pack('<I', int(template['bits'], 16))
    nonce_bytes = struct.pack('<I', nonce)
    return version + prev_block + merkle_root + ts + bits + nonce_bytes


def _assemble_block(template, nonce, coinbase_tx, coinbase_txid, transactions):
    """Serialize a complete block from template + solved coinbase + transactions."""
    tx_hashes = [coinbase_txid]
    tx_hashes += [bytes.fromhex(tx['txid'])[::-1] for tx in transactions]
    merkle_root = _compute_merkle_root(tx_hashes)

    header = _build_block_header(template, nonce, merkle_root)

    tx_count = 1 + len(transactions)
    txs = coinbase_tx + b''.join(bytes.fromhex(tx['data']) for tx in transactions)

    return header + _write_compact_size(tx_count) + txs


# ---------------------------------------------------------------------------
# RPC client
# ---------------------------------------------------------------------------
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


# ---------------------------------------------------------------------------
# Mining logic
# ---------------------------------------------------------------------------
def _bits_to_target(n_bits):
    exponent = (n_bits >> 24) & 0xff
    coefficient = n_bits & 0x007fffff
    return coefficient * (2 ** (8 * (exponent - 3)))


def _mine_worker(tid, header, target, start_nonce, stop_event, result):
    nonce = start_nonce
    while not stop_event.is_set() and nonce < 0xffffffff:
        nonce_bytes = struct.pack('<I', nonce)
        h = _double_sha256(header[:76] + nonce_bytes)
        if int.from_bytes(h, 'little') <= target:
            result.append((nonce, h[::-1].hex()))
            stop_event.set()
            return
        nonce += 1
        if nonce % 1_000_000 == 0 and tid == 0:
            print(f"  [thread {tid}] tried {nonce:,} nonces...")


def mine_block(template, num_threads, coinbase_txid, transactions):
    merkle_root = _compute_merkle_root(
        [coinbase_txid] + [bytes.fromhex(tx['txid'])[::-1] for tx in transactions]
    )
    header = _build_block_header(template, 0, merkle_root)
    target = _bits_to_target(int(template['bits'], 16))
    print(f"Target: {target:064x}")

    stop_event = threading.Event()
    result = []
    threads = []

    for i in range(num_threads):
        start_nonce = i
        t = threading.Thread(
            target=_mine_worker,
            args=(i, header, target, start_nonce, stop_event, result)
        )
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    if result:
        nonce, block_hash = result[0]
        print(f"FOUND! nonce={nonce} hash={block_hash}")
        return nonce
    return None


def submit_block(rpc, template, nonce, coinbase_tx, coinbase_txid, transactions):
    block = _assemble_block(template, nonce, coinbase_tx, coinbase_txid, transactions)
    block_hex = block.hex()

    # Local sanity check: verify the block hash
    header = block[:80]
    block_hash = _double_sha256(header)[::-1].hex()
    print(f"Block hash (local):  {block_hash}")

    try:
        result = rpc.call('submitblock', block_hex)
        if result is None or result == "":
            print("Block accepted.")
        else:
            print(f"Block submit result: {result}")
            if result in ("duplicate", "duplicate-invalid", "duplicate-inconclusive", "inconclusive"):
                print("WARNING: Block was NOT accepted into the chain!")
    except Exception as e:
        print(f"Submit failed: {e}")


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------
def main():
    # --- defaults ---
    config = {
        "rpc": {"url": "http://127.0.0.1:8332", "user": "user", "password": "password"},
        "mining": {"address": "", "threads": 4}
    }

    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(script_dir, "config.json")
    if os.path.exists(config_path):
        with open(config_path, 'r', encoding='utf-8') as f:
            file_config = json.load(f)
            if 'rpc' in file_config:
                config['rpc'].update(file_config['rpc'])
            if 'mining' in file_config:
                config['mining'].update(file_config['mining'])

    parser = argparse.ArgumentParser(description="Standalone Elektron Net CPU miner")
    parser.add_argument('--url', default=config['rpc']['url'], help='RPC URL')
    parser.add_argument('--user', default=config['rpc']['user'], help='RPC username')
    parser.add_argument('--password', default=config['rpc']['password'], help='RPC password')
    parser.add_argument('--address', default=config['mining'].get('address', ''),
                        help='Payout address (bech32/base58). Overrides config.json.')
    parser.add_argument('--threads', type=int, default=config['mining'].get('threads', 4),
                        help='Number of mining threads')
    parser.add_argument('--continuous', action='store_true',
                        default=config['mining'].get('continuous', False),
                        help='Mine continuously')
    args = parser.parse_args()

    if not args.address:
        print("ERROR: No payout address specified. Use --address or set mining.address in config.json")
        sys.exit(1)

    try:
        script_pubkey = address_to_scriptpubkey(args.address)
    except ValueError as e:
        print(f"ERROR: Invalid address: {e}")
        sys.exit(1)

    rpc = RpcClient(args.url, args.user, args.password)

    print(f"Elektron Net Miner (Python)")
    print(f"Network:   Elektron Net (60s blocks)")
    print(f"RPC:       {args.url}")
    print(f"Address:   {args.address}")
    print(f"Threads:   {args.threads}")

    while True:
        try:
            print("\nFetching block template...")
            template = rpc.call('getblocktemplate', {
                'rules': ['segwit'],
                'coinbaseaddress': args.address,
            })
        except Exception as e:
            print(f"Failed to get block template: {e}")
            time.sleep(5)
            continue

        transactions = template.get('transactions', [])
        coinbase_tx, coinbase_tx_no_witness = _build_coinbase_tx(template, script_pubkey)
        coinbase_txid = _double_sha256(coinbase_tx_no_witness)

        nonce = mine_block(template, args.threads, coinbase_txid, transactions)
        if nonce is not None:
            submit_block(rpc, template, nonce, coinbase_tx, coinbase_txid, transactions)
        else:
            print("No nonce found (template expired?).")

        if not args.continuous:
            break
        time.sleep(1)


if __name__ == '__main__':
    main()
