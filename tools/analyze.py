"""Pacing analysis of one pace_test log (+ optional ETW CSV). Usage: analyze.py <results/run.csv> [--etw etw/run.csv]

Display time per present:
  comp runs: IIndependentFlipFramePresentStatistics displayed time (interrupt time -> QPC via the header offsets);
  dxgi runs: from the ETW capture (see etw_display_times), when given.
Prints one summary line (JSON) with interval jitter vs the ideal output period, error vs the schedule, and latency."""
import json
import sys

import os

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def load(path):
    hdr, P, S, D = {}, [], [], []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line.startswith("# "):
            for kv in (t for t in line[2:].split() if "=" in t):
                k, v = kv.split("=")
                hdr[k] = v
        elif line.startswith("P,"):
            f = line.split(",")
            P.append(dict(j=int(f[1]), k=int(f[2]), i=int(f[3]), real=int(f[4]), sched=int(f[5]), call=int(f[6]),
                          ret=int(f[7]), id=int(f[8]), werr=float(f[9])))
        elif line.startswith("S,"):
            f = line.split(",")
            S.append(dict(id=int(f[1]), kind=int(f[2]), status=int(f[3]), comp=int(f[4]), disp=int(f[5]), dur=int(f[6])))
        elif line.startswith("D,"):
            f = line.split(",")
            D.append(f)
    return hdr, P, S, D


def summarize(name, hdr, P, disp_qpc, freq):
    """disp_qpc: dict j -> displayed QPC. Returns summary dict."""
    ms = 1000.0 / freq
    refresh = float(hdr["refresh_hz"])
    mult = int(hdr["mult"])
    js = sorted(j for j in disp_qpc)
    out = dict(run=name, api=hdr["api"], mode=hdr["mode"], mult=mult, load=int(hdr.get("load", 0)),
               presents=len(P), displayed=len(js))
    if len(js) < 10:
        return out
    t_out = np.median(np.diff([p["sched"] for p in P])) * ms
    d = np.array([disp_qpc[j] for j in js], dtype=np.float64)
    iv = np.diff(d) * ms
    # Only consecutive displayed frames count as intervals; a gap in j is a dropped/merged frame.
    cons = np.diff(np.array(js)) == 1
    ivc = iv[cons]
    dev = np.abs(ivc - t_out)
    sched = {p["j"]: p["sched"] for p in P}
    err = np.array([(disp_qpc[j] - sched[j]) * ms for j in js])
    err_c = err - np.median(err)
    period = 1000.0 / refresh
    out.update(
        t_out_ms=round(float(t_out), 4), refresh_hz=round(refresh, 2),
        interval_mean_ms=round(float(ivc.mean()), 4), interval_sd_ms=round(float(ivc.std()), 4),
        absdev_mean_ms=round(float(dev.mean()), 4), absdev_p95_ms=round(float(np.percentile(dev, 95)), 4),
        absdev_p99_ms=round(float(np.percentile(dev, 99)), 4), absdev_max_ms=round(float(dev.max()), 4),
        # share of intervals more than half a refresh away from ideal (visible stutter)
        bad_intervals_pct=round(float((dev > period / 2).mean() * 100), 2),
        dropped=len(P) - len(js),
        sched_err_median_ms=round(float(np.median(err)), 4), sched_err_sd_ms=round(float(err_c.std()), 4),
        sched_err_p99_abs_ms=round(float(np.percentile(np.abs(err_c), 99)), 4),
        present_call_ms_p99=round(float(np.percentile([(p["ret"] - p["call"]) * ms for p in P], 99)), 4),
    )
    return out


def comp_display_times(hdr, P, S):
    freq = int(hdr["qpc_freq"])
    q0, i0 = int(hdr["qpc0"]), int(hdr["interrupt0"])
    per100 = freq / 1e7
    disp_by_id = {}
    kinds = {}
    for s in S:
        kinds[s["kind"]] = kinds.get(s["kind"], 0) + 1
        if s["kind"] == 3 and s["disp"]:
            disp_by_id[s["id"]] = s["disp"]  # GetDisplayedTime() is in QPC units on this system (measured)
    status = {}
    for s in S:
        if s["kind"] == 1:
            status[s["status"]] = status.get(s["status"], 0) + 1
    disp = {p["j"]: disp_by_id[p["id"]] for p in P if p["id"] in disp_by_id}
    return disp, kinds, status


def main():
    path = sys.argv[1]
    hdr, P, S, D = load(path)
    freq = int(hdr["qpc_freq"])
    extra = {}
    if hdr["api"] == "comp":
        disp, kinds, status = comp_display_times(hdr, P, S)
        extra = dict(stat_kinds=kinds, present_status=status)
    else:
        disp = {}
        if "--etw" in sys.argv:
            import etw_flips
            disp, extra = etw_flips.display_times(sys.argv[sys.argv.index("--etw") + 1], hdr, P)
    s = summarize(path.replace("\\", "/").split("/")[-1][:-4], hdr, P, disp, freq)
    s.update(extra)
    print(json.dumps(s))


if __name__ == "__main__":
    main()
