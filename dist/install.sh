#!/bin/bash
set -e

PRODUCT_NAME="InstantSpaceSwitcher"
BUNDLE_ID="com.interversehq.InstantSpaceSwitcher"
SRC="$(cd "$(dirname "$0")/.." && pwd)/build/${PRODUCT_NAME}.app"
DEST="/Applications/${PRODUCT_NAME}.app"

if [ ! -d "${SRC}" ]; then
  echo "Error: ${SRC} not found. Run ./dist/build.sh first."
  exit 1
fi

echo "==> Quitting running instance (if any)"
osascript -e "tell application \"${PRODUCT_NAME}\" to quit" 2>/dev/null || true
pkill -x "${PRODUCT_NAME}" 2>/dev/null || true
sleep 0.5

echo "==> Replacing ${DEST}"
rm -rf "${DEST}"
cp -R "${SRC}" "${DEST}"

echo "==> Stripping Gatekeeper quarantine"
xattr -dr com.apple.quarantine "${DEST}" 2>/dev/null || true

if [ "$1" = "--reset-permissions" ]; then
  echo "==> Resetting Accessibility / Input Monitoring grants (sudo)"
  sudo tccutil reset Accessibility "${BUNDLE_ID}" || true
  sudo tccutil reset ListenEvent "${BUNDLE_ID}" || true
fi

echo "==> Launching"
open "${DEST}"

echo
echo "Installed at ${DEST}."
echo "Enable autostart: System Settings > General > Login Items > + > ${PRODUCT_NAME}"
