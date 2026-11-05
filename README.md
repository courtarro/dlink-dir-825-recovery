# dlinkflash

Flash older D-Link routers through their emergency (recovery) web interface.

The router's recovery page is a very small HTTP server with a tiny TCP stack, so it only works if the upload is sent slowly, in small chunks, with no other traffic getting in the way. This tool does exactly that.

These instructions are exclusively for Linux-based hosts.

Note that this process worked in a DIR-825 B1 with the latest [OpenWRT 19 'backup loader' firmware](https://archive.openwrt.org/releases/19.07.9/targets/ar71xx/generic/openwrt-19.07.9-ar71xx-generic-dir-825-b1-squashfs-backup-loader.bin). Details for [OpenWRT on this hardware](https://openwrt.org/toh/d-link/dir-825).

## Build

```bash
gcc -Wall -Wextra -O2 -o dlinkflash dlinkflash.c
```

## Usage

```bash
./dlinkflash <firmware.bin> [-d]
```

`-d` prints debug output (bytes sent/received and the router's replies) to stderr.

Before running it:

1. Put the router into emergency/recovery mode by holding the reset button while applying power. This usually takes 30-40 seconds.
2. Connect your ethernet port directly to the router.
3. Give that port an address in `192.168.0.0/24` other than `.1`, e.g. `192.168.0.2/24`. The router is always at `192.168.0.1`.

## How it works

1. **Load firmware.** The whole file is read into memory.
2. **Build two HTTP requests.**
   - A plain `GET /` (the recovery page).
   - A `POST /cgi/index` with a `multipart/form-data` body containing the firmware. Headers imitate IE6 on Windows 98, which is what the recovery page expects. `Content-Length` is deliberately one byte larger than the real body (an old IE6 quirk the router seems to rely on).
3. **Phase 1: GET.** Connect to `192.168.0.1:80`, send the GET, and read the page until the router closes the connection or goes quiet for 5 seconds. This puts the router in a state where it will accept the upload.
4. **Phase 2: POST.** Open a fresh connection and send the POST in 512-byte chunks with a 5 ms pause after each chunk, so the router can keep up. Sockets are set up with a small MSS, send/receive buffer and window clamp for the same reason.
5. **Check the result.** Read the router's reply (up to 60 seconds). If it contains `count_down`, the router accepted the image and is rebooting. Otherwise the reply is printed and the tool exits with an error.

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | Firmware sent, router is rebooting |
| 1 | Usage error or firmware file could not be opened |
| 2 | Firmware read error, empty file, or out of memory |
| 5 | Send error during upload |
| 6 | Receive error, timeout, or no `count_down` in the reply |
| 7 | Could not connect to the router, or send error during the GET |
| 8 | Receive error during the GET |

## Isolating the link with a network namespace

If other programs (avahi/mDNS, IPv6 router solicitations, DHCP, NetworkManager checks) send traffic on the same link, they can confuse the router. Putting the NIC in its own network namespace means only the processes you start inside it can use the link.

Replace `enp3s0` with your interface name (`ip -br link` lists them).

### Set up an isolated network namespace

```bash
# Stop NetworkManager from managing the NIC (runtime only)
sudo nmcli dev set enp3s0 managed no
sudo ip link set enp3s0 down

# Create the namespace and disable IPv6 in it before the NIC arrives
sudo ip netns add dlink
sudo ip netns exec dlink sysctl -qw \
    net.ipv6.conf.all.disable_ipv6=1 net.ipv6.conf.default.disable_ipv6=1

# Move the NIC in and configure it
sudo ip link set enp3s0 netns dlink
sudo ip -n dlink addr add 192.168.0.2/24 dev enp3s0
sudo ip -n dlink link set lo up
sudo ip -n dlink link set enp3s0 up
```

### Flash

```bash
sudo ip netns exec dlink ./dlinkflash firmware.bin -d
```

### Watch the traffic (optional)

Live in Wireshark (capture runs in the namespace, GUI runs as you):

```bash
sudo ip netns exec dlink dumpcap -q -i enp3s0 -w - | wireshark -k -i -
```

Or capture to a file and open it afterwards:

```bash
sudo ip netns exec dlink dumpcap -i enp3s0 -w /tmp/dl.pcapng   # Ctrl-C to stop
sudo chown $USER /tmp/dl.pcapng
wireshark /tmp/dl.pcapng
```

To see where an upload stalls, open the capture and use
**Statistics → TCP Stream Graphs → Time Sequence (tcptrace)** on the POST connection.

### Tear down

```bash
sudo ip netns del dlink                 # NIC returns to the main namespace
sudo nmcli dev set enp3s0 managed yes
```

Nothing here survives a reboot: the namespace lives in `/run/netns` (tmpfs), the NIC returns to the main namespace at boot, and `nmcli dev set ... managed` is runtime-only.

## Troubleshooting

- **"connect: No route to host" / timeout:** the NIC has no 192.168.0.x address, or the router isn't in recovery mode.
- **Hangs after "Finished sending GET":** the router isn't answering; it times out after 5 seconds and continues.
- **Upload stalls partway:** other traffic on the link, or the router needs a slower rate. Raise the `usleep(5000)` delay (the `5000` passed to `send_all` for the upload in `main`).
- **"Error sending firmware" and a printed reply:** the router rejected the image; check that the file is the correct firmware for your exact model and hardware revision.

## License

GPL v3 or later. Original code Copyright 2014 Daniel Dickinson with enhancements by Ethan Trewhitt.
