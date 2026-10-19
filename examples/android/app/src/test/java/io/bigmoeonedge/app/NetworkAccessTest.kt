package io.bigmoeonedge.app

import io.bigmoeonedge.app.engine.isNetworkDenied
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import java.net.ConnectException
import java.net.SocketException

class NetworkAccessTest {
    @Test fun theSocketRefusedByTheSystemIsADenial() {
        // The exact text #298 reported, as libcore wraps the ErrnoException.
        assertTrue(isNetworkDenied(SocketException("socket failed: EPERM (Operation not permitted)")))
    }

    @Test fun theDenialIsFoundUnderAWrapper() {
        val cause = SocketException("socket failed: EPERM (Operation not permitted)")
        assertTrue(isNetworkDenied(IOException("request failed", cause)))
    }

    @Test fun aMissingPermissionIsADenial() {
        assertTrue(isNetworkDenied(SecurityException("Permission denied (missing INTERNET permission?)")))
    }

    @Test fun aNetworkThatFailsIsNotADenial() {
        assertFalse(isNetworkDenied(ConnectException("failed to connect to /127.0.0.1 (port 8765): ECONNREFUSED")))
        assertFalse(isNetworkDenied(IOException("unexpected end of stream")))
        assertFalse(isNetworkDenied(IOException()))
    }
}
