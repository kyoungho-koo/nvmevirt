WAF_FILE=$1
PARSE_DATA_FILE=$2
SCRIPT_DIR=$(realpath $(dirname "$0"))

cat $WAF_FILE | grep "HBMW" | cut -d ' ' -f 7 > hbmw.txt
cat $WAF_FILE | grep "MBMW" | cut -d ' ' -f 7 > mbmw.txt
cat $WAF_FILE | grep "MBE" | cut -d ' ' -f 5 > mbe.txt


paste hbmw.txt mbmw.txt mbe.txt > tmp.txt


python3 $SCRIPT_DIR/parse_waf.py tmp.txt > $PARSE_DATA_FILE 

rm tmp.txt
rm hbmw.txt
rm mbmw.txt
rm mbe.txt      
