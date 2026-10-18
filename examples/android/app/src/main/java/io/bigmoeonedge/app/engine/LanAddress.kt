package io.bigmoeonedge.app.engine

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import java.net.Inet4Address
import java.net.InetAddress

/**
 * Where other devices on this network reach the phone's engine: the IPv4 addresses of the Wi-Fi and
 * Ethernet networks it is on. Only those: a mobile network often carries a private address too
 * (carrier NAT), which no laptop on the same Wi-Fi can reach, so offering it would be wrong.
 */
object LanAddress {
    /** The addresses worth showing among [addresses]: IPv4, neither loopback nor link-local, each once. */
    fun pick(addresses: List<InetAddress>): List<String> =
        addresses.filter { it is Inet4Address && !it.isLoopbackAddress && !it.isLinkLocalAddress && !it.isAnyLocalAddress }
            .mapNotNull { it.hostAddress }
            .distinct()

    /** The engine's address for a client on this network, `http://<ip>:<port>/v1`. */
    fun url(ip: String): String = "http://$ip:${ServerProcess.PORT}/v1"

    /** What the phone has right now on its Wi-Fi and Ethernet networks. */
    fun current(ctx: Context): List<String> {
        val cm = ctx.getSystemService(ConnectivityManager::class.java) ?: return emptyList()
        return pick(
            cm.allNetworks.filter { isLan(cm, it) }.flatMap { n ->
                cm.getLinkProperties(n)?.linkAddresses?.map { it.address }.orEmpty()
            },
        )
    }

    /**
     * Calls [onChange] with the addresses each time a Wi-Fi or Ethernet network comes, goes or gets
     * a new address (a DHCP renewal, a switch of network). Returns the call that stops it.
     */
    fun watch(ctx: Context, onChange: (List<String>) -> Unit): () -> Unit {
        val app = ctx.applicationContext
        val cm = app.getSystemService(ConnectivityManager::class.java) ?: return {}
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = onChange(current(app))
            override fun onLost(network: Network) = onChange(current(app))
            override fun onLinkPropertiesChanged(network: Network, lp: android.net.LinkProperties) = onChange(current(app))
        }
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
            .build()
        return try {
            cm.registerNetworkCallback(request, callback)
            ({ runCatching { cm.unregisterNetworkCallback(callback) } })
        } catch (_: RuntimeException) {
            {} // too many callbacks registered by this app: the address shown just stops following
        }
    }

    private fun isLan(cm: ConnectivityManager, n: Network): Boolean {
        val caps = cm.getNetworkCapabilities(n) ?: return false
        return caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) || caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)
    }
}
