#!/bin/bash
# Builds and signs the APK with the SDK's command-line tools; no Gradle.
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}
SDK=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
BT=$SDK/build-tools/35.0.0
JAR=$SDK/platforms/android-35/android.jar
rm -rf build && mkdir -p build/classes build/dex

"$BT/aapt2" link -o build/base.apk --manifest AndroidManifest.xml -I "$JAR"
"$JAVA_HOME/bin/javac" -source 17 -target 17 -Xlint:-options -cp "$JAR" -d build/classes src/com/rumblebridge/*.java
"$BT/d8" --lib "$JAR" --min-api 31 --output build/dex $(find build/classes -name '*.class')
(cd build/dex && zip -q -j ../base.apk classes.dex)
"$BT/zipalign" -f 4 build/base.apk build/aligned.apk
[ -f debug.keystore ] || "$JAVA_HOME/bin/keytool" -genkeypair -keystore debug.keystore -storepass android -keypass android \
  -alias kishi -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Rumblebridge" >/dev/null 2>&1
"$BT/apksigner" sign --ks debug.keystore --ks-pass pass:android --key-pass pass:android --out build/rumblebridge.apk build/aligned.apk
ls -l build/rumblebridge.apk
