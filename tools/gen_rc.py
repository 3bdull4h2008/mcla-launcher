import os
root = r"C:\Users\abdullah\OneDrive\Documents\mcla-launcher"
lines = ["#include <windows.h>", "#define IDI_APP 101", "", "IDI_APP ICON \"../launcher.ico\"", ""]
entries = [
    (1, r"..\ui\index.html"),
    (2, r"..\ui\launcher-bridge.js"),
    (3, r"..\ui\launcher.schema.json"),
    (4, r"..\ui\logo.png"),
    (5, r"..\ui\fonts\rajdhani-600.woff2"),
    (6, r"..\ui\fonts\rajdhani-700.woff2"),
    (7, r"..\ui\fonts\saira-600.woff2"),
    (8, r"..\ui\fonts\saira-700.woff2"),
    (9, r"..\deps\WebView2Loader.dll"),
    (10, r"..\ui\fonts\noto-sans-arabic-700.woff2"),
    (11, r"..\ui\fonts\OFL-NotoSansArabic.txt"),
]
for rid, rel in entries:
    p = os.path.join(root, rel.replace("..\\", ""))
    if not os.path.exists(p):
        raise SystemExit("MISSING " + p)
    lines.append(f"{rid} RCDATA \"{rel.replace(chr(92), '/')}\"")
out = os.path.join(root, "src", "mcla_launcher.rc")
open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("RC", out, "ENTRIES", len(entries))
