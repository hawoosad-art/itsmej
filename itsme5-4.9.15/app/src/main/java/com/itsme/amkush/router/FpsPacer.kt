package com.itsme.amkush.router

class FpsPacer(targetFps: Int) {

    private val frameNs: Long =
        if (targetFps > 0) (1_000_000_000L / targetFps) else 33_333_333L

    private var lastNs: Long = 0L



    fun pace() {
        val now = System.nanoTime()
        if (lastNs > 0L) {
            val elapsed   = now - lastNs
            val remaining = frameNs - elapsed
            if (remaining > 1_000_000L) {
                try {
                    Thread.sleep(remaining / 1_000_000L, (remaining % 1_000_000L).toInt())
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                }
            }
        }
        lastNs = System.nanoTime()
    }


    fun reset() { lastNs = 0L }
}
