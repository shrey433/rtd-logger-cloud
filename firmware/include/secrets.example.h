// Copy this file to secrets.h (git-ignored) and fill it in.
#pragma once

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Full ingest URL of the cloud server. Use https:// for anything on the internet.
#define SERVER_URL    "https://your-app.example.com/ingest"

// Must equal RTD_API_TOKEN on the server.
#define DEVICE_TOKEN  "change-me-to-the-server-token"

// Optional but recommended for https: PEM of the root CA that signed the server certificate.
// Without it the connection is encrypted but the server is not verified.
// #define SERVER_CA_CERT "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
