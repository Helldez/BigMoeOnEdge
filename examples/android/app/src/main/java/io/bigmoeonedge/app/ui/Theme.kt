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

val PixelFont = FontFamily(Font(R.font.pixelify_sans, FontWeight.Normal), Font(R.font.pixelify_sans, FontWeight.Bold))

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

private val PixelTypography = Typography().let { t ->
    t.copy(
        titleLarge = t.titleLarge.copy(fontFamily = PixelFont, fontWeight = FontWeight.Bold),
        titleMedium = t.titleMedium.copy(fontFamily = PixelFont, fontWeight = FontWeight.Bold),
        titleSmall = t.titleSmall.copy(fontFamily = PixelFont),
        labelLarge = t.labelLarge.copy(fontFamily = PixelFont),
        labelMedium = t.labelMedium.copy(fontFamily = PixelFont),
        labelSmall = t.labelSmall.copy(fontFamily = PixelFont),
        headlineSmall = t.headlineSmall.copy(fontFamily = PixelFont, fontWeight = FontWeight.Bold),
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
        typography = PixelTypography,
        shapes = SquareShapes,
        content = content,
    )
}

/** A figure the engine reports, in the product's own face. */
val FigureStyle: TextStyle @Composable get() = TextStyle(fontFamily = PixelFont, fontSize = 13.sp)

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
