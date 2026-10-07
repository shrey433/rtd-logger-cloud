# AWS setup: IoT Core + one Lightsail server (Mumbai, ap-south-1)

Status: `aws_iot_setup.py` and the Compose files are written but have not been run against AWS yet. Check each step as you go.

```
logger --MQTT/TLS 8883--> AWS IoT Core --MQTT/TLS--> app (Lightsail) --HTTPS + live stream--> your browser
                                                       SQLite on a volume
```

## 1. IoT Core: things, certificates, policies

```bash
pip install boto3
python deploy/aws_iot_setup.py --region ap-south-1 --device rtd-logger-01 --out deploy/certs
```

It creates:

| What | Allowed to |
|---|---|
| `rtd-device-policy` | connect as its own thing name, publish only `rtd/<thing name>/telemetry` |
| `rtd-dashboard-policy` | connect as `rtd-dashboard`, subscribe to `rtd/+/telemetry` |
| thing `rtd-logger-01` + certificate | the logger (flash the cert and key onto it) |
| thing `rtd-dashboard` + certificate | the server |

Certificates and keys land in `deploy/certs/`, which is git-ignored. The script prints the MQTT endpoint (for `MQTT_HOST`).

A second logger later is the same command with another `--device` name; the shared device policy already covers it.

## 2. The server

1. Lightsail: create an instance in ap-south-1 (Ubuntu 22.04, the 1 GB plan is enough), attach a **static IP**, and open ports 80 and 443 in its firewall.
2. Install Docker: `curl -fsSL https://get.docker.com | sh`.
3. Get the code onto the server. The repo is private, so add a read-only deploy key under the repo's Settings, Deploy keys, or copy the folder with `scp`.
4. Copy `deploy/certs/` to the server (`scp -r`), keeping it out of git.
5. `cp deploy/.env.example deploy/.env` and fill it in. With no domain, `DOMAIN` is the static IP with dashes plus `.sslip.io` (for 13.232.10.20 that is `13-232-10-20.sslip.io`).
6. `cd deploy && docker compose up -d --build`.
7. Check `https://<DOMAIN>/healthz` shows `"mqtt":"subscribed"`, then open `https://<DOMAIN>/` and sign in with the dashboard user and password.

## 3. Quick check from your PC, before the firmware speaks MQTT

Publish a test reading with the AWS IoT console's MQTT test client to topic `rtd/rtd-logger-01/telemetry`:

```json
{ "fw_version": "test", "backlog": 0, "ts": "2026-10-07T12:00:00Z",
  "channels": [ {"ch":1,"type":"PT100","temp_c":24.1}, {"ch":2,"type":"PT100","temp_c":24.2},
                {"ch":3,"type":"PT100","temp_c":24.0}, {"ch":4,"type":"PT100","temp_c":24.3},
                {"ch":5,"type":"PT100","temp_c":24.1}, {"ch":6,"type":"PT100","temp_c":24.2},
                {"ch":7,"type":"PT100","temp_c":24.0}, {"ch":8,"type":"PT1000","temp_c":24.1} ] }
```

Use a `ts` close to the current UTC time; readings older than 2024 or more than a day ahead are discarded.

## Cost and housekeeping

- One logger at 10 s is about 260,000 MQTT messages a month, a few tens of cents on IoT Core, plus the Lightsail plan. Check current AWS pricing.
- Do not use the AdministratorAccess keys on the server. The server authenticates to IoT Core with its certificate only.
