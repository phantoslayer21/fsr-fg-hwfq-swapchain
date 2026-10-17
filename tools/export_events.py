"""Export the flip-related DxgKrnl events of an ETW capture to one CSV, all fields as decoded by tracerpt.
Usage: export_events.py <capture.etl> <out.csv>
Columns: event_id, event_name, pid, time_filetime (100 ns, wall clock as recorded), time_qpc (same instant in QPC units,
via the median offset between event 505 records and their CompletionTimeStamp), then every decoded field as name=value."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import etw_flips as E  # noqa: E402

NAMES = {"184": "Present", "266": "IndependentFlip", "273": "VSyncDPCMultiPlane", "386": "MMIOFlipMultiPlaneOverlay3",
         "505": "VSyncHwFlipQueueLogUpdate", "518": "SelectPresentAtFlip", "521": "CancelHwQueuedFlips",
         "530": "SetInterruptTargetPresentId", "268": "CancelFlip", "500": "CancelPresentAtFlips"}


def main():
    etl, out = sys.argv[1], sys.argv[2]
    xml = out + ".tmp.xml"
    subprocess.run(["tracerpt", etl, "-o", xml, "-of", "XML", "-y"], capture_output=True)
    E.WANT |= set(NAMES)
    ev = E.parse(xml)
    os.remove(xml)
    off = E.clock_offset(ev)
    rows = []
    for eid, lst in ev.items():
        for e in lst:
            fields = [f"{k}={v.strip()}" for k, v in e.items() if not k.startswith("_") and not k.endswith("[]")]
            for k, v in e.items():
                if k.endswith("[]"):
                    fields.append(f"{k[:-2]}=[{' '.join(x.strip() for x in v)}]")
            rows.append((e["_t"], eid, e["_pid"], fields))
    rows.sort()
    with open(out, "w", encoding="utf-8") as f:
        f.write("event_id,event_name,pid,time_filetime,time_qpc,fields\n")
        for t, eid, pid, fields in rows:
            q = t - off if off is not None else ""
            f.write(f"{eid},{NAMES.get(eid, eid)},{pid},{t},{q},\"{'; '.join(fields)}\"\n")
    print(f"{out}: {len(rows)} events")


if __name__ == "__main__":
    main()
