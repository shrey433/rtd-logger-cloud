# Device certificates

The logger authenticates to AWS IoT Core with its own certificate. Three files must exist here before the
firmware will build; they are embedded in the image and are git-ignored (`*.pem` and `*.key`).

| File here | Copy from |
|---|---|
| `AmazonRootCA1.pem` | `../deploy/certs/AmazonRootCA1.pem` |
| `device.cert.pem` | `../deploy/certs/rtd-logger-01.cert.pem` |
| `device.private.key` | `../deploy/certs/rtd-logger-01.private.key` |

```bash
cp ../deploy/certs/AmazonRootCA1.pem certs/AmazonRootCA1.pem
cp ../deploy/certs/rtd-logger-01.cert.pem certs/device.cert.pem
cp ../deploy/certs/rtd-logger-01.private.key certs/device.private.key
```

Run those from `firmware/`. `DEVICE_ID` in `include/config.h` must equal the thing name the certificate belongs to
(`rtd-logger-01`): the IoT policy only lets a device connect and publish as itself.

The private key sits unencrypted in the logger's flash. That is fine for a test unit; for a fleet, use flash
encryption or provisioning-time keys.
