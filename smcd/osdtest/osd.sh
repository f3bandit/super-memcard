#!/bin/sh
# osd.sh: the test Main's OSD rows as text (its phosphor-main.sock "osdtext"; the test-main branch). "osd False": the
# rows are the last ones shown, the OSD is closed
python3 -c "
import socket,json; s=socket.socket(socket.AF_UNIX); s.connect('/tmp/phosphor-main.sock'); s.settimeout(3); s.sendall(b'osdtext\n')
j=json.loads(s.recv(4000).decode()); print('osd', j['osd']); [print(r) for r in j['rows'].split('|')]"
