# AWS setup: IoT Core + one Lightsail server (Mumbai, ap-south-1)

Status: all of this has been run. The server is a t3.micro EC2 instance paid from the AWS Free plan credits (the plan expires 2026-12-02; after that the account must be upgraded or the resources go away).

```
logger --MQTT/TLS 8883--> AWS IoT Core --MQTT/TLS--> app (EC2) --HTTPS + live stream--> your browser
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

1. Create the instance: `python deploy/aws_ec2_setup.py --keys-csv rtd-developer_accessKeys.csv`. It makes a security group (80 and 443 open to all, SSH from one address), an SSH key pair saved in `deploy/certs/`, a t3.micro Ubuntu 22.04 instance that installs Docker on first boot, and an Elastic IP.
2. Create `deploy/.env` from `deploy/.env.example`. With no domain, `DOMAIN` is the Elastic IP with dashes plus `.sslip.io` (for 13.232.10.20 that is `13-232-10-20.sslip.io`).
3. Ship and start it: `bash deploy/deploy.sh <elastic-ip>`. It sends the committed code (`git archive`), the app's certificate and `deploy/.env`, then runs `docker compose up -d --build`. Run it again after each change you commit.
4. Check `https://<DOMAIN>/healthz` shows `"mqtt":"subscribed"`, then open `https://<DOMAIN>/` and sign in with the dashboard user and password.

SSH note: the security group only lets in the address you give it, and the address AWS sees for SSH can differ from what `checkip.amazonaws.com` reports (carrier-grade NAT). If SSH times out, check which address the server sees (`echo $SSH_CLIENT` over a temporary rule) and allow that one.

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
