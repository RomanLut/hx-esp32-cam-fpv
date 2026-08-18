# Spectator Mode and Camera Association

Spectator mode allows more than one ground station (GS) to receive video and OSD data from the same camera while ensuring that only the camera's associated GS can control it.

## Transport Availability

Spectator mode is available only with the raw broadcast transport. In broadcast mode, several ground stations can receive the same transmitted camera packets without making the camera send a separate video stream to each GS.

APFPV mode does not support spectator mode. Due to the camera's limited memory, its Wi-Fi access point intentionally permits only one connected client.

## Device IDs

Every camera and GS has a device ID:

- `airDeviceId` identifies the camera (Air unit).
- `gsDeviceId` identifies the ground station.
- Air-to-ground packets contain the camera ID and the ID of the GS that owns the camera.
- An Air-to-ground `gsDeviceId` of `0` means the camera has not associated with a GS since boot.

Each GS derives a stable nonzero 16-bit ID from its device identity. Android uses its secure device ID and build properties; Linux and Radxa use the machine ID. The GS Settings menu displays the active value as `ID: 0x....` on the same status line as the IP address.

Device IDs provide routing and ownership selection. They are not cryptographic authentication and do not protect against deliberate ID spoofing.

## Camera Association

After boot, the camera is initially unassociated. It associates with the first valid GS that does either of the following:

1. Sends a broadcast Connect packet with a nonzero GS ID.
2. Sends a valid Config packet addressed to that camera's Air ID.

When association succeeds, the camera:

- Stores the GS ID as its owner.
- Addresses all outgoing Air packets to that GS ID.
- Accepts Config, control, and telemetry packets only from that GS ID.
- Ignores packets from every other GS ID.

The association has no inactivity timeout. It remains fixed until the camera reboots. Restarting, disconnecting, or searching again on a GS does not release the camera. Rebooting only the GS also does not clear ownership stored by the running camera.

This first-GS-wins behavior means GS device IDs must be unique when several ground stations operate in the same area.

## Ground-Station Behavior

An unconnected GS listens without a GS-destination filter so that it can discover both unassociated cameras and cameras owned by another GS.

When it receives a valid Config packet, the GS records:

- The selected camera's Air ID.
- The GS ID carried by the camera's packets.
- Whether that ID matches its own local GS ID.

If the IDs match, the GS is the owner and operates normally.

If the IDs differ, the GS enters a receive-only spectator session. It filters incoming traffic using the selected camera ID and the camera's actual owner ID, allowing video, OSD, statistics, and telemetry reception from that stream.

While spectating, the GS does not:

- Send control or Config packets to the camera.
- Send buffered telemetry to the camera.
- Attempt to replace the camera's existing association.

The camera also independently rejects foreign-GS packets, so ownership is enforced on both sides of the link.

## `SPECTATOR` Warning

Every decoded Air packet from the currently selected camera with a nonzero GS destination different from the local GS ID refreshes the spectator warning for two seconds. Packets from other camera IDs are ignored and cannot trigger or refresh the warning.
