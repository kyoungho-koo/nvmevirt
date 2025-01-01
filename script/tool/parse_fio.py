import sys
import re

class FioJob:
    def __init__(self, job_name):
        self.job_name = job_name
        self.iops = ""
        self.bw = ""
    def __str__(self):
        return (f"Job Name: {self.job_name} "
                f"IOPS: {self.iops} BW: {self.bw} ")

class FioResultPerSec:
    def __init__(self, sec):
        self.sec = sec
        self.bw = ""
        self.wv = ""
        self.jobs = []
    def __str__(self):
        result = f"sec: {self.sec} Total_BW({self.bw_unit}): {self.bw} Total_WV(GiB): {self.wv:.1f} "
        for job in self.jobs:
            result += f"[Job: {job.job_name} "
            result += f"IOPS: {job.iops} "
            result += f"BW: {job.bw} ] "

        return result



def parse_fio_results(file_path):
    with open(file_path, 'r') as file:
        lines = file.readlines()

    results = []
    jobs = []
    current_result = None
    current_job = None


    sec = 0
    for line in lines:
        if (sec == 0):
            sec += 1
            current_result = FioResultPerSec(sec)
            results.append(current_result)

        if 'groupid' in line:
            job_name = line.split(':')[0].strip()
            current_job = FioJob(job_name)
            current_result.jobs.append(current_job)
        elif 'write:' in line:
            current_job.iops = re.search(r'IOPS=(\d+)', line).group(1)
            kiops = re.search(r'IOPS=([\d.]+k)', line)

            if (kiops is not None):
                current_job.iops = str(float(kiops.group(1)[0:-1]) * 1024)
            
            current_job.bw = re.search(r'BW=(\d+)', line).group(1)
        elif 'WRITE:' in line:
            bw_match = re.search(r'bw=(\d+)([A-Za-z/]+)', line)

            if bw_match:
                current_result.bw = bw_match.group(1)
                current_result.bw_unit = bw_match.group(2)

            wv_match_mib = re.search(r'io=(\d+)(MiB)', line)

            if wv_match_mib:
                current_result.wv = float(wv_match_mib.group(1)) / 1024

            wv_match_gib = re.search(r'io=([\d.]+)(GiB)', line)

            if wv_match_gib:
                current_result.wv = float(wv_match_gib.group(1))

            print(current_result)
            sec += 1
            current_result = FioResultPerSec(sec)



    return jobs



if len(sys.argv) < 2:
    print("Usage: parse_fio.py <fio_result_file_path>")
    sys.exit(1)

file_path = sys.argv[1]
jobs = parse_fio_results(file_path)

for job in jobs:
    print(job)
