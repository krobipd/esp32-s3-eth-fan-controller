#!/bin/sh
# ui/index.html -> gzip -> fan_controller/ui_asset.h
set -e
cd "$(dirname "$0")/.."
gzip -9 -n -c ui/index.html > /tmp/ui.gz
{
  echo "// GENERIERT von tools/build_ui.sh - nicht von Hand editieren"
  echo "#pragma once"
  echo "#include <pgmspace.h>"
  echo "static const uint8_t UI_ASSET[] PROGMEM = {"
  xxd -i < /tmp/ui.gz
  echo "};"
  echo "static const unsigned int UI_ASSET_LEN = sizeof(UI_ASSET);"
} > fan_controller/ui_asset.h
echo "ui_asset.h erzeugt: $(wc -c < /tmp/ui.gz) Bytes gzip"
