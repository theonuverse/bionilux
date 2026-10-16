# Source this before cross-compiling for aarch64-linux-android.
#   source scripts/android-env.sh
#   cargo build --release --target aarch64-linux-android

export NDK="${NDK:-$HOME/android-ndk-r30}"
export TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

if [ ! -x "$TC/bin/aarch64-linux-android24-clang" ]; then
    echo "NDK not found at $NDK" >&2
    echo "Download: https://dl.google.com/android/repository/android-ndk-r30-linux.zip" >&2
    return 1 2>/dev/null || exit 1
fi

export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$TC/bin/aarch64-linux-android24-clang"
export CC_aarch64_linux_android="$TC/bin/aarch64-linux-android24-clang"
export AR_aarch64_linux_android="$TC/bin/llvm-ar"