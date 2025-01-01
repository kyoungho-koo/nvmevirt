import sys

def parse_input(file_path):
    with open(file_path, 'r') as file:
        lines = file.readlines()

    data = []
    for line in lines:
        if not line.strip().startswith('#') and line.strip():
            numbers = tuple(map(int, line.split()))
            if len(numbers) == 3:
                data.append(numbers)
            else:
                print(f"Skipping invalid line: {line.strip()}")
    return data

def calculate_deltas_from_first(data):
    first_data = data[0]
    changes = []
    for group in data:
        changes.append((group[0] - first_data[0], group[1] - first_data [1], group[2] - first_data[2]))
    return changes


if len(sys.argv) < 2:
    print("Usage: parse_waf.py <waf_file_path>")
    sys.exit(1)
            

file_path = sys.argv[1]
# Parse the input
data = parse_input(file_path)

# Calculate the deltas
deltas_from_first = calculate_deltas_from_first(data)

# For demonstration, printing the calculated deltas
for change in deltas_from_first:
        print(round(change[0]/1000000000), round(change[1]/1000000000), round(change[2]/1000000000))
