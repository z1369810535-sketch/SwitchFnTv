"""Validate detail XML and bindings without needing a GPU or Switch."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent.parent
for kind in ("movie", "series"):
    xml = (root / f"resources/xml/tabs/{kind}.xml").read_text(encoding="utf-8")
    tree = ET.fromstring(xml.replace("brls:", "brls_"))
    ids = [element.attrib["id"] for element in tree.iter() if "id" in element.attrib]
    assert len(ids) == len(set(ids)), f"Duplicate ID in {kind} layout"
    header = (root / f"app/include/tab/media_{kind}.hpp").read_text(encoding="utf-8")
    for bound in re.findall(r'BRLS_BIND\([^"\n]+"([^"]+)"', header):
        assert bound in ids, f"Missing view for binding {bound}"
    print(f"PASS XML and all bindings: {kind}")
