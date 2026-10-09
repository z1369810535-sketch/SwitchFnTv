"""Static checks for layouts, view bindings and translation keys (no GPU or Switch needed).

- every XML layout is well formed;
- every BRLS_BIND id exists in the layouts the class inflates;
- every i18n key used by C++ code or layouts exists in the Simplified Chinese and English files.
"""
from pathlib import Path
import json
import re
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent.parent
failures = []


def layout_ids(*names):
    ids = set()
    for name in names:
        text = (root / "resources/xml" / name).read_text(encoding="utf-8-sig")
        tree = ET.fromstring(text.replace("brls:", "brls_"))
        ids.update(e.attrib["id"] for e in tree.iter() if "id" in e.attrib)
    return ids


# 1. Well-formed layouts
for path in sorted((root / "resources/xml").rglob("*.xml")):
    try:
        ET.fromstring(path.read_text(encoding="utf-8-sig").replace("brls:", "brls_"))
    except ET.ParseError as ex:
        failures.append(f"{path.relative_to(root)}: {ex}")

# 2. Bindings: source file -> layouts it inflates
BINDINGS = {
    "app/include/tab/media_collection.hpp": ["tabs/media.xml"],
    "app/include/tab/media_movie.hpp": ["tabs/movie.xml"],
    "app/include/tab/media_series.hpp": ["tabs/series.xml"],
    "app/include/tab/home_tab.hpp": ["tabs/home.xml"],
    "app/include/tab/setting_tab.hpp": ["tabs/settings.xml"],
    "app/include/tab/server_login.hpp": ["tabs/server_login.xml"],
    "app/include/tab/server_add.hpp": ["tabs/server_add.xml"],
    "app/include/view/media_filter.hpp": ["view/media_filter.xml"],
    "app/include/view/context_menu.hpp": ["view/context_menu.xml"],
    "app/include/activity/server_list.hpp": ["activity/server_list.xml"],
    "app/src/view/video_source.cpp": ["tabs/seasons.xml", "view/episode_card.xml"],
}
for source, layouts in BINDINGS.items():
    ids = layout_ids(*layouts)
    text = (root / source).read_text(encoding="utf-8-sig")
    # MenuItem ids live in an inline XML string in context_menu.cpp
    if source.endswith("context_menu.hpp"):
        ids.update({"menu_item/title", "menu_item/icon"})
    for bound in re.findall(r'BRLS_BIND\([^"\n]+"([^"]+)"', text):
        if bound not in ids:
            failures.append(f"{source}: binding '{bound}' not found in {', '.join(layouts)}")
    print(f"checked bindings: {source}")


# 3. Translation keys
def flatten(prefix, node, out):
    for key, value in node.items():
        if isinstance(value, dict):
            flatten(f"{prefix}{key}/", value, out)
        else:
            out.add(f"{prefix}{key}")


def keys_for(lang):
    keys = set()
    for base in (root / "resources/i18n" / lang, root / "library/borealis/resources/i18n" / lang):
        if not base.exists():
            continue
        for path in base.glob("*.json"):
            flatten(f"{path.stem}/", json.loads(path.read_text(encoding="utf-8-sig")), keys)
    return keys


used = set()
for path in list((root / "app/src").rglob("*.cpp")) + list((root / "app/include").rglob("*.hpp")):
    text = path.read_text(encoding="utf-8-sig")
    used.update(re.findall(r'"((?:main|hints)/[A-Za-z0-9_/]+)"_i18n', text))
    used.update(re.findall(r'getStr\("((?:main|hints)/[A-Za-z0-9_/]+)"', text))
# sort labels are looked up at runtime from fntv::sortOptions()
used.update(re.findall(r'\{[^{}]*"(main/media/[a-z_]+)"\}', (root / "app/src/api/fntv.cpp").read_text(encoding="utf-8")))
for path in (root / "resources/xml").rglob("*.xml"):
    used.update(re.findall(r'@i18n/((?:main|hints)/[A-Za-z0-9_/]+)', path.read_text(encoding="utf-8-sig")))

for lang in ("zh-Hans", "en-US"):
    available = keys_for(lang)
    for key in sorted(used - available):
        failures.append(f"missing translation {lang}: {key}")
print(f"checked {len(used)} translation keys")

if failures:
    print("\n".join("FAIL " + f for f in failures))
    sys.exit(1)
print("PASS layouts, bindings and translations")
