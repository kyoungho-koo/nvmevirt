#!/bin/bash


#Namespace SIZE
NSZE=918149526
#NSZE=1024


RUNTIME=10000

#Result File Configuration
DATE=`date +%y%m%d_%H%M%S`
RET_DIR=fio_result/ru_bw_0415/$DATE



#Parsing FDP Device
NVME_LIST=$(nvme list | grep "MZOL63T8HDLT-00AFB" | awk '{print $1, $2}')
#NVME_LIST=$(nvme list | grep "Samsung SSD 970 EVO" | awk '{print $1, $2}')
if [ -z "$NVME_LIST" ]; then
	echo "NVME_LIST is NULL"
	DEV_LIST=("/dev/nvme0" "/dev/nvme1")

	for DEV in "${DEV_LIST[@]}"
	do
		echo $DEV
		nvme delete-ns ${DEV} -n 1
		nvme create-ns ${DEV} -b 4096 --nsze=$NSZE --ncap=$NSZE
		nvme attach-ns ${DEV} --namespace-id=1 --controller=0x7
	done
	NVME_LIST=$(nvme list | grep "MZOL63T8HDLT-00AFB" | awk '{print $1, $2}')
fi

BLKDEV_NS=$(echo $NVME_LIST | cut -d ' ' -f 1)
NGDEV_NS=$(echo $NVME_LIST | cut -d ' ' -f 2)
BLKDEV=${BLKDEV_NS:0:${#BLKDEV_NS}-2}
NGDEV=${NGDEV_NS:0:${#NGDEV_NS}-2}



