#!/usr/bin/env python3
"""Zieht das inline-JavaScript aus ui/index.html heraus — ZEILENTREU.

Jeder <script>-Block landet an genau der Zeilennummer, an der er im HTML steht;
alles andere wird zu Leerzeilen. Dadurch zeigt eine Fehlermeldung von `node --check`
direkt auf die richtige Zeile in ui/index.html statt auf eine Position in einem
zusammengeschnittenen Extrakt.

Aufruf:  python3 tools/extract_ui_js.py <html> <ziel.js>
"""
import re
import sys

SCRIPT_AUF = re.compile(r"<script\b([^>]*)>", re.IGNORECASE)
SCRIPT_ZU = re.compile(r"</script\s*>", re.IGNORECASE)


def extrahiere(html: str) -> str:
    """Gibt JS zurück, bei dem jede Zeile ihrer Zeile im HTML entspricht."""
    zeilen = html.split("\n")
    raus = [""] * len(zeilen)
    in_block = False
    for nr, zeile in enumerate(zeilen):
        rest = zeile
        spalte = 0
        neu = ""
        while rest:
            if not in_block:
                m = SCRIPT_AUF.search(rest)
                if not m:
                    break
                # <script src=…> verweist auf eine externe Datei — hier ist kein Code.
                if "src=" in m.group(1).lower():
                    rest = rest[m.end():]
                    spalte += m.end()
                    continue
                neu += " " * (spalte + m.end() - len(neu))
                rest = rest[m.end():]
                spalte += m.end()
                in_block = True
            else:
                m = SCRIPT_ZU.search(rest)
                if not m:
                    neu += rest
                    rest = ""
                else:
                    neu += rest[:m.start()]
                    rest = rest[m.end():]
                    spalte += m.end()
                    in_block = False
        raus[nr] = neu
    return "\n".join(raus)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        js = extrahiere(f.read())
    if not js.strip():
        print("FEHLER: kein inline-JavaScript gefunden — Extraktion kaputt?", file=sys.stderr)
        return 1
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(js)
    return 0


if __name__ == "__main__":
    sys.exit(main())
