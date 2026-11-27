# Runs bedrock_server_mod.exe with a pipe on stdin. Lines appended to cmd.txt are sent to the
# console; all server output goes to out.log. Usage: python -I bds_driver.py <serverdir> <workdir>
import os, subprocess, sys, threading, time

server, work = sys.argv[1], sys.argv[2]
cmd_path = os.path.join(work, "cmd.txt")
out_path = os.path.join(work, "out.log")
open(cmd_path, "w").close()
out = open(out_path, "w", encoding="utf-8", buffering=1)
p = subprocess.Popen([os.path.join(server, "bedrock_server_mod.exe")], cwd=server,
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def pump():
    for raw in p.stdout:
        out.write(raw.decode("utf-8", "replace"))
threading.Thread(target=pump, daemon=True).start()

sent = 0
while p.poll() is None:
    with open(cmd_path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    for line in lines[sent:]:
        out.write(f"> {line}\n")
        p.stdin.write((line + "\n").encode("utf-8"))
        p.stdin.flush()
    sent = len(lines)
    time.sleep(0.3)
out.write(f"[driver] server exited {p.returncode}\n")
