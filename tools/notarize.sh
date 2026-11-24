#!/bin/sh
# Notarizes a signed release disk image (tools/make_dmg.sh with
# SHOGO_SIGN_IDENTITY) with Apple and staples the ticket to it, so macOS opens
# the app without the "couldn't verify" warning, even offline.
# Usage: tools/notarize.sh Shogo-x.y.z.dmg
# Apple's credentials, one of:
#   NOTARY_PROFILE=name   a keychain profile saved with
#                         `xcrun notarytool store-credentials name`
#   NOTARY_KEY=AuthKey_XXXX.p8 NOTARY_KEY_ID=XXXX NOTARY_ISSUER=uuid
#                         an App Store Connect API key (the release workflow)
set -e
DMG=$1
[ -f "$DMG" ] || { echo "usage: $0 Shogo-x.y.z.dmg" >&2; exit 1; }
if [ -n "$NOTARY_PROFILE" ]; then
    set -- --keychain-profile "$NOTARY_PROFILE"
elif [ -n "$NOTARY_KEY" ] && [ -n "$NOTARY_KEY_ID" ] && [ -n "$NOTARY_ISSUER" ]; then
    set -- --key "$NOTARY_KEY" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER"
else
    echo "set NOTARY_PROFILE, or NOTARY_KEY, NOTARY_KEY_ID and NOTARY_ISSUER" >&2
    exit 1
fi
codesign --verify --strict "$DMG"
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
xcrun notarytool submit "$DMG" "$@" --wait --output-format plist > "$OUT"
STATUS=$(/usr/libexec/PlistBuddy -c 'Print status' "$OUT")
ID=$(/usr/libexec/PlistBuddy -c 'Print id' "$OUT")
if [ "$STATUS" != "Accepted" ]; then
    echo "notarization $STATUS ($ID); Apple's log:" >&2
    xcrun notarytool log "$ID" "$@" >&2 || true
    exit 1
fi
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature -v "$DMG"
echo "notarized: $DMG"
