from time import sleep
from datetime import datetime, timedelta
from collections import defaultdict
from subprocess import check_output

now = datetime.now()

day = timedelta(seconds=1)

x = check_output(["ssh","hwsec", "nodeman", "queue"]).decode()
print(x)

if "dgl800" in x:
	print("[!] Already reserved.")
	lines = [datetime.strptime(i.split("\t")[-1], "%d/%m/%y %H:%M")-now for i in x.splitlines() if "dgl800" in i]
	sleep_time = lines[0]
else:
	res = [(i.split()[0], datetime.strptime(i.split("\t")[-1], "%d/%m/%y %H:%M")-now) for i in x.split("\n") if len(i.split())>2 and ":" in i.split()[-1]]

	nodes = defaultdict(lambda: day)
	for i in sorted(res, key=lambda x: x[1]):
		if nodes[i[0]] < i[1]:
			nodes[i[0]] = i[1]

	next_slot = sorted(nodes.items(), key=lambda x: x[1])[0][0]
	print("[=] Reserving Next Slot:", next_slot)

	check_output(["ssh","hwsec", "nodeman", "reserve", next_slot, "20"]).decode()

last = ""

while True:
	try:
		x = check_output(["ssh","hwsec", "nodeman", "queue"]).decode()
		lines = [(i.split()[0], datetime.strptime(i.split("\t")[-1], "%d/%m/%y %H:%M")-now) for i in x.splitlines() if "dgl800" in i]
		sleep_time = lines[0][1] - timedelta(minutes=20)
		host = lines[0][0]
	except:
		print("[!] Failed to check.")
	startat = (now+sleep_time).strftime("%H:%M")
	if startat != last:
		if not last:
			print("[%] Waiting until", startat, "for", host)
		else:
			print("[!] Hey hey! Moved up from", last, "to", startat)

	if sleep_time.seconds<60:
		sleep(sleep_time.seconds+59)
		for i in range(8):
			check_output(["notify-send", f"Reservation Active on {host}!"])

	else:
		sleep(60)
	last = startat
