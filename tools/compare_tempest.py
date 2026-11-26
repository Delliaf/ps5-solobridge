#!/usr/bin/env python3
"""Compare controlled, repeatable stereo PCM captures. Requires numpy.
This reports signal differences, never asserts that a difference is Tempest.
"""
import argparse
import json
import wave
from pathlib import Path
import numpy as np

def load(path):
    with wave.open(str(path), "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (48000, 2, 2):
            raise ValueError(f"{path}: expected 48 kHz stereo PCM16")
        if w.getnframes() > 48000 * 21:
            raise ValueError(f"{path}: exceeds diagnostic duration")
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64).reshape(-1, 2) / 32768

def db(x):
    return float(20*np.log10(max(float(x),1e-12)))

def stats(x):
    mid=(x[:,0]+x[:,1])*0.5
    side=(x[:,0]-x[:,1])*0.5
    rms=lambda z: np.sqrt(np.mean(z*z))
    return {"rms_dbfs":db(rms(x)), "peak_dbfs":db(np.max(np.abs(x))),
            "side_mid_db":db(rms(side)/max(rms(mid),1e-12)),
            "clipped_samples":int(np.count_nonzero(np.abs(x)>=32767/32768))}

def compare(x,y):
    # Find global alignment from a sum of per-channel correlations (FFT).
    # Bounded lag; silence or non-repeatable content remains inconclusive.
    n=min(len(x),len(y))
    if n<48000*3: raise ValueError("need at least three seconds")
    x=x[:n];y=y[:n]
    if min(np.sqrt(np.mean(x*x)),np.sqrt(np.mean(y*y))) < 0.0001:
        return {"comparable":False,"reason":"silent or very quiet"}
    z=1<<(2*n-1).bit_length()
    corr=np.fft.irfft(np.sum(np.conj(np.fft.rfft(x,z,axis=0))*np.fft.rfft(y,z,axis=0),axis=1),z)
    m=min(48000,n//4)
    lags=np.arange(-m,m+1)
    lag=int(lags[np.argmax(np.abs(corr[lags%z]))])
    a=x[max(0,-lag):n-max(0,lag)]
    b=y[max(0,lag):n-max(0,-lag)]
    a=a-a.mean(axis=0); b=b-b.mean(axis=0)
    ea=float(np.sum(a*a)); eb=float(np.sum(b*b))
    if min(ea,eb)<1e-12: return {"comparable":False,"reason":"no varying signal"}
    dot=float(np.sum(a*b))
    gain=dot/ea
    error=np.sqrt(np.sum((b-gain*a)**2)/eb)
    return {"comparable":True,"lag_ms":lag/48,"correlation":dot/np.sqrt(ea*eb),
            "normalized_residual_db":db(error),"level_difference_db":db(np.sqrt(eb/ea))}

def report(folder,prefix=""):
    names=[prefix+s for s in ("off_a","off_b","on_a","on_b")]
    x=[load(folder/(s+".wav")) for s in names]
    pairs={"off_repeat":compare(x[0],x[1]),"on_repeat":compare(x[2],x[3]),
           "off_vs_on_a":compare(x[0],x[2]),"off_vs_on_b":compare(x[1],x[3])}
    return {"files":dict(zip(names,map(stats,x))),"comparisons":pairs,
            "note":"Compare cross-setting differences with both same-setting repeats. "
                   "Content, timing, level and routing differences are confounders. "
                   "A changed capture does not prove Tempest; an unchanged capture "
                   "does not prove absence for all content. Use a matched HDMI reference if possible."}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("folder",type=Path)
    p.add_argument("--measurement",action="store_true",help="compare Apply Measurement Results OFF/ON")
    p.add_argument("--calibration",action="store_true",help="report signal level in native calibration capture")
    a=p.parse_args()
    if a.calibration:
        x=load(a.folder/"calibration.wav")
        result={"capture":stats(x),"one_second_rms_dbfs":[stats(x[i:i+48000])["rms_dbfs"] for i in range(0,len(x),48000)],
                "note":"Signal presence is not proof of calibration tones. Verify audible tones from CX31993 alone."}
    else: result=report(a.folder,"measurement_" if a.measurement else "")
    print(json.dumps(result,indent=2))
if __name__=="__main__":
    main()
