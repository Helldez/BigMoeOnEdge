package io.bigmoeonedge.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.Spacer
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Text
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.bigmoeonedge.app.R

/**
 * The visual language (docs/design.md), as Material 3 reads it: the logo's palette as a colour
 * scheme, Pixelify Sans for the product's own voice (titles, labels, figures), the system face for
 * prose, and every corner square. Material's ripple and elevation stay: a phone's touch feedback
 * is what the toolkit is for.
 */
object Palette {
    val Bg = Color(0xFF0A0512)
    val Surface = Color(0xFF160B2A)
    val Surface2 = Color(0xFF2A1150)
    val Border = Color(0xFF3B2A63)
    val Text = Color(0xFFF2ECFF)
    val Muted = Color(0xFFB9A9E6)
    val Accent = Color(0xFF21E6FF)
    val AccentText = Color(0xFF0A0512)
    val Accent2 = Color(0xFF6C5AA8)
    val Warn = Color(0xFFFFC53D)
    val Ok = Color(0xFF7CE0B0)
    val Err = Color(0xFFFF6B8B)

    val LightBg = Color(0xFFF2ECFF)
    val LightSurface = Color(0xFFFFFFFF)
    val LightSurface2 = Color(0xFFE6DCFF)
    val LightText = Color(0xFF0A0512)
    val LightMuted = Color(0xFF4B3A7A)
    val LightAccent = Color(0xFF0FA8C2)
    val LightWarn = Color(0xFF8A5A00)
    val LightOk = Color(0xFF1D7A46)
    val LightErr = Color(0xFFB42318)
}

/** The wordmark's face; nothing else is set in it, because pixel text is hard to read small. */
private val PixelFont = FontFamily(Font(R.font.pixelify_sans, FontWeight.Normal), Font(R.font.pixelify_sans, FontWeight.Bold))

/** Every word the app shows, the person's and the model's included. */
val UiFont = FontFamily(
    Font(R.font.oxanium_regular, FontWeight.Normal),
    Font(R.font.oxanium_semibold, FontWeight.SemiBold),
    Font(R.font.oxanium_bold, FontWeight.Bold),
)

private val DarkScheme: ColorScheme = darkColorScheme(
    primary = Palette.Accent,
    onPrimary = Palette.AccentText,
    primaryContainer = Palette.Surface2,
    onPrimaryContainer = Palette.Text,
    secondary = Palette.Accent2,
    onSecondary = Palette.Text,
    secondaryContainer = Palette.Surface2,
    onSecondaryContainer = Palette.Text,
    tertiary = Palette.Warn,
    onTertiary = Palette.AccentText,
    tertiaryContainer = Color(0xFF3D2E0C),
    onTertiaryContainer = Palette.Warn,
    background = Palette.Bg,
    onBackground = Palette.Text,
    surface = Palette.Bg,
    onSurface = Palette.Text,
    surfaceVariant = Palette.Surface,
    onSurfaceVariant = Palette.Muted,
    surfaceContainer = Palette.Surface,
    surfaceContainerHigh = Palette.Surface2,
    surfaceContainerHighest = Palette.Surface2,
    outline = Palette.Border,
    outlineVariant = Palette.Border,
    error = Palette.Err,
    onError = Palette.AccentText,
    errorContainer = Color(0xFF3F1424),
    onErrorContainer = Palette.Err,
)

private val LightScheme: ColorScheme = lightColorScheme(
    primary = Palette.LightAccent,
    onPrimary = Palette.LightSurface,
    primaryContainer = Palette.LightSurface2,
    onPrimaryContainer = Palette.LightText,
    secondary = Palette.Accent2,
    onSecondary = Palette.LightSurface,
    secondaryContainer = Palette.LightSurface2,
    onSecondaryContainer = Palette.LightText,
    tertiary = Palette.LightWarn,
    onTertiary = Palette.LightSurface,
    tertiaryContainer = Color(0xFFFFF3D6),
    onTertiaryContainer = Palette.LightWarn,
    background = Palette.LightBg,
    onBackground = Palette.LightText,
    surface = Palette.LightBg,
    onSurface = Palette.LightText,
    surfaceVariant = Palette.LightSurface,
    onSurfaceVariant = Palette.LightMuted,
    surfaceContainer = Palette.LightSurface,
    surfaceContainerHigh = Palette.LightSurface2,
    surfaceContainerHighest = Palette.LightSurface2,
    outline = Palette.Accent2,
    outlineVariant = Palette.LightSurface2,
    error = Palette.LightErr,
    onError = Palette.LightSurface,
    errorContainer = Color(0xFFFDE8E6),
    onErrorContainer = Palette.LightErr,
)

private val UiTypography = Typography().let { t ->
    t.copy(
        displayLarge = t.displayLarge.copy(fontFamily = UiFont),
        displayMedium = t.displayMedium.copy(fontFamily = UiFont),
        displaySmall = t.displaySmall.copy(fontFamily = UiFont),
        headlineLarge = t.headlineLarge.copy(fontFamily = UiFont, fontWeight = FontWeight.Bold),
        headlineMedium = t.headlineMedium.copy(fontFamily = UiFont, fontWeight = FontWeight.Bold),
        headlineSmall = t.headlineSmall.copy(fontFamily = UiFont, fontWeight = FontWeight.Bold),
        titleLarge = t.titleLarge.copy(fontFamily = UiFont, fontWeight = FontWeight.Bold),
        titleMedium = t.titleMedium.copy(fontFamily = UiFont, fontWeight = FontWeight.Bold),
        titleSmall = t.titleSmall.copy(fontFamily = UiFont, fontWeight = FontWeight.SemiBold),
        bodyLarge = t.bodyLarge.copy(fontFamily = UiFont),
        bodyMedium = t.bodyMedium.copy(fontFamily = UiFont),
        bodySmall = t.bodySmall.copy(fontFamily = UiFont),
        labelLarge = t.labelLarge.copy(fontFamily = UiFont, fontWeight = FontWeight.SemiBold),
        labelMedium = t.labelMedium.copy(fontFamily = UiFont),
        labelSmall = t.labelSmall.copy(fontFamily = UiFont),
    )
}

private val SquareShapes = Shapes(
    extraSmall = androidx.compose.foundation.shape.RoundedCornerShape(0.dp),
    small = androidx.compose.foundation.shape.RoundedCornerShape(0.dp),
    medium = androidx.compose.foundation.shape.RoundedCornerShape(0.dp),
    large = androidx.compose.foundation.shape.RoundedCornerShape(0.dp),
    extraLarge = androidx.compose.foundation.shape.RoundedCornerShape(0.dp),
)

@Composable
fun BmoeTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (isSystemInDarkTheme()) DarkScheme else LightScheme,
        typography = UiTypography,
        shapes = SquareShapes,
        content = content,
    )
}

/** A figure the engine reports: digits of one width, so a live value does not jitter. */
val FigureStyle: TextStyle @Composable get() = TextStyle(fontFamily = UiFont, fontSize = 13.sp, fontFeatureSettings = "tnum")

/** The mark: the chip beside the wordmark, as the design has it in a bar. */
@Composable
fun BrandMark(wordmark: Boolean = true) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(painterResource(R.drawable.ic_logo), contentDescription = "BigMoeOnEdge", tint = Color.Unspecified, modifier = Modifier.size(24.dp))
        if (wordmark) {
            Spacer(Modifier.width(8.dp))
            Text("BigMoeOnEdge", fontFamily = PixelFont, fontWeight = FontWeight.Bold, fontSize = 18.sp, color = MaterialTheme.colorScheme.onSurface)
        }
    }
}
