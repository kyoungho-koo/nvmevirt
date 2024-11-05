#source ./tool/parameter.sh

NSZE=134217728
RUNTIME=10
DATE=`date +%y%m%d_%H%M%S`
RET_DIR=fio_result/nvmevirt_verifing_0703/$DATE

BLKDEV_NS=/dev/nvme0n1
NGDEV_NS=/dev/ng0n1

BLKDEV=/dev/nvme0
NGDEV=/dev/ng0

PARSE_DATA=$RET_DIR/parse_data

helpFunction() 
{
	echo ""
	echo "Usage: $0 -a parameterA -b parameterB -c parameterC"
	echo -e "\t-a Description of what is parameterA"
	echo -e "\t-b Description of what is parameterB"
	echo -e "\t-c Description of what is parameterC"
	exit 1 # Exit script after printing help
}


while getopts "a:b:c:w:" opt
do
   case "$opt" in
      a ) parameterA="$OPTARG" ;;
      b ) parameterB="$OPTARG" ;;
      c ) parameterC="$OPTARG" ;;
	  w ) WORKLOADS+=("$OPTARG") ;;
      ? ) helpFunction ;; # Print helpFunction in case parameter is non-existent
   esac
done


#echo ============= EXPRIMENT LIST ==============

#echo ${WORKLOADS[0]}
#echo ${WORKLOADS[1]}

#echo ===========================================


mkdir -p $RET_DIR
mkdir -p $PARSE_DATA


#echo
#echo ------------------------------------------
#echo "  EXPR 1: ${WORKLOADS[0]}"
#echo ------------------------------------------
###################################################################
#


echo BLKDEV: ${BLKDEV}
nvme delete-ns ${BLKDEV} -n 1

nvme set-feature ${BLKDEV} -f 0x1D -c 0 -s
nvme set-feature ${BLKDEV} -f 0x1D -c 1 -s

nvme create-ns ${BLKDEV} -b 512 --nsze=$NSZE --ncap=$NSZE -p 0,1,2,3 -n 4
nvme attach-ns ${BLKDEV} --namespace-id=1 --controller=0x7

sleep 10
nvme list
sleep 10

dd if=/dev/zero of=${BLKDEV}n1 bs=1M count=1000 status=progress
sleep 1000
dd if=/dev/zero of=${BLKDEV}n1 bs=1M count=3000000 status=progress
echo "NVMe Device FDP Status"
nvme fdp status ${BLKDEV}n1

WORKLOAD=${WORKLOADS[0]##*/}
echo $WORKLOAD
sed "s|/dev/<NGDEV_NS>|${NGDEV_NS}|g" ${WORKLOADS[0]} > $RET_DIR/$WORKLOAD
sed -i "s|<RUNTIME>|${RUNTIME}|g" $RET_DIR/$WORKLOAD 

cat $RET_DIR/$WORKLOAD

RET_FILE=$RET_DIR/${WORKLOAD:10:${#WORKLOAD}-14}.txt
RET_PARSE_FILE=$PARSE_DATA/${WORKLOAD:10:${#WORKLOAD}-14}
#sudo bash ./tool/waf.sh > $RET_DIR/${WORKLOAD:10:${#WORKLOAD}-14}.waf &
#WAF_PID=$!
fio $RET_DIR/${WORKLOAD} --status-interval=1 > $RET_FILE
python3 ./tool/parse_fio.py $RET_FILE > ${RET_PARSE_FILE}.txt
#kill -9 $WAF_PID
#./tool/parse_waf.sh $RET_DIR/${WORKLOAD:10:${#WORKLOAD}-14}.waf $PARSE_DATA/${WORKLOAD:10:${#WORKLOAD}-14}.waf

#grep 'WRITE:' $RET_FILE | sed 's/bw=//' | sed 's/MB\/s/ MB\/s/' |sed 's/(//'  |sed 's/),//' > ${RET_PARSE_FILE}-wbw.dat
#grep 'READ:' $RET_FILE | sed 's/bw=//' | sed 's/MB\/s/ MB\/s/' |sed 's/(//'  |sed 's/),//' > ${RET_PARSE_FILE}-rbw.dat


