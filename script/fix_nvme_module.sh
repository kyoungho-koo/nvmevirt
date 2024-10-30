#!/bin/bash

echo "Starting NVMe module fix script..."

# Step 1: Update and reinstall kernel modules and headers
echo "Reinstalling kernel modules and headers for current kernel..."
sudo apt update
sudo apt install --reinstall -y linux-modules-$(uname -r) linux-headers-$(uname -r)

# Step 2: Rebuild module dependencies
echo "Rebuilding module dependencies..."
sudo depmod -a

# Step 3: Load NVMe modules
echo "Loading NVMe modules..."
sudo modeprobe nvme
sudo modeprobe nvme-core

echo " NVMe module fix completed!"


