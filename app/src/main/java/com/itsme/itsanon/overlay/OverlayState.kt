package com.itsme.itsanon.overlay

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.setValue

/**
 * Singleton holding the current pan/zoom state for the floating overlay.
 *
 * scale_q16 is a Q16 fixed-point zoom factor:
 *   65536  = 1.0  → no zoom (default)
 *   131072 = 2.0  → 2× zoom in
 *
 * panX / panY are signed pixel offsets in source-frame coordinates.
 */
object OverlayState {
    /** Q16 zoom factor. 65536 = 1.0 (identity). */
    var scaleQ16: Int by mutableIntStateOf(65536)
        private set

    /** Horizontal pan offset in source pixels (positive = pan right). */
    var panX: Int by mutableIntStateOf(0)
        private set

    /** Vertical pan offset in source pixels (positive = pan down). */
    var panY: Int by mutableIntStateOf(0)
        private set

    /** Currently active media slot index (0–2). */
    @get:JvmName("getActiveSlot")
    @set:JvmName("setActiveSlotInternal")
    var activeSlot: Int by mutableIntStateOf(0)
        private set

    // ── Zoom ──────────────────────────────────────────────────────────────

    fun zoomIn() {
        // Increase zoom by 10% (multiply scale by 1.1 in Q16)
        val next = (scaleQ16.toLong() * 72090L / 65536L).toInt() // *1.1
        scaleQ16 = next.coerceAtMost(65536 * 16) // cap at 16×
    }

    fun zoomOut() {
        val next = (scaleQ16.toLong() * 59578L / 65536L).toInt() // *0.909…
        scaleQ16 = next.coerceAtLeast(32768) // allow down to 0.5× (letterbox)
    }

    // ── Pan ───────────────────────────────────────────────────────────────

    /** Pan step size in source pixels. Larger when zoomed in less. */
    private val panStep: Int get() = (40 * 65536) / scaleQ16.coerceAtLeast(1)

    fun panLeft()  { panX -= panStep }
    fun panRight() { panX += panStep }
    fun panUp()    { panY -= panStep }
    fun panDown()  { panY += panStep }

    // ── Slot ──────────────────────────────────────────────────────────────

    fun setActiveSlot(slot: Int) {
        activeSlot = slot.coerceIn(0, MediaSlotState.SLOT_COUNT - 1)
    }

    // ── Rotation ──────────────────────────────────────────────────────────

    /** Manual CW rotation in degrees (0, 90, 180, 270). */
    var rotationDeg: Int by mutableIntStateOf(0)
        private set

    /** Rotate 90° clockwise. */
    fun rotateRight() {
        rotationDeg = (rotationDeg + 90) % 360
    }

    /** Rotate 90° counter-clockwise (= 270° CW). */
    fun rotateLeft() {
        rotationDeg = (rotationDeg + 270) % 360
    }

    // ── Reset ─────────────────────────────────────────────────────────────

    fun reset() {
        scaleQ16    = 65536
        panX        = 0
        panY        = 0
        rotationDeg = 0
        activeSlot  = 0
    }

    /**
     * Reset only the media-transform state (zoom / pan / rotation) to defaults.
     * Called when a new media source starts, because the native producer also
     * recreates the ring with manual_rotation=0, scale_q16=65536 and pan=0.
     * This keeps the app's transform state in sync so a stale rotation/zoom is
     * never applied to the freshly-started media. The active media slot is NOT
     * reset here (the user's slot selection should be preserved).
     */
    fun resetTransform() {
        scaleQ16    = 65536
        panX        = 0
        panY        = 0
        rotationDeg = 0
    }
}
