#source ./tool/parameter.sh

NSZE=134217728
RUNTIME=10
DATE=`date +%y%m%d_%H%M%S`

BLKDEV_NS=/dev/nvme0n1
NGDEV_NS=/dev/ng0n1

BLKDEV=/dev/nvme0
NGDEV=/dev/ng0


next_test() {
	echo Proceed to the next experiment? Press 'y' to continue or any other key to exit.
	read -p "Enter choice: " choice
	if [ "$choice" != "y"]; then
		echo Experiment terminated by user,
		exit 1
	fi
}

echo BLKDEV: ${BLKDEV}

next_test

echo ====== Delete Namespace ======
nvme delete-ns ${BLKDEV} -n 1
echo   PASS

next_test

echo ====== set FDP feature ======
nvme set-feature ${BLKDEV} -f 0x1D -c 0 -s
nvme set-feature ${BLKDEV} -f 0x1D -c 1 -s
echo   PASS

next_test

echo ====== Create and Attach Namespace ======
nvme create-ns ${BLKDEV} -b 512 --nsze=$NSZE --ncap=$NSZE -p 0,1,2,3 -n 4
nvme attach-ns ${BLKDEV} --namespace-id=1 --controller=0x7


nvme list
echo   PASS

next_test
echo ====== Write 1MB ======
dd if=/dev/zero of=${BLKDEV}n1 bs=1M count=1 status=progress
echo   PASS

next_test

echo "====== Write 100MB ======"
dd if=/dev/zero of=${BLKDEV}n1 bs=1M count=100 status=progress
echo "  PASS"

next_test

echo "====== Write 10GB ======"
dd if=/dev/zero of=${BLKDEV}n1 bs=1M count=10000 status=progress
echo "  PASS"

next_test

echo "====== Write Full ======"
dd if=/dev/zero of=${BLKDEV}n1 bs=1M count=3000000 status=progress
echo "  PASS"

next_test

echo "NVMe Device FDP Status"
nvme fdp status ${BLKDEV}n1


