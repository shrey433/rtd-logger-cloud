#!/usr/bin/env python3
"""Create the one EC2 server that runs the app. Safe to re-run: it reuses what already exists.

  python deploy/aws_ec2_setup.py --keys-csv rtd-developer_accessKeys.csv

Creates (all in the default VPC of the chosen region):
  * security group rtd-server-sg   80 and 443 open to everyone, SSH (22) only from this PC's current IP
  * key pair rtd-server-key        private key saved to --out (git-ignored)
  * instance rtd-server            Ubuntu 22.04, Docker installed on first boot, IMDSv2 only
  * an Elastic IP, so the address (and the sslip.io name derived from it) never changes
"""
import argparse
import csv
import os
import sys
import urllib.request

import boto3
from botocore.exceptions import ClientError

NAME = "rtd-server"
USER_DATA = """#!/bin/bash
curl -fsSL https://get.docker.com | sh
usermod -aG docker ubuntu
touch /var/lib/rtd-docker-ready
"""
UBUNTU_PARAM = "/aws/service/canonical/ubuntu/server/22.04/stable/current/amd64/hvm/ebs-gp2/ami-id"


def session_from(args) -> boto3.Session:
    if args.keys_csv:
        with open(args.keys_csv, newline="", encoding="utf-8-sig") as f:
            row = {k.strip().lower(): v.strip() for k, v in next(csv.DictReader(f)).items()}
        return boto3.Session(aws_access_key_id=row["access key id"],
                             aws_secret_access_key=row["secret access key"], region_name=args.region)
    return boto3.Session(region_name=args.region)


def ensure_security_group(ec2, vpc_id: str, my_ip: str) -> str:
    found = ec2.describe_security_groups(Filters=[{"Name": "group-name", "Values": ["rtd-server-sg"]},
                                                  {"Name": "vpc-id", "Values": [vpc_id]}])["SecurityGroups"]
    if found:
        sg_id = found[0]["GroupId"]
        print(f"security group: reusing {sg_id}")
    else:
        sg_id = ec2.create_security_group(GroupName="rtd-server-sg", VpcId=vpc_id,
                                          Description="RTD logger dashboard: web from anywhere, SSH from one IP")["GroupId"]
        print(f"security group: created {sg_id}")
    rules = [(80, "0.0.0.0/0"), (443, "0.0.0.0/0"), (22, f"{my_ip}/32")]
    for port, cidr in rules:
        try:
            ec2.authorize_security_group_ingress(GroupId=sg_id, IpPermissions=[
                {"IpProtocol": "tcp", "FromPort": port, "ToPort": port, "IpRanges": [{"CidrIp": cidr}]}])
            print(f"  opened port {port} to {cidr}")
        except ClientError as e:
            if e.response["Error"]["Code"] != "InvalidPermission.Duplicate":
                raise
    return sg_id


def ensure_key(ec2, out: str) -> str:
    path = os.path.join(out, "rtd-server-key.pem")
    try:
        ec2.describe_key_pairs(KeyNames=["rtd-server-key"])
        if not os.path.exists(path):
            sys.exit("key pair rtd-server-key exists in AWS but its private key is not in " + out +
                     "; delete the key pair in the console and re-run")
        print("key pair: reusing rtd-server-key")
    except ClientError as e:
        if e.response["Error"]["Code"] != "InvalidKeyPair.NotFound":
            raise
        material = ec2.create_key_pair(KeyName="rtd-server-key", KeyType="ed25519")["KeyMaterial"]
        os.makedirs(out, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", newline="\n") as f:
            f.write(material)
        print(f"key pair: created, private key saved to {path}")
    return "rtd-server-key"


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--region", default="ap-south-1")
    p.add_argument("--instance-type", default="t3.micro")
    p.add_argument("--disk-gb", type=int, default=16)
    p.add_argument("--out", default=os.path.join("deploy", "certs"))
    p.add_argument("--keys-csv")
    args = p.parse_args()

    session = session_from(args)
    ec2 = session.client("ec2")
    try:
        my_ip = urllib.request.urlopen("https://checkip.amazonaws.com", timeout=10).read().decode().strip()
        vpc_id = ec2.describe_vpcs(Filters=[{"Name": "isDefault", "Values": ["true"]}])["Vpcs"][0]["VpcId"]
        sg_id = ensure_security_group(ec2, vpc_id, my_ip)
        key_name = ensure_key(ec2, args.out)

        existing = ec2.describe_instances(Filters=[
            {"Name": "tag:Name", "Values": [NAME]},
            {"Name": "instance-state-name", "Values": ["pending", "running", "stopped"]}])["Reservations"]
        if existing:
            instance_id = existing[0]["Instances"][0]["InstanceId"]
            print(f"instance: reusing {instance_id}")
        else:
            ami = session.client("ssm").get_parameter(Name=UBUNTU_PARAM)["Parameter"]["Value"]
            instance_id = ec2.run_instances(
                ImageId=ami, InstanceType=args.instance_type, KeyName=key_name, SecurityGroupIds=[sg_id],
                MinCount=1, MaxCount=1, UserData=USER_DATA,
                BlockDeviceMappings=[{"DeviceName": "/dev/sda1",
                                      "Ebs": {"VolumeSize": args.disk_gb, "VolumeType": "gp3",
                                              "DeleteOnTermination": True}}],
                MetadataOptions={"HttpTokens": "required", "HttpEndpoint": "enabled"},
                TagSpecifications=[{"ResourceType": "instance", "Tags": [{"Key": "Name", "Value": NAME}]}],
            )["Instances"][0]["InstanceId"]
            print(f"instance: launched {instance_id} ({args.instance_type})")
        ec2.get_waiter("instance_running").wait(InstanceIds=[instance_id])

        addresses = [a for a in ec2.describe_addresses()["Addresses"] if a.get("InstanceId") == instance_id]
        if addresses:
            ip = addresses[0]["PublicIp"]
            print(f"elastic ip: reusing {ip}")
        else:
            alloc = ec2.allocate_address(Domain="vpc", TagSpecifications=[
                {"ResourceType": "elastic-ip", "Tags": [{"Key": "Name", "Value": NAME}]}])
            ec2.associate_address(InstanceId=instance_id, AllocationId=alloc["AllocationId"])
            ip = alloc["PublicIp"]
            print(f"elastic ip: {ip} attached")
    except ClientError as e:
        sys.exit(f"AWS refused: {e.response['Error']['Code']}: {e.response['Error']['Message']}")

    print(f"\nhost   : {ip}")
    print(f"domain : {ip.replace('.', '-')}.sslip.io")


if __name__ == "__main__":
    main()
