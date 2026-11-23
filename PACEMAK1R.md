# PaceMak1r

This fork is based on KOWX712/KernelSU (KowSU). The Android manager is
PaceMak1r, application ID com.pace.maker. Internal JNI/namespace names remain
compatible with the upstream implementation.

## Cloud builds

Run the PaceMak1r Actions workflow on master or pacemak1r-manager. It builds
kernel modules, init and all four Android ksud targets before assembling the
manager. The PaceMak1r artifact contains PaceMak1r_<versionCode>.apk, identity/signature
reports and SHA256SUMS. The inherited Build Manager workflow is skipped in
this fork; use PaceMak1r for consistently signed builds.

The encrypted repository secret PACEMAK1R_SIGNING contains a JSON object with
keystore (base64 PKCS12), password and alias. The public release certificate is
manager/release-certificate.der. Kernel defaults and release signing must use
that same certificate. Keep the private keystore backed up outside Git history;
changing it prevents ordinary APK upgrades and requires rebuilding the kernel.

The in-app update checker reads releases from this repository only. To publish
an update later, attach the versioned APK from the Actions artifact to a GitHub
release; its filename supplies the Android version code to the update checker.

## Dynamic managers

Use Settings > Dynamic managers to authorize or revoke an installed app.
An authorization registers its APK v2 certificate and grants its app ID;
recognition by certificate alone does not grant manager privileges. Grants
cover the Android app ID, including other Android users and shared-UID apps.
The UI groups shared UIDs. Only authorize apps you trust with root management.

This requires the matching PaceMak1r kernel with CONFIG_KSU_DYNAMIC_MANAGER=y
(default). An existing KowSU kernel does not gain this feature by installing
only the APK. Choose kernel artifacts matching the device kernel/KMI.

Grants are bound to package and certificate, persisted by the kernel in
/data/adb/ksu/dynamic_manager (format v3), and by ksud in
/data/adb/ksu/.dynamic_manager. Early boot restores signatures without querying
PackageManager; boot completion validates installed identities before replaying
grants. Older binary formats are rejected instead of granting by certificate.
Root CLI equivalents: ksud dynamic set-uid UID, ungrant UID and list --json.

The implementation adapts LyraVoid/FinaSU at
a85c03ab6be57fd1a81a6d345eb5aa8275652b42, retaining upstream licenses.
Compilation and package/signature checks do not replace real-device testing.
