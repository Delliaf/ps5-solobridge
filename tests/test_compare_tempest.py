import importlib.util
import pathlib
import tempfile
import wave
import numpy as np

p=pathlib.Path(__file__).resolve().parents[1]/"tools"/"compare_tempest.py"
spec=importlib.util.spec_from_file_location("compare_tempest",p)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

rng=np.random.default_rng(42)
x=rng.normal(0,0.06,(48000*4,2))
r=m.compare(x,x)
assert r["correlation"]>0.99999 and r["normalized_residual_db"]<-100
y=np.zeros_like(x);y[2400:]=x[:-2400]*0.5
r=m.compare(x,y)
assert abs(r["lag_ms"]-50)<0.1 and r["correlation"]>0.9999
assert abs(r["level_difference_db"]+6.0206)<0.01
assert m.compare(x,np.zeros_like(x))["comparable"] is False
z=x.copy();z[:,1]=0.7*x[:,1]+0.3*x[:,0]
assert m.compare(x,z)["normalized_residual_db"]>-30
with tempfile.TemporaryDirectory() as d:
    d=pathlib.Path(d)
    for name,value in (("off_a",x),("off_b",x),("on_a",z),("on_b",z)):
        with wave.open(str(d/(name+".wav")),"wb") as w:
            w.setparams((2,2,48000,0,"NONE","not compressed"))
            w.writeframes((value*32768).astype("<i2").tobytes())
    result=m.report(d)
    assert result["comparisons"]["off_repeat"]["correlation"]>0.99999
    assert result["comparisons"]["off_vs_on_a"]["normalized_residual_db"]>-30
print("Tempest analyzer synthetic alignment and repeat tests passed")
