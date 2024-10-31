
#!/bin/bash
rmmod nvmev
echo "NVMe Virt Removed"
nvme list

cd ..
make
cd script


echo "Intalling new NVMe Virt module"
insmod ../nvmev.ko memmap_start=32G memmap_size=64G cpus=15,16
sleep 5

echo "After"
nvme list

