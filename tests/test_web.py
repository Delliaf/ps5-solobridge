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
    key=(root/"web_key").read_text().strip()
    assert len(key)==16
    def req(method,path,auth=True,expected=200,extra=None):
        c=http.client.HTTPConnection("127.0.0.1",port,timeout=4)
        h={"X-TTGK-Key":key} if auth else {}
        if extra:h.update(extra)
        c.request(method,path,headers=h)
        r=c.getresponse();data=r.read();c.close()
        assert r.status==expected,(path,r.status,data)
        return data
    assert b"TTGK Audio Bridge" in req("GET","/",False)
    req("GET","/api/status",False,401)
    req("POST","/api/volume/128",False,401)
    req("GET","/api/status",True,401,{"X-TTGK-Key":"incorrect"})
    s=json.loads(req("GET","/api/status"))
    assert s["hw"]==68 and s["pad"]["buttons"]==0x404
    req("POST","/api/volume/108")
    assert json.loads(req("GET","/api/status"))["desired"]==108
    req("POST","/api/volume/up")
    assert json.loads(req("GET","/api/status"))["desired"]==112
    req("POST","/api/volume/toggle")
    assert json.loads(req("GET","/api/status"))["desired"]==368
    req("POST","/api/volume/149",True,409)
    req("GET","/api/volume/68",True,404)
    assert req("GET","/api/log")==b"test log\n"
    req("GET","/api/log/../../web_key",True,404)
    req("POST","/api/trace/on")
    req("POST","/api/trace/off")
    # Slow client is bounded; next request still succeeds.
    with socket.create_connection(("127.0.0.1",port),timeout=2) as slow:
        slow.sendall(b"GET /")
        time.sleep(1.4)
    req("GET","/api/status")
    req("POST","/api/web/disable")
    assert p.wait(timeout=4)==0
    assert (root/"disable_web").exists()
    assert subprocess.run([binary],timeout=4).returncode==0
    (root/"disable_web").unlink()
    p=subprocess.Popen([binary],stdout=subprocess.PIPE,text=True)
    assert p.stdout.readline().strip()=="ready"
    key=(root/"web_key").read_text().strip()
    req("POST","/api/stop")
    assert p.wait(timeout=4)==2
    print("web HTTP/auth/volume/log/timeout/disable/stop tests passed")
finally:
    if p.poll() is None:p.kill();p.wait()
