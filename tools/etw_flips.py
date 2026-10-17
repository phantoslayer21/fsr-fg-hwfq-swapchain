"""Per-present flip timing from a DxgKrnl ETW capture decoded with `tracerpt <etl> -of XML`.

Chain (field names as decoded on this machine, Windows 26200):
  IndependentFlip (266): hContext (the swapchain's context; ProcessID on this event is not reliable), SubmitSequence,
      FlipInterval, PresentAtQpc
  MMIOFlipMultiPlaneOverlay3 (386, = DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3): FlipSubmitSequence,
      PresentId, LayerIndex, InputFlags, TargetFlipTimeQpc, ThisFlipEarliestIdealTimeQpc
  VSyncHwFlipQueueLogUpdate (505, hardware flip queue log): PlaneId, PresentId, CompletionTimeStamp (QPC)
  VSyncDPCMultiPlane (273): per-plane PresentIdOrPhysicalAddress scanned at this vsync
Event times are wall clock (FILETIME ticks); they are mapped to QPC with the median offset between 505 events and
their CompletionTimeStamp (the vsync DPC runs a few tens of microseconds after the vblank).
Display time of a flip: its 505 CompletionTimeStamp if logged; else, for an immediate (tearing) flip, the time of
its 386 call; else the first vsync (273) that scans it.
Usage as a script: etw_flips.py <xml>  -> prints counts and a few matched rows."""
import bisect
import datetime
import re
import sys

WANT = {"266", "273", "386", "505", "268", "500", "518", "521"}
RX_ID = re.compile(r"<EventID>(\d+)</EventID>")
RX_TIME = re.compile(r'SystemTime="([^"]+)"')
RX_PID = re.compile(r'ProcessID="(\d+)"')
RX_DATA = re.compile(r'<Data Name="([^"]+)">([^<]*)</Data>')
EPOCH = datetime.datetime(1601, 1, 1, tzinfo=datetime.timezone.utc)


def num(v):
    v = v.strip()
    if not v:
        return 0
    return int(v, 16) if v.lower().startswith("0x") else int(v)


def ticks(s):
    """'2026-10-16T23:03:33.287965900-04:00' -> 100 ns ticks since 1601 (FILETIME)."""
    m = re.match(r"(.*T\d\d:\d\d:\d\d)\.(\d{7})\d*(.*)", s)
    base = datetime.datetime.fromisoformat(m.group(1) + m.group(3))
    return int((base - EPOCH).total_seconds()) * 10_000_000 + int(m.group(2))


def parse(xml_path):
    ev = {k: [] for k in WANT}
    buf, keep = None, False
    with open(xml_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.lstrip().startswith("<Event ") or line.lstrip().startswith("<Event>"):
                buf, keep = [line], False
                continue
            if buf is None:
                continue
            buf.append(line)
            if "<EventID>" in line:
                m = RX_ID.search(line)
                keep = bool(m and m.group(1) in WANT)
            if "</Event>" in line:
                if keep:
                    txt = "".join(buf)
                    pm = RX_PID.search(txt)
                    d = {"_id": RX_ID.search(txt).group(1), "_t": ticks(RX_TIME.search(txt).group(1)), "_pid": int(pm.group(1)) if pm else -1}
                    for k, v in RX_DATA.findall(txt):
                        if k in d:
                            d.setdefault(k + "[]", [d[k]]).append(v)
                        else:
                            d[k] = v
                    ev[d["_id"]].append(d)
                buf = None
    return ev


def main_context(ev):
    """The swapchain's DxgKrnl context: the one with the most IndependentFlip events."""
    counts = {}
    for e in ev["266"]:
        counts[e["hContext"]] = counts.get(e["hContext"], 0) + 1
    return (max(counts, key=counts.get) if counts else None), counts


def clock_offset(ev):
    d = sorted(e["_t"] - num(e["CompletionTimeStamp"]) for e in ev["505"])
    return d[len(d) // 2] if d else None


def flips_for_context(ev, ctx):
    off = clock_offset(ev)
    by_seq = {}
    for e in ev["386"]:
        by_seq.setdefault(num(e["FlipSubmitSequence"]), e)
    done = {}
    for e in ev["505"]:
        done.setdefault((num(e["PlaneId"]), num(e["PresentId"])), num(e["CompletionTimeStamp"]))
    vs_t = [e["_t"] - off for e in ev["273"]] if off is not None else []
    vs_ids = [[num(x) for x in e.get("PresentIdOrPhysicalAddress[]", [e.get("PresentIdOrPhysicalAddress", "0")])]
              for e in ev["273"]]
    rows = []
    for e in ev["266"]:
        if e["hContext"] != ctx:
            continue
        r = dict(seq=num(e["SubmitSequence"]), interval=num(e["FlipInterval"]), present_at=num(e.get("PresentAtQpc", "0")),
                 call=None, target=None, earliest=None, layer=None, flip_id=None, inflags=None, disp=None, how=None)
        m = by_seq.get(r["seq"])
        if m and off is not None:
            r.update(call=m["_t"] - off, target=num(m["TargetFlipTimeQpc"]), earliest=num(m["ThisFlipEarliestIdealTimeQpc"]),
                     layer=num(m["LayerIndex"]), flip_id=num(m["PresentId"]), inflags=num(m["InputFlags"]))
            c = done.get((r["layer"], r["flip_id"]))
            if c:
                r.update(disp=c, how="hwlog")
            elif r["interval"] == 0:
                r.update(disp=r["call"], how="immediate")
            else:
                n = bisect.bisect_left(vs_t, r["call"])
                while n < len(vs_t):
                    ids = vs_ids[n]
                    if r["layer"] < len(ids) and ids[r["layer"]] >= r["flip_id"]:
                        r.update(disp=vs_t[n], how="vsync")
                        break
                    n += 1
        rows.append(r)
    return rows


def display_times(xml_path, hdr, P):
    """dict j -> display QPC for the logged presents (the last len(P) independent flips of the swapchain context)."""
    ev = parse(xml_path)
    ctx, _ = main_context(ev)
    rows = [r for r in flips_for_context(ev, ctx) if r["call"] is not None]
    rows.sort(key=lambda r: r["call"])
    # Match each logged present to the first flip of the swapchain context whose DDI call comes after the
    # Present() call (the flip is submitted after the present; immediate flips within ~0.2 ms).
    disp = {}
    how = {}
    k = 0
    for p in sorted(P, key=lambda p: p["call"]):
        while k < len(rows) and rows[k]["call"] < p["call"] - 2000:  # 0.2 ms slack for the clock mapping
            k += 1
        if k >= len(rows):
            break
        r = rows[k]
        k += 1
        how[r["how"]] = how.get(r["how"], 0) + 1
        if r["disp"]:
            disp[p["j"]] = r["disp"]
    extra = dict(etw_context=ctx, iflips=len(rows), display_source=how,
                 present_at_nonzero=sum(1 for r in rows if r["present_at"]),
                 cancel_events={k: len(ev[k]) for k in ("268", "500", "521")}, select_present_at=len(ev["518"]))
    return disp, extra


if __name__ == "__main__":
    ev = parse(sys.argv[1])
    ctx, counts = main_context(ev)
    print("events:", {k: len(v) for k, v in ev.items()}, "contexts:", counts, "using", ctx, "offset", clock_offset(ev))
    rows = flips_for_context(ev, ctx)
    for r in rows[200:206]:
        print(r)
