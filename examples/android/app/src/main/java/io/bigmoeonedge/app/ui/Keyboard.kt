package io.bigmoeonedge.app.ui

import android.view.ViewTreeObserver
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat

/**
 * Whether the on-screen keyboard is showing, read from the window's own insets on every layout.
 *
 * The window resizes for the keyboard (adjustResize, not edge to edge), so Compose's inset values do
 * not see it; the root view's insets do. Recomputed on each layout pass rather than on focus, so it
 * can never stick at "showing": a stuck value would hide the navigation and trap the person on the
 * screen. Visibility, not an inset height, so a hardware or floating keyboard reads as what it is.
 */
@Composable
fun rememberImeVisible(): Boolean {
    val view = LocalView.current
    var visible by remember { mutableStateOf(false) }
    DisposableEffect(view) {
        fun read() {
            visible = ViewCompat.getRootWindowInsets(view)?.isVisible(WindowInsetsCompat.Type.ime()) == true
        }
        val listener = ViewTreeObserver.OnGlobalLayoutListener { read() }
        view.viewTreeObserver.addOnGlobalLayoutListener(listener)
        read()
        onDispose { view.viewTreeObserver.removeOnGlobalLayoutListener(listener) }
    }
    return visible
}
