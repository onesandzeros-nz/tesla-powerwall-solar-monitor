#!/usr/bin/env python3
"""
One-time Tesla Fleet API setup helper for the Powerwall dashboard.

Run the subcommands in order:

    python3 tesla_setup.py genkey      # 1. make the EC key pair to host on your domain
    python3 tesla_setup.py register    # 2. register your partner account with Tesla
    python3 tesla_setup.py login       # 3. one-time OAuth -> get the refresh token
    python3 tesla_setup.py site        # 4. find your energy_site_id
    python3 tesla_setup.py test        # 5. sanity check: refresh + fetch live_status

Everything reads/writes ../secrets/tesla_secrets.env (git-ignored).
The firmware only needs TESLA_CLIENT_ID, TESLA_CLIENT_SECRET,
TESLA_REFRESH_TOKEN and TESLA_ENERGY_SITE_ID from that file.

Tesla auth endpoints are global at https://auth.tesla.com; data calls go to
the regional TESLA_AUDIENCE base URL.
"""
import os
import sys
import json
import subprocess
import urllib.parse
from pathlib import Path

try:
    import requests
except ImportError:
    sys.exit("Missing dependency. Run:  pip install -r requirements.txt")

ROOT = Path(__file__).resolve().parent.parent
SECRETS_DIR = ROOT / "secrets"
ENV_FILE = SECRETS_DIR / "tesla_secrets.env"
PRIV_KEY = SECRETS_DIR / "private-key.pem"
PUB_KEY = SECRETS_DIR / "public-key.pem"

AUTH_BASE = "https://auth.tesla.com"
TOKEN_URL = f"{AUTH_BASE}/oauth2/v3/token"
AUTHZ_URL = f"{AUTH_BASE}/oauth2/v3/authorize"
# Partner (client_credentials) tokens must NOT request offline_access — that
# scope is only valid in the user authorization-code flow. Sending it here
# makes Tesla return 400. The user login below DOES need offline_access (it's
# what yields a refresh token).
PARTNER_SCOPES = "openid energy_device_data"
USER_SCOPES = "openid offline_access energy_device_data"


# ---------- tiny .env helpers ----------
def load_env() -> dict:
    if not ENV_FILE.exists():
        sys.exit(f"{ENV_FILE} not found. Copy tesla_secrets.example.env to it and fill it in.")
    env = {}
    for line in ENV_FILE.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        v = v.strip()
        # tolerate accidentally quoted values: TESLA_CLIENT_SECRET="ta-secret..."
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        env[k.strip()] = v
    return env


def save_env_value(key: str, value: str) -> None:
    lines = ENV_FILE.read_text().splitlines()
    out, found = [], False
    for line in lines:
        if line.strip().startswith(f"{key}="):
            out.append(f"{key}={value}")
            found = True
        else:
            out.append(line)
    if not found:
        out.append(f"{key}={value}")
    ENV_FILE.write_text("\n".join(out) + "\n")
    print(f"  saved {key} to {ENV_FILE.name}")


def require(env: dict, *keys: str) -> None:
    missing = [k for k in keys if not env.get(k)]
    if missing:
        sys.exit(f"Missing in {ENV_FILE.name}: {', '.join(missing)}")


