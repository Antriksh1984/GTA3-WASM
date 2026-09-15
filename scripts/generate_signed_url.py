#!/usr/bin/env python3
"""Generate a time-limited CloudFront signed URL for an object under
/protected-assets/* on the re3-wasm distribution.

This is the *only* way to reach that path -- the CloudFront cache behavior
for /protected-assets/* requires a valid signature from the trusted key
group (re3-wasm-protected-assets-keygroup), so a plain URL to anything under
that prefix returns 403. See docs/ (or ask) for how the key pair/key group/
cache behavior were set up.

The private key (cf-signing/private_key.pem) must never be committed to this
repository or uploaded anywhere -- it is what lets you mint valid signed
URLs at all. Keep it local, outside version control.

Usage:
    python scripts/generate_signed_url.py \\
        --key-pair-id KRV0IOTC2A8L9 \\
        --private-key /path/to/private_key.pem \\
        --url https://d1g8dbaxokxzyj.cloudfront.net/protected-assets/some-file \\
        --expires-in-hours 24
"""
import argparse
import base64
import json
import time

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding


def cloudfront_b64(data: bytes) -> str:
    # CloudFront's own base64 variant: standard base64, then swap the three
    # characters that aren't URL-safe for its own substitutes.
    return (
        base64.b64encode(data)
        .decode("ascii")
        .replace("+", "-")
        .replace("=", "_")
        .replace("/", "~")
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key-pair-id", required=True, help="CloudFront public key Id (e.g. KRV0IOTC2A8L9)")
    ap.add_argument("--private-key", required=True, help="Path to the local private_key.pem (never share this file)")
    ap.add_argument("--url", required=True, help="Full https:// URL to sign, e.g. .../protected-assets/foo.zip")
    ap.add_argument("--expires-in-hours", type=float, default=24, help="How long the URL stays valid (default 24h)")
    args = ap.parse_args()

    expire_ts = int(time.time() + args.expires_in_hours * 3600)

    policy = {
        "Statement": [
            {
                "Resource": args.url,
                "Condition": {"DateLessThan": {"AWS:EpochTime": expire_ts}},
            }
        ]
    }
    policy_bytes = json.dumps(policy, separators=(",", ":")).encode("utf-8")

    with open(args.private_key, "rb") as f:
        private_key = serialization.load_pem_private_key(f.read(), password=None)

    signature = private_key.sign(policy_bytes, padding.PKCS1v15(), hashes.SHA1())

    signed_url = (
        f"{args.url}"
        f"?Policy={cloudfront_b64(policy_bytes)}"
        f"&Signature={cloudfront_b64(signature)}"
        f"&Key-Pair-Id={args.key_pair_id}"
    )

    print(signed_url)
    print(f"\n(expires {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(expire_ts))})")


if __name__ == "__main__":
    main()
