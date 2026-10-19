package io.bigmoeonedge.app.engine

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings

/**
 * Whether a failure means this app has been denied the network, rather than the network failing.
 *
 * Android ties every IP socket to the INTERNET permission, loopback included, and checks it when
 * the socket is created: the engine on 127.0.0.1 is unreachable without it, even offline. Stock
 * Android grants it at install and lets nobody take it back, but some systems add a per-app toggle
 * (GrapheneOS's "Network", on by default and switchable on the install screen) and firewall apps
 * do the same: the kernel then refuses the socket with EPERM, which libcore reports as
 * "socket failed: EPERM (...)". A runtime without the permission at all throws a SecurityException.
 * Neither is something a retry fixes, so a person has to be told where the switch is (#298).
 *
 * Read from the message, not from ErrnoException: the same text on every Android release, and
 * testable off the device.
 */
fun isNetworkDenied(t: Throwable): Boolean {
    var e: Throwable? = t
    var depth = 0
    while (e != null && depth++ < 8) {
        if (e is SecurityException) return true
        if (e.message?.contains("EPERM") == true) return true
        e = e.cause
    }
    return false
}

/** What the status line says instead: the cause, and that being offline is not it. */
const val NETWORK_DENIED = "This app has no network access, which the engine needs even offline."

/** What a download failed with, for the same cause: the import works without the network. */
const val DOWNLOAD_NETWORK_DENIED =
    "this app has no network access: allow it in the app's settings, or import the file instead"

/** The screen where a person gives the network back: this app's own page in the system settings. */
fun openAppSettings(ctx: Context) {
    ctx.startActivity(
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", ctx.packageName, null))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
    )
}
