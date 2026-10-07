// Copy this file to secrets.h (git-ignored) and fill it in.
// The device certificate and key are not here: they go in firmware/certs/ (see certs/README.md).
#pragma once

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// AWS IoT Core endpoint for your account and region (the script deploy/aws_iot_setup.py prints it).
#define MQTT_HOST     "your-endpoint-ats.iot.ap-south-1.amazonaws.com"
