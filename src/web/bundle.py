#!/usr/bin/env python3
"""
Heat Exchanger Web Asset Bundler
Reads standalone HTML, CSS, and JS files from src/web/
and bundles them into src/network/web_dashboard.hpp as a Flash PROGMEM string.
"""

import os
import sys

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    src_dir = os.path.dirname(script_dir)
    network_dir = os.path.join(src_dir, "network")
    output_hpp = os.path.join(network_dir, "web_dashboard.hpp")

    html_path = os.path.join(script_dir, "index.html")
    css_path = os.path.join(script_dir, "style.css")
    js_path = os.path.join(script_dir, "app.js")

    if not (os.path.isfile(html_path) and os.path.isfile(css_path) and os.path.isfile(js_path)):
        print(f"Error: Missing web asset in {script_dir}", file=sys.stderr)
        sys.exit(1)

    with open(html_path, "r", encoding="utf-8") as f:
        html = f.read()

    with open(css_path, "r", encoding="utf-8") as f:
        css = f.read()

    with open(js_path, "r", encoding="utf-8") as f:
        js = f.read()

    # Inline CSS
    css_tag = f"<style>\n{css}\n</style>"
    html = html.replace('<link rel="stylesheet" href="style.css">', css_tag)

    # Inline JS
    js_tag = f"<script>\n{js}\n</script>"
    html = html.replace('<script src="app.js"></script>', js_tag)

    # Generate web_dashboard.hpp
    os.makedirs(network_dir, exist_ok=True)
    header_content = (
        "#pragma once\n\n"
        "#include <Arduino.h>\n\n"
        "static const char PAGE_DASHBOARD[] PROGMEM = R\"rawliteral(\n"
        f"{html}\n"
        ")rawliteral\";\n"
    )

    with open(output_hpp, "w", encoding="utf-8") as f:
        f.write(header_content)

    print(f"✓ Bundled web assets into {output_hpp} ({len(header_content)} bytes)")

if __name__ == "__main__":
    main()
