#!/usr/bin/env bash
# setup_veth.sh — create or delete the veth pair used for AF_XDP testing.
#
# Usage (must run as root):
#   sudo ./tools/setup_veth.sh create
#   sudo ./tools/setup_veth.sh delete
#
# After "create":
#   sender_ns / veth0  10.11.0.1/24   sender side  (SyntheticSender binds here)
#   root ns   / veth1  10.11.0.2/24   receiver side (FeedHandler opens AF_XDP here)
#
# Placing veth0 in a separate network namespace forces packets to physically
# cross the veth pair instead of being short-circuited through loopback.

set -euo pipefail

case "${1:-}" in
  create)
    echo "[setup_veth] Creating network namespace sender_ns ..."
    ip netns add sender_ns

    echo "[setup_veth] Creating veth pair veth0 <-> veth1 ..."
    ip link add veth0 type veth peer name veth1

    echo "[setup_veth] Moving veth0 into sender_ns ..."
    ip link set veth0 netns sender_ns

    echo "[setup_veth] Configuring veth0 (sender_ns) ..."
    ip netns exec sender_ns ip link set veth0 up
    ip netns exec sender_ns ip addr add 10.11.0.1/24 dev veth0

    echo "[setup_veth] Configuring veth1 (root ns) ..."
    ip link set veth1 up
    ip addr add 10.11.0.2/24 dev veth1

    # Disable GRO/GSO/TSO — AF_XDP requires these off on veth so that
    # frames are not aggregated before reaching the XDP hook.
    ip netns exec sender_ns \
        ethtool -K veth0 gro off gso off tso off 2>/dev/null || true
    ethtool -K veth1 gro off gso off tso off 2>/dev/null || true

    # Pre-populate static ARP entries so AF_XDP never intercepts an ARP
    # request before the kernel can reply.  Without this, the sender sends
    # an ARP request for 10.11.0.2; AF_XDP captures it on veth1; the kernel
    # never sees it; no ARP reply is ever sent; all UDP traffic stalls.
    VETH1_MAC=$(cat /sys/class/net/veth1/address)
    VETH0_MAC=$(ip netns exec sender_ns cat /sys/class/net/veth0/address)

    echo "[setup_veth] Adding static ARP: sender_ns knows veth1 MAC ($VETH1_MAC) ..."
    ip netns exec sender_ns \
        ip neigh add 10.11.0.2 lladdr "$VETH1_MAC" dev veth0 nud permanent

    echo "[setup_veth] Adding static ARP: root ns knows veth0 MAC ($VETH0_MAC) ..."
    ip neigh add 10.11.0.1 lladdr "$VETH0_MAC" dev veth1 nud permanent

    echo "[setup_veth] Done."
    echo "  sender_ns / veth0  10.11.0.1/24  (sender)"
    echo "  root ns   / veth1  10.11.0.2/24  (AF_XDP receiver)"
    ;;

  delete)
    echo "[setup_veth] Removing veth pair and sender_ns ..."
    # Deleting veth1 automatically removes its peer veth0 (even across namespaces).
    ip link del veth1 2>/dev/null || true
    ip netns del sender_ns 2>/dev/null || true
    echo "[setup_veth] Done."
    ;;

  *)
    echo "Usage: $0 create|delete" >&2
    exit 1
    ;;
esac
