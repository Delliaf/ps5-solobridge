import sys
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from measure_latency import measure
rng=np.random.default_rng(42);rate=48000
a=rng.normal(0,.1,rate*5)
for lag in (480,-384,0):
    b=np.zeros_like(a)
    if lag>0:b[lag:]=a[:-lag]
    elif lag<0:b[:lag]=a[-lag:]
    else:b[:]=a
    r=measure(np.column_stack([a,b]),rate)
    assert r["valid"] and abs(r["median_ms"]-lag*1000/rate)<.001
assert not measure(np.zeros((rate*5,2)),rate)["valid"]
print("Simultaneous-reference latency analyzer tests passed")