# ---------- subcommands ----------
def cmd_genkey(_env=None) -> None:
    """Generate an EC prime256v1 key pair. Host the PUBLIC key on your domain."""
    if PRIV_KEY.exists():
        sys.exit(f"{PRIV_KEY} already exists — refusing to overwrite. Delete it first if you really mean to.")
    subprocess.run(
        ["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(PRIV_KEY)],
        check=True,
    )
    subprocess.run(
        ["openssl", "ec", "-in", str(PRIV_KEY), "-pubout", "-out", str(PUB_KEY)],
        check=True,
    )
    print(f"\nWrote {PRIV_KEY.name} (keep secret) and {PUB_KEY.name}.")
    print("\nNow host the PUBLIC key at exactly this URL on your domain:")
    env = load_env()
    domain = env.get("TESLA_DOMAIN", "<your-domain>")
    print(f"  https://{domain}/.well-known/appspecific/com.tesla.3p.public-key.pem")
    print(f"\nIt must serve the contents of {PUB_KEY} verbatim, as text/plain.")
    print("Verify it's reachable, then run:  python3 tesla_setup.py register")


def _post_token(data: dict) -> dict:
    """POST to the token endpoint, surfacing Tesla's error body on failure."""
    r = requests.post(TOKEN_URL, data=data, timeout=30)
    if not r.ok:
        print(f"HTTP {r.status_code} from {TOKEN_URL}")
        print(r.text)  # Tesla returns {"error": "...", "error_description": "..."}
        sys.exit("Token request failed (see error above).")
    return r.json()


def _partner_token(env: dict) -> str:
    return _post_token({
        "grant_type": "client_credentials",
        "client_id": env["TESLA_CLIENT_ID"],
        "client_secret": env["TESLA_CLIENT_SECRET"],
        "scope": PARTNER_SCOPES,
        "audience": env["TESLA_AUDIENCE"],
    })["access_token"]


def cmd_register(env: dict) -> None:
    """Register the partner account (requires the public key to be live on your domain)."""
    require(env, "TESLA_CLIENT_ID", "TESLA_CLIENT_SECRET", "TESLA_AUDIENCE", "TESLA_DOMAIN")
    token = _partner_token(env)
    r = requests.post(
        f"{env['TESLA_AUDIENCE']}/api/1/partner_accounts",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json={"domain": env["TESLA_DOMAIN"]},
        timeout=30,
    )
    print(f"HTTP {r.status_code}")
    print(json.dumps(r.json(), indent=2))
    if r.ok:
        print("\nPartner registered. Next:  python3 tesla_setup.py login")
    else:
        print("\nIf this failed on the public key, confirm this URL serves the key as text:")
        print(f"  https://{env['TESLA_DOMAIN']}/.well-known/appspecific/com.tesla.3p.public-key.pem")


def cmd_login(env: dict) -> None:
    """Run the user OAuth flow once and store the refresh token."""
    require(env, "TESLA_CLIENT_ID", "TESLA_CLIENT_SECRET", "TESLA_AUDIENCE", "TESLA_REDIRECT_URI")
    params = {
        "response_type": "code",
        "client_id": env["TESLA_CLIENT_ID"],
        "redirect_uri": env["TESLA_REDIRECT_URI"],
        "scope": USER_SCOPES,
        "state": "powerwall-dashboard",
    }
    url = f"{AUTHZ_URL}?{urllib.parse.urlencode(params)}"
    print("\n1) Open this URL in a browser and log in with your Tesla account:\n")
    print(f"   {url}\n")
    print("2) After approving, your browser is redirected to your redirect URI with")
    print("   ?code=...  in the address bar (the page itself may 404 — that's fine).")
    print("3) Paste the FULL redirected URL (or just the code) here.\n")
    pasted = input("Redirected URL or code: ").strip()
    code = pasted
    if "code=" in pasted:
        q = urllib.parse.urlparse(pasted).query
        code = urllib.parse.parse_qs(q).get("code", [pasted])[0]

    tok = _post_token({
        "grant_type": "authorization_code",
        "client_id": env["TESLA_CLIENT_ID"],
        "client_secret": env["TESLA_CLIENT_SECRET"],
        "code": code,
        "audience": env["TESLA_AUDIENCE"],
        "redirect_uri": env["TESLA_REDIRECT_URI"],
    })
    save_env_value("TESLA_REFRESH_TOKEN", tok["refresh_token"])
    print("\nGot the refresh token. Next:  python3 tesla_setup.py site")


def _access_token(env: dict) -> str:
    require(env, "TESLA_CLIENT_ID", "TESLA_REFRESH_TOKEN")
    tok = _post_token({
        "grant_type": "refresh_token",
        "client_id": env["TESLA_CLIENT_ID"],
        "refresh_token": env["TESLA_REFRESH_TOKEN"],
    })
    # Tesla may rotate the refresh token; persist the new one if present.
    if tok.get("refresh_token") and tok["refresh_token"] != env.get("TESLA_REFRESH_TOKEN"):
        save_env_value("TESLA_REFRESH_TOKEN", tok["refresh_token"])
    return tok["access_token"]


def cmd_site(env: dict) -> None:
    """List products and store the energy_site_id."""
    token = _access_token(env)
    r = requests.get(
        f"{env['TESLA_AUDIENCE']}/api/1/products",
        headers={"Authorization": f"Bearer {token}"},
        timeout=30,
    )
    r.raise_for_status()
    products = r.json().get("response", [])
    sites = [p for p in products if "energy_site_id" in p]
    if not sites:
        print(json.dumps(products, indent=2))
        sys.exit("No energy sites found on this account.")
    for s in sites:
        print(f"  energy_site_id={s['energy_site_id']}  "
              f"name={s.get('site_name')}  type={s.get('resource_type')}")
    save_env_value("TESLA_ENERGY_SITE_ID", str(sites[0]["energy_site_id"]))
    if len(sites) > 1:
        print("\nMultiple sites — saved the first. Edit TESLA_ENERGY_SITE_ID if that's wrong.")
    print("\nAll set. Verify with:  python3 tesla_setup.py test")


def cmd_info(env: dict) -> None:
    """Dump site_info — capacity, nameplate power, etc. (not in live_status)."""
    require(env, "TESLA_ENERGY_SITE_ID")
    token = _access_token(env)
    r = requests.get(
        f"{env['TESLA_AUDIENCE']}/api/1/energy_sites/{env['TESLA_ENERGY_SITE_ID']}/site_info",
        headers={"Authorization": f"Bearer {token}"},
        timeout=30,
    )
    r.raise_for_status()
    print(json.dumps(r.json()["response"], indent=2))


def cmd_test(env: dict) -> None:
    """Refresh a token and print live_status — the exact call the firmware makes."""
    require(env, "TESLA_ENERGY_SITE_ID")
    token = _access_token(env)
    r = requests.get(
        f"{env['TESLA_AUDIENCE']}/api/1/energy_sites/{env['TESLA_ENERGY_SITE_ID']}/live_status",
        headers={"Authorization": f"Bearer {token}"},
        timeout=30,
    )
    r.raise_for_status()
    d = r.json()["response"]
    print(json.dumps(d, indent=2))
    print("\n--- what the dashboard will show ---")
    print(f"  Solar:   {d.get('solar_power', 0)/1000:6.2f} kW")
    print(f"  Home:    {d.get('load_power', 0)/1000:6.2f} kW")
    print(f"  Grid:    {d.get('grid_power', 0)/1000:6.2f} kW  (+ import, - export)")
    print(f"  Battery: {d.get('battery_power', 0)/1000:6.2f} kW  (+ discharge, - charge)")
    print(f"  Charge:  {d.get('percentage_charged', 0):6.1f} %")


def cmd_check(env: dict) -> None:
    """Print what the script loaded from the .env (secret stays masked)."""
    def mask(s: str) -> str:
        return f"{s[:4]}…{s[-3:]}  (length {len(s)})" if s else "(EMPTY)"
    print(f"  CLIENT_ID:     {env.get('TESLA_CLIENT_ID') or '(EMPTY)'}")
    print(f"  CLIENT_SECRET: {mask(env.get('TESLA_CLIENT_SECRET', ''))}")
    print(f"  AUDIENCE:      {env.get('TESLA_AUDIENCE') or '(EMPTY)'}")
    print(f"  DOMAIN:        {env.get('TESLA_DOMAIN') or '(EMPTY)'}")
    print(f"  REDIRECT_URI:  {env.get('TESLA_REDIRECT_URI') or '(EMPTY)'}")
    print("\nConfirm CLIENT_ID matches your app and CLIENT_SECRET is the current one.")


COMMANDS = {
    "check": cmd_check,
    "genkey": cmd_genkey,
    "register": cmd_register,
    "login": cmd_login,
    "site": cmd_site,
    "test": cmd_test,
    "info": cmd_info,
}


def main() -> None:
    if len(sys.argv) != 2 or sys.argv[1] not in COMMANDS:
        sys.exit(f"Usage: python3 tesla_setup.py [{' | '.join(COMMANDS)}]")
    cmd = sys.argv[1]
    # genkey needs the env only for the domain hint; others need real values.
    env = load_env() if ENV_FILE.exists() else {}
    COMMANDS[cmd](env)


if __name__ == "__main__":
    main()
