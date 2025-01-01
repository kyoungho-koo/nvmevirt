source ./tool/parameter.sh
while true; do
	nvme fdp stats $BLKDEV -e 1
	sleep 1
done;

	
