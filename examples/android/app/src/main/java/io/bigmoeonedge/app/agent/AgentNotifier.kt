package io.bigmoeonedge.app.agent

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import io.bigmoeonedge.app.MainActivity

/**
 * What the agent tells a person who is not looking: an approval it waits for, a run that ended.
 * A channel of its own beside the engine's (the service's is silent and ongoing; these are the
 * ones worth a sound), built the same way. Posted only while no screen of the app is shown, and
 * only when the person allowed notifications (asked at launch on Android 13 and later).
 */
object AgentNotifier {
    private const val CHANNEL = "agent"
    const val EXTRA_CONVERSATION = "io.bigmoeonedge.app.CONVERSATION"

    fun post(ctx: Context, conversation: String, title: String, text: String) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ctx.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) return
        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Agent", NotificationManager.IMPORTANCE_DEFAULT).apply {
            description = "An approval the agent waits for, a run that finished"
        })
        val open = Intent(ctx, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            .putExtra(EXTRA_CONVERSATION, conversation)
        val pi = PendingIntent.getActivity(ctx, conversation.hashCode(), open, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val n = Notification.Builder(ctx, CHANNEL)
            .setContentTitle(title)
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_notify_chat)
            .setContentIntent(pi)
            .setAutoCancel(true)
            .build()
        // One notification per conversation: its latest news replaces the one before.
        nm.notify(CHANNEL, conversation.hashCode(), n)
    }
}
