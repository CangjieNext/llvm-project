# REQUIRES: aarch64

## Test that arm64e.x1-macos is accepted instead of causing an error.
## arm64 and arm64e are ABI compatible (same CPU type), and ld64 accepts this.

# RUN: rm -rf %t; split-file --no-leading-lines %s %t

# RUN: llvm-mc -filetype=obj -triple=arm64-apple-macos -o %t/test.o /dev/null

# RUN: %no-arg-lld -syslibroot %t/sdk -lSystem -dylib -arch arm64 \
# RUN:     -platform_version macos 15 26 %t/test.o -o /dev/null

#--- sdk/usr/lib/libSystem.tbd
--- !tapi-tbd
tbd-version:      4
targets:          [ arm64-macos, arm64e.x1-macos ]
install-name:     '/usr/lib/libSystem.dylib'
current-version:  0001.001.1
...
