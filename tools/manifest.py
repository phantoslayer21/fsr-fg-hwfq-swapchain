"""Turn `wevtutil gp Microsoft-Windows-DxgKrnl /ge:true /gm:true /f:xml` output into a TSV of
event id -> version, task, opcode, keywords, template fields. Usage: manifest.py <manifest.xml> <out.tsv> [regex]"""
import re
import sys
import xml.etree.ElementTree as ET

NS = {"e": "http://schemas.microsoft.com/win/2004/08/events"}


def main():
    root = ET.parse(sys.argv[1]).getroot()
    tasks = {t.get("value"): t.get("name") for t in root.iterfind(".//e:tasks/e:task", NS)}
    opcodes = {o.get("value"): o.get("name") for o in root.iterfind(".//e:opcodes/e:opcode", NS)}
    templates = {}
    for t in root.iterfind(".//e:templates/e:template", NS):
        templates[t.get("tid")] = [d.get("name") for d in t.iterfind("e:data", NS)]
    rows = []
    for ev in root.iterfind(".//e:events/e:event", NS):
        rows.append((int(ev.get("value")), ev.get("version"), tasks.get(ev.get("task"), ev.get("task")),
                     opcodes.get(ev.get("opcode"), ev.get("opcode")), ev.get("keywords"),
                     ",".join(templates.get(ev.get("template"), []))))
    pat = re.compile(sys.argv[3], re.I) if len(sys.argv) > 3 else None
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write("id\tver\ttask\topcode\tkeywords\tfields\n")
        for r in sorted(rows):
            line = "\t".join(str(x) for x in r)
            f.write(line + "\n")
            if pat and pat.search(line):
                print(line[:400])


if __name__ == "__main__":
    main()
