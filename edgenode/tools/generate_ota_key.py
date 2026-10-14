"""Create the offline P-256 signing key and the device's public trust anchor once."""
import argparse
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--private-key', required=True, type=Path)
    parser.add_argument('--public-dir', required=True, type=Path)
    args = parser.parse_args()
    private = args.private_key.resolve()
    public_dir = args.public_dir.resolve()
    repository = Path(__file__).resolve().parents[2]
    if private.is_relative_to(repository):
        parser.error('private key must be outside the repository')
    public_c = public_dir / 'ota_public_key.c'
    public_pem = public_dir / 'ota_public_key.pem'
    if any(path.exists() for path in (private, public_c, public_pem)):
        parser.error('key files already exist; refusing to replace a trust anchor')
    private.parent.mkdir(parents=True, exist_ok=True)
    public_dir.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    with private.open('xb') as output:
        output.write(key.private_bytes(serialization.Encoding.PEM,
                                      serialization.PrivateFormat.PKCS8,
                                      serialization.NoEncryption()))
    private.chmod(0o600)
    public_pem.write_bytes(key.public_key().public_bytes(serialization.Encoding.PEM,
                                                       serialization.PublicFormat.SubjectPublicKeyInfo))
    numbers = key.public_key().public_numbers()
    raw = numbers.x.to_bytes(32, 'big') + numbers.y.to_bytes(32, 'big')
    rows = ['    ' + ', '.join('0x%02X' % b for b in raw[i:i+8]) + ',' for i in range(0,64,8)]
    public_c.write_text('#include "ota_public_key.h"\n\n/* Generated public trust anchor; private key stays offline. */\n'
                        'const uint8_t ota_signature_public_key[64] = {\n' + '\n'.join(rows) + '\n};\n', encoding='utf-8')
    print('Created offline private key:', private)
    print('Created device public key:', public_pem)


if __name__ == '__main__':
    main()
