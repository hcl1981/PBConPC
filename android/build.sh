#!/bin/sh
# Builds the Android app: android/app/build/outputs/apk/release/app-release.apk
#
# Needs the Android SDK with platform 36 and NDK 29.0.14206865 (ANDROID_HOME, default
# ~/Android/Sdk), a JDK, CMake and Gradle 9 (or a Gradle wrapper's download of it).
set -e
cd "$(dirname "$0")"

SDL=SDL3-3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3   # as cmake/Dependencies.cmake
if [ ! -d deps/$SDL ]; then
    mkdir -p deps
    curl -fL -o deps/$SDL.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/$SDL.tar.gz
    echo "$SDL_SHA256  deps/$SDL.tar.gz" | sha256sum -c -
    tar -xzf deps/$SDL.tar.gz -C deps
    rm deps/$SDL.tar.gz
fi

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
if command -v gradle >/dev/null; then
    GRADLE=gradle
else
    GRADLE=$(ls -d "$HOME"/.gradle/wrapper/dists/gradle-9*/*/gradle-9*/bin/gradle 2>/dev/null | tail -n 1)
fi
[ -n "$GRADLE" ] || { echo "Gradle 9 not found" >&2; exit 1; }

"$GRADLE" --console=plain assembleRelease "$@"
ls -l app/build/outputs/apk/release/*.apk
