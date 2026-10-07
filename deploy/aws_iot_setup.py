#!/usr/bin/env python3
"""Create the AWS IoT Core pieces the logger and the app need. Safe to re-run.

  python deploy/aws_iot_setup.py --region ap-south-1 --device rtd-logger-01 --out deploy/certs

Credentials come from the usual AWS chain (env vars, ~/.aws), or --keys-csv with an IAM
"accessKeys.csv" download. Nothing secret is printed; certificates and private keys are written
to --out, which is git-ignored.

Creates:
  * policy rtd-device-policy     a device may connect as itself and publish only rtd/<its name>/telemetry
  * policy rtd-dashboard-policy  the app may connect as rtd-dashboard and subscribe to rtd/+/telemetry
  * thing  <device>     + certificate + private key   (flash these onto the logger)
  * thing  rtd-dashboard + certificate + private key  (used by the server)
"""
import argparse
import csv
import json
import os
import sys
import urllib.request

import boto3
from botocore.exceptions import ClientError

ROOT_CA_URL = "https://www.amazontrust.com/repository/AmazonRootCA1.pem"


def session_from(args) -> boto3.Session:
    if args.keys_csv:
        with open(args.keys_csv, newline="", encoding="utf-8-sig") as f:
            row = {k.strip().lower(): v.strip() for k, v in next(csv.DictReader(f)).items()}
        return boto3.Session(aws_access_key_id=row["access key id"],
                             aws_secret_access_key=row["secret access key"], region_name=args.region)
    return boto3.Session(region_name=args.region)


def policies(region: str, account: str) -> dict:
    arn = f"arn:aws:iot:{region}:{account}"
    return {
        "rtd-device-policy": {
            "Version": "2012-10-17",
            "Statement": [
                {"Effect": "Allow", "Action": "iot:Connect",
                 "Resource": f"{arn}:client/${{iot:Connection.Thing.ThingName}}"},
                {"Effect": "Allow", "Action": "iot:Publish",
                 "Resource": f"{arn}:topic/rtd/${{iot:Connection.Thing.ThingName}}/telemetry"},
            ],
        },
        "rtd-dashboard-policy": {
            "Version": "2012-10-17",
            "Statement": [
                {"Effect": "Allow", "Action": "iot:Connect", "Resource": f"{arn}:client/rtd-dashboard"},
                {"Effect": "Allow", "Action": "iot:Subscribe", "Resource": f"{arn}:topicfilter/rtd/+/telemetry"},
                {"Effect": "Allow", "Action": "iot:Receive", "Resource": f"{arn}:topic/rtd/*/telemetry"},
            ],
        },
    }


def ensure_policy(iot, name: str, document: dict) -> None:
    try:
        iot.create_policy(policyName=name, policyDocument=json.dumps(document))
        print(f"policy {name}: created")
    except iot.exceptions.ResourceAlreadyExistsException:
        print(f"policy {name}: already exists (left as is)")


def ensure_thing_with_cert(iot, thing: str, policy: str, out: str) -> None:
    try:
        iot.create_thing(thingName=thing)
        print(f"thing {thing}: created")
    except iot.exceptions.ResourceAlreadyExistsException:
        pass
    if iot.list_thing_principals(thingName=thing)["principals"]:
        print(f"thing {thing}: already has a certificate, not creating another")
        return
    created = iot.create_keys_and_certificate(setAsActive=True)
    iot.attach_policy(policyName=policy, target=created["certificateArn"])
    iot.attach_thing_principal(thingName=thing, principal=created["certificateArn"])
    cert_path = os.path.join(out, f"{thing}.cert.pem")
    key_path = os.path.join(out, f"{thing}.private.key")
    with open(cert_path, "w", newline="\n") as f:
        f.write(created["certificatePem"])
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", newline="\n") as f:
        f.write(created["keyPair"]["PrivateKey"])
    print(f"thing {thing}: certificate saved to {cert_path} and key to {key_path}")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--region", default="ap-south-1")
    p.add_argument("--device", default="rtd-logger-01")
    p.add_argument("--out", default=os.path.join("deploy", "certs"))
    p.add_argument("--keys-csv", help="IAM accessKeys.csv; otherwise the standard AWS credential chain")
    args = p.parse_args()

    session = session_from(args)
    try:
        account = session.client("sts").get_caller_identity()["Account"]
        iot = session.client("iot")
        os.makedirs(args.out, exist_ok=True)

        for name, document in policies(args.region, account).items():
            ensure_policy(iot, name, document)
        ensure_thing_with_cert(iot, args.device, "rtd-device-policy", args.out)
        ensure_thing_with_cert(iot, "rtd-dashboard", "rtd-dashboard-policy", args.out)

        ca_path = os.path.join(args.out, "AmazonRootCA1.pem")
        if not os.path.exists(ca_path):
            urllib.request.urlretrieve(ROOT_CA_URL, ca_path)
        endpoint = iot.describe_endpoint(endpointType="iot:Data-ATS")["endpointAddress"]
    except ClientError as e:
        sys.exit(f"AWS refused: {e.response['Error']['Code']}: {e.response['Error']['Message']}")

    print(f"\nMQTT endpoint: {endpoint}  (port 8883, TLS)")
    print(f"Root CA saved to {ca_path}")


if __name__ == "__main__":
    main()
