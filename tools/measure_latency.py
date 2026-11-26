#!/usr/bin/env python3
"""Measure *relative* delay from simultaneous reference/DAC mono channels.
Input stereo PCM16 WAV: left=HDMI reference, right=TTGK; shared ADC clock.
Use nonperiodic sharp transients or broadband noise, not continuous tones.
Requires numpy. Positive delay means TTGK is later than reference.
"""
import argparse, json, wave
import numpy as np

def measure(x, rate, max_ms=200):
    if x.ndim != 2 or x.shape[1] != 2 or len(x) < rate*3:
        raise ValueError("need >=3 seconds of two simultaneous channels")
    n=rate
    m=min(n//3, round(rate*max_ms/1000))
    if m<1: raise ValueError("max lag too small")
    z=1<<(2*n-1).bit_length()
    lags=np.arange(-m,m+1)
    results=[]
    for i in range(0,len(x)-n+1,n):
        a,b=x[i:i+n].astype(np.float64).T
        a-=a.mean();b-=b.mean()
        if min(np.sqrt(np.mean(a*a)),np.sqrt(np.mean(b*b)))<1e-4:
            continue
        corr=np.fft.irfft(np.conj(np.fft.rfft(a,z))*np.fft.rfft(b,z),z)
        v=np.abs(corr[lags%z]);k=int(np.argmax(v));lag=int(lags[k])
        if abs(lag)==m:continue
        aa=a[max(0,-lag):n-max(0,lag)]
        bb=b[max(0,lag):n-max(0,-lag)]
        den=np.linalg.norm(aa)*np.linalg.norm(bb)
        if den<=0:continue
        coherence=abs(float(np.dot(aa,bb)/den))
        # Reject repetitive or ambiguous peaks more than 1 ms away.
        mask=np.abs(lags-lag)>max(1,rate//1000)
        second=float(v[mask].max()) if mask.any() else 0
        if coherence<0.7 or second>0.95*v[k]:continue
        results.append({"second":i/rate,"delay_ms":lag*1000/rate,"correlation":coherence})
    if len(results)<3:
        return {"valid":False,"windows":results,"note":"Need at least 3 unambiguous active windows. Use broadband transient content."}
    delays=np.array([r["delay_ms"] for r in results])
    return {"valid":True,"median_ms":float(np.median(delays)),
            "p05_ms":float(np.percentile(delays,5)),"p95_ms":float(np.percentile(delays,95)),
            "min_ms":float(delays.min()),"max_ms":float(delays.max()),
            "windows":results,"note":"Relative path delay only, not button-to-sound or display latency. Reference/extractor delay is included in the comparison."}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("wav");p.add_argument("--max-ms",type=float,default=200)
    a=p.parse_args()
    if not 1<=a.max_ms<=300: p.error("--max-ms must be 1..300")
    with wave.open(a.wav,"rb") as w:
        if w.getnchannels()!=2 or w.getsampwidth()!=2:raise ValueError("stereo PCM16 required")
        if w.getnframes()>w.getframerate()*120:raise ValueError("max 120 seconds")
        x=np.frombuffer(w.readframes(w.getnframes()),dtype="<i2").reshape(-1,2)/32768.0
        r=measure(x,w.getframerate(),a.max_ms)
    print(json.dumps(r,indent=2))
if __name__=="__main__":main()
