# SculptureABC - SPDX-License-Identifier: GPL-3.0-or-later
"""LAN test server for the web build, so an iPad or phone on the same Wi-Fi can open it.

    python mobile/tools/serve.py [--port 8080] [--dir build/mobile-web]
    python mobile/tools/serve.py --https [--port 8443]

Use --https for testing speed on an iPad: iPadOS Safari ran the page ~30x slower over plain http to a LAN address
(no JIT: JavaScript and WebAssembly both), at full speed over https. --https makes a self-signed certificate for
this PC's LAN addresses (build/lan-cert/, kept between runs); the iPad warns about it once ("Show Details" ->
"visit this website").

Sends the cross-origin isolation headers multithreading will need (they only take effect over HTTPS
or on localhost), serves .wasm with the right type and disables caching so a rebuild shows at once.
"""
import argparse
import datetime
import ipaddress
import http.server
import os
import socket
import ssl
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript", ".webmanifest": "application/manifest+json"}

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


def lan_addresses():
    addresses = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.0.1", 9))  # no packet is sent; picks the LAN interface
        addresses.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addresses.add(info[4][0])
    except OSError:
        pass
    return sorted(a for a in addresses if not a.startswith("127."))


def make_certificate(folder, addresses):
    """A self-signed certificate for localhost and `addresses`; remade when an address is missing from it."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    os.makedirs(folder, exist_ok=True)
    cert_path, key_path = os.path.join(folder, "cert.pem"), os.path.join(folder, "key.pem")
    wanted = {"127.0.0.1", *addresses}
    if os.path.exists(cert_path) and os.path.exists(key_path):
        with open(cert_path, "rb") as f:
            cert = x509.load_pem_x509_certificate(f.read())
        san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
        have = {str(ip) for ip in san.get_values_for_type(x509.IPAddress)}
        if wanted <= have and cert.not_valid_after_utc > datetime.datetime.now(datetime.timezone.utc):
            return cert_path, key_path
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "SculptureABC LAN test")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=397))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")] +
                                                       [x509.IPAddress(ipaddress.ip_address(a)) for a in sorted(wanted)]),
                           critical=False)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .sign(key, hashes.SHA256()))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    return cert_path, key_path


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int)
    parser.add_argument("--dir", default=os.path.join(root, "build", "mobile-web"))
    parser.add_argument("--https", action="store_true", help="serve over https with a self-signed certificate")
    args = parser.parse_args()
    port = args.port or (8443 if args.https else 8080)
    scheme = "https" if args.https else "http"
    addresses = lan_addresses()
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    if args.https:
        cert, key = make_certificate(os.path.join(root, "build", "lan-cert"), addresses)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    os.chdir(args.dir)
    print(f"Serving {args.dir}")
    print(f"  this PC:      {scheme}://localhost:{port}/SculptureABC.html")
    for address in addresses:
        print(f"  iPad / phone: {scheme}://{address}:{port}/SculptureABC.html")
    sys.stdout.flush()
    server.serve_forever()


if __name__ == "__main__":
    main()
