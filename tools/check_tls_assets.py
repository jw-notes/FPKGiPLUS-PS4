#!/usr/bin/env python3
"""Check pinned trust assets, their signature relationship and package copies."""
import hashlib
from pathlib import Path
import ssl
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
assets = ROOT / "assets"
pins = {
    "cacert.pem": "427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285",
    "godaddy-secure-g2.pem": "708c1477f5e2b2ae402ba1020a696cdd0051d44859a7e1423c900826d09053f4",
    "letsencrypt-gen-y.pem": "4d1ea37c7926b3a1418568319b59d692804181a78a7adf78a63dee9128b99669",
    "digicert-playstation.pem": "0ddb18822d8a9bf1816bde4694b1af582b65b97fe5f81498db40ae7693de33fe",
}
for name, expected in pins.items():
    data = (assets / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit(f"Trust asset checksum mismatch: {name}")
    if len(sys.argv) > 1 and (ROOT / sys.argv[1] / "assets" / name).read_bytes() != data:
        raise SystemExit(f"Packaged trust asset mismatch: {name}")
ca = assets / "godaddy-secure-g2.pem"
der = ssl.PEM_cert_to_DER_cert(ca.read_text())
if hashlib.sha256(der).hexdigest() != "973a41276ffd01e027a2aad49e34c37846d3e976ff6a620b6712e33832041aa6":
    raise SystemExit("GoDaddy publisher DER fingerprint mismatch")
# This verifies the issuer's signature and dates against the unchanged Mozilla
# roots; the intermediate is deliberately NOT supplied as a trust anchor here.
subprocess.run(["openssl", "verify", "-CAfile", str(assets / "cacert.pem"),
                "-no-CApath", "-no-CAstore", str(ca)], check=True)
description = subprocess.check_output(["openssl", "x509", "-in", str(ca), "-text", "-noout"], text=True)
for expected in ("CA:TRUE", "Certificate Sign, CRL Sign", "May  3 07:00:00 2031 GMT"):
    if expected not in description:
        raise SystemExit(f"Unexpected GoDaddy CA constraints/expiry: {expected}")
subprocess.run(["openssl", "x509", "-in", str(ca), "-checkend", str(30 * 24 * 60 * 60), "-noout"], check=True)
# Let's Encrypt Gen-Y: pinned to the publisher's DER fingerprints (each was
# verified against ISRG Root X1 when added; see letsencrypt-gen-y-SOURCE.txt),
# and the intermediates must not be near expiry.
gen_y = assets / "letsencrypt-gen-y.pem"
records = [ "-----BEGIN CERTIFICATE-----" + part for part in gen_y.read_text().split("-----BEGIN CERTIFICATE-----")[1:]]
expected_der = [
    "13949634d99cd6fd6aa80bc034fefacceb1969feef986586713ecdbb05758d3f",  # YR1
    "238b85a0099c65b970477d5724f1a1d475ce5058cffe4efa8733899bdb863c47",  # YR2
    "35500cb63b8d9769528d65d87220f51c069807a595acbf2fb6eca38fbe0a4bd2",  # YR3
    "a2372d06431e9716365eeed47ec020351497d182fcc038e457e58168a03cac07",  # YE1
    "97658de8c68dfa98ace1e5028a63d54a1aae911b3e21471076c6850cd08cbab4",  # YE2
    "82bcfc482cc19f71e6f98f4a027616ebda6d47096d408b336160d1c0ef8b995a",  # YE3
    "e57b7e6f150c419102e8d5c055729ff967b9d1a829bf00cec89ca604ebf4a86f",  # Root YR
    "e14ffcad5b0025731006caa43a121a22d8e9700f4fb9cf852f02a708aa5d5666",  # Root YE
]
if [hashlib.sha256(ssl.PEM_cert_to_DER_cert(r)).hexdigest() for r in records] != expected_der:
    raise SystemExit("Let's Encrypt Gen-Y publisher DER fingerprint mismatch")
for record in records[:6]:
    subprocess.run(["openssl", "x509", "-checkend", str(30 * 24 * 60 * 60), "-noout"],
                   input=record, text=True, check=True)
# DigiCert for the PlayStation Store: pinned to the publisher's DER
# fingerprints, each verified against the unchanged Mozilla roots, none near expiry.
digicert = assets / "digicert-playstation.pem"
records = ["-----BEGIN CERTIFICATE-----" + part for part in digicert.read_text().split("-----BEGIN CERTIFICATE-----")[1:]]
expected_der = [
    "c06e307f7cfc1d32fa72a4c033c87b90019af216f0775d64978a2eca6c8a230e",  # GeoTrust TLS RSA CA G1
    "c8025f9fc65fdfc95b3ca8cc7867b9a587b5277973957917463fc813d0b625a9",  # DigiCert Global G2 TLS RSA SHA256 2020 CA1
    "cb3ccbb76031e5e0138f8dd39a23f9de47ffc35e43c1144cea27d46a5ab1cb5f",  # DigiCert Global Root G2
]
if [hashlib.sha256(ssl.PEM_cert_to_DER_cert(r)).hexdigest() for r in records] != expected_der:
    raise SystemExit("DigiCert publisher DER fingerprint mismatch")
for record in records:
    subprocess.run(["openssl", "verify", "-CAfile", str(assets / "cacert.pem"), "-no-CApath", "-no-CAstore"],
                   input=record, text=True, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["openssl", "x509", "-checkend", str(30 * 24 * 60 * 60), "-noout"], input=record, text=True, check=True)
print("TLS trust assets: fingerprints, CA constraints, root signature and expiry verified")
