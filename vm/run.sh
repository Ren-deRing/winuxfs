#!/usr/bin/env bash
# Start the winuxfs Windows test VM (dockur/windows) in a detached podman
# container. Normally called by `tools/vmctl.py up`; see docs/vm.md.
#
# Environment overrides:
#   WINUXFS_VM_NAME   container name            (default winuxfs-vm)
#   WINUXFS_VM_IMAGE  dockur image              (default docker.io/dockurr/windows:6.06)
#   WINUXFS_ISO       local Windows 11 ISO      (default ~/다운로드/Windows11_Client_x64_en-us_26300_9457.iso)
#   WINUXFS_CPU       CPU cores for the guest   (default 4)
#   WINUXFS_RAM       RAM for the guest         (default 4G)
set -euo pipefail

VM_DIR="$(cd "$(dirname "$0")" && pwd)"
NAME="${WINUXFS_VM_NAME:-winuxfs-vm}"
IMAGE="${WINUXFS_VM_IMAGE:-docker.io/dockurr/windows:6.06}"
ISO="${WINUXFS_ISO:-$HOME/다운로드/Windows11_Client_x64_en-us_26300_9457.iso}"

mkdir -p "$VM_DIR/storage" "$VM_DIR/shared" "$VM_DIR/oem"

for f in authorized_keys winfsp.msi install.bat; do
  if [ ! -s "$VM_DIR/oem/$f" ]; then
    echo "run.sh: missing vm/oem/$f (run tools/vmctl.py up, see docs/vm.md)" >&2
    exit 1
  fi
done

args=(
  run -d --rm --name "$NAME"
  --device=/dev/kvm --device=/dev/net/tun --cap-add=NET_ADMIN
  # The user's ISO must not be relabeled with :Z, so SELinux separation is
  # disabled for this container instead (see docs/vm.md).
  --security-opt label=disable
  -p 127.0.0.1:8006:8006 -p 127.0.0.1:2222:22
  --stop-timeout 120
  -e VERSION=11
  -e CPU_CORES="${WINUXFS_CPU:-4}"
  -e RAM_SIZE="${WINUXFS_RAM:-4G}"
  -e USERNAME=Docker -e PASSWORD=admin
  # Needed only if dockur falls back to user-mode networking (rootless podman
  # without working NAT): Windows forwards only RDP there by default.
  -e USER_PORTS=22
  -v "$VM_DIR/storage:/storage:Z"
  -v "$VM_DIR/oem:/oem:Z"
  -v "$VM_DIR/shared:/shared:Z"
)

# A host block device as an NVMe disk of the guest, READ-ONLY unless WINUXFS_HOST_DISK_RW=1:
#   WINUXFS_HOST_DISK=/dev/nvme0n1p1 tools/vmctl.py up
# The device must be unmounted on the host and readable (setfacl -m u:$USER:r DEV; rw for the
# read-write mode). It is not given to dockur as /diskN (dockur would open it read-write): QEMU
# gets it as a block node, read-only by default, so even a "format" in the guest cannot write.
host_ro=on; [ "${WINUXFS_HOST_DISK_RW:-0}" = 1 ] && host_ro=off
if [ -n "${WINUXFS_HOST_DISK:-}" ]; then
  args+=(
    --device "$WINUXFS_HOST_DISK:/xraw"
    -e ARGUMENTS="-blockdev driver=host_device,node-name=xrawf,filename=/xraw,read-only=$host_ro -blockdev driver=raw,node-name=xrawd,file=xrawf,read-only=$host_ro -device nvme,serial=winuxfsx,drive=xrawd"
  )
fi

# Second disk: dockur uses an existing /storage2/data2.img as-is. Mount the
# directory only when an image is present; otherwise dockur would create a
# blank 64G data2.img on every boot.
if [ -s "$VM_DIR/storage2/data2.img" ]; then
  args+=(-v "$VM_DIR/storage2:/storage2:Z")
fi

# Local install media. Only needed for the first install; read-only and
# never relabeled. dockur ignores VERSION when /custom.iso is present.
if [ -f "$ISO" ]; then
  args+=(-v "$ISO:/custom.iso:ro")
elif [ ! -s "$VM_DIR/storage/data.img" ]; then
  echo "run.sh: ISO $ISO not found and no installed disk; dockur would download Windows (VERSION=11)" >&2
fi

exec podman "${args[@]}" "$IMAGE"
