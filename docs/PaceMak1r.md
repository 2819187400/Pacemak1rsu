# PaceMak1r

This fork is based on KOWX712/KernelSU (KowSU). The Android manager is named
PaceMak1r and installs as com.pace.maker. Its source/JNI namespace remains
me.weishu.kernelsu.

Dynamic manager support was adapted from LyraVoid/FinaSU commit
a85c03ab6be57fd1a81a6d345eb5aa8275652b42, preserving upstream licenses.
The KowSU feature IDs are preserved; the new feature IDs are 10004 and 10005.

## Usage

Install a kernel/module built from this fork and the matching signed manager.
Open Settings > Dynamic managers. Select an installed app and confirm to
register its APK certificate and explicitly grant its app ID. Turn it off to
revoke manager access. Root and module control are powerful permissions.
Android shared UIDs and secondary users share the grant. Restart the target
app after changes; revocation cannot undo root processes it already launched.

The CLI is ksud dynamic: set-uid UID, ungrant UID, list --json,
get-sign --uid UID, set-apk APK, set-hash SIZE HASH, del SIZE HASH, clear.
Signature registration alone does not grant manager access. Only root can
change the kernel tables. Grants bind to package and certificate; reinstalling
under a reused UID or a changed certificate requires fresh authorization.

Kernel persistence lives at /data/adb/ksu/dynamic_manager (format v3).
The bundled daemon also atomically stores /data/adb/ksu/.dynamic_manager.
Corrupt/old unbound grants are not automatically authorized.
The bootstrap manager requires both com.pace.maker and the release certificate.
The public certificate is manager/release-certificate.der; its size and SHA256
are pinned in kernel/Kbuild. Keep the signing key backed up privately.

## CI

The PaceMak1r workflow checks Android Rust with cargo-ndk check, clippy, then
fmt. It builds the existing eight KMI module pairs, ksuinit, four Android ksud
ABIs, and finally the signed release APK with the daemons already bundled.
The encrypted repository secret PACEMAK1R_SIGNING contains a JSON object with
base64 keystore, password and alias. Signing files never enter artifacts.
CI verifies the package, label, APK v2 certificate and bundled daemon files.
No Telegram or release publishing is performed by this workflow.

Compilation does not replace tests on a real Android device. Check grant,
revoke, reboot, app upgrade/uninstall and a shared UID on your target kernel
before daily use. Keep a working recovery/boot image available.
