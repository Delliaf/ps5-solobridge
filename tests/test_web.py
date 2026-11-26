import http.client
import json
import pathlib
import socket
import subprocess
import sys
import time
root=pathlib.Path(sys.argv[1])
port=int(sys.argv[2])
binary=sys.argv[3]
(root/"log.txt").write_text("test log\n")
p=subprocess.Popen([binary],stdout=subprocess.PIPE,text=True)
try:
    assert p.stdout.readline().strip()=="ready"
    assert not (root/"web_key").exists()
    def req(method,path,action=True,expected=200,extra=None):
        c=http.client.HTTPConnection("127.0.0.1",port,timeout=4)
        h={"X-TTGK-Action":"1"} if action else {}
        if extra:h.update(extra)
        c.request(method,path,headers=h)
        r=c.getresponse();data=r.read();c.close()
        assert r.status==expected,(path,r.status,data)
        return data
    page=req("GET","/",False)
    assert b"TTGK Audio Bridge" in page and b"X-TTGK-Key" not in page
    assert b"TTGK Web OK" in req("GET","/health",False)
    assert b"listening" in req("GET","/api/web/status",False)
    req("POST","/api/volume/128",False,403)
    req("GET","/api/status",False,403,{"Host":"attacker.example"})
    req("POST","/api/volume/128",True,403,{"Origin":"http://attacker.example"})
    s=json.loads(req("GET","/api/status",False))
    assert s["hw"]==68 and s["version"]=="2.3.0" and s["fresh"]==1 and "pad" not in s
    req("POST","/api/volume/108")
    assert json.loads(req("GET","/api/status",False))["desired"]==108
    req("POST","/api/volume/up")
    assert json.loads(req("GET","/api/status"))["desired"]==112
    req("POST","/api/volume/toggle")
    assert json.loads(req("GET","/api/status"))["desired"]==368
    req("POST","/api/volume/149",True,409)
    req("GET","/api/volume/68",True,404)
    assert req("GET","/api/log",False)==b"test log\n"
    req("GET","/api/log/../../web_key",True,404)
    req("POST","/api/pacing/legacy")
    assert json.loads(req("GET","/api/status"))["economy"]==0
    req("POST","/api/pacing/economy")
    assert json.loads(req("GET","/api/status"))["economy"]==1
    req("POST","/api/benchmark/start")
    time.sleep(.1)
    assert json.loads(req("GET","/api/status"))["benchmark_running"]==1
    req("POST","/api/benchmark/start",True,409)
    req("POST","/api/pacing/legacy",True,409)
    req("POST","/api/web/disable",True,409)
    assert not (root/"disable_web").exists()
    assert b"mock result" in req("GET","/api/benchmark/report")
    req("POST","/api/benchmark/cancel")
    time.sleep(.1)
    assert b"CANCELLED" in req("GET","/api/benchmark/report")
    assert json.loads(req("GET","/api/status"))["benchmark_running"]==0
    req("POST","/api/notification/test")
    req("POST","/api/trace/on",True,404)
    with socket.create_connection(("127.0.0.1",port),timeout=2) as slow:
        slow.sendall(b"GET /")
        time.sleep(1.4)
    req("GET","/api/status")
    req("POST","/api/web/disable")
    assert p.wait(timeout=4)==0
    assert (root/"disable_web").exists()
    assert subprocess.run([binary],timeout=4).returncode==0
    (root/"disable_web").unlink()
    # Occupied primary port -> fallback; no key or notification required.
    with socket.socket() as blocker:
        blocker.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        blocker.bind(("0.0.0.0",port));blocker.listen()
        p=subprocess.Popen([binary],stdout=subprocess.PIPE,text=True)
        assert p.stdout.readline().strip()=="ready"
        port+=1
        assert b"listening" in req("GET","/api/web/status",False)
        req("POST","/api/web/off")
        assert p.wait(timeout=4)==0
        assert not (root/"disable_web").exists()
        port-=1
    p=subprocess.Popen([binary],stdout=subprocess.PIPE,text=True)
    assert p.stdout.readline().strip()=="ready"
    req("POST","/api/stop")
    assert p.wait(timeout=4)==2
    # No incoming connections: shutdown must wake an idle select.
    p=subprocess.Popen([binary],stdout=subprocess.PIPE,text=True)
    assert p.stdout.readline().strip()=="ready"
    p.terminate()
    assert p.wait(timeout=4)==0
    print("web no-key/CSRF/Host/volume/log/timeout/fallback/disable/idle-stop tests passed")
finally:
    if p.poll() is None:p.kill();p.wait()
