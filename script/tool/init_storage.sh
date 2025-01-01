SCRIPT_DIR=$(realpath $(dirname "$0"))

source $SCRIPT_DIR/parameter.sh

PARTITION_NSZE=$((1024))
BS=$((4096))

echo NSZE $PARTITION_NSZE
echo BS $BS


nvme delete-ns ${BLKDEV} -n 1
nvme set-feature ${BLKDEV} -f 0x1D -c 0 -s
nvme set-feature ${BLKDEV} -f 0x1D -c 1 -s
nvme create-ns ${BLKDEV} -b ${BS} --nsze=$PARTITION_NSZE --ncap=$PARTITION_NSZE -p 0,1,2,3 -n 4
nvme attach-ns ${BLKDEV} --namespace-id=1 --controller=0x7
nvme fdp status ${BLKDEV_NS}

nvme list | grep "MZOL63"
sleep 10
