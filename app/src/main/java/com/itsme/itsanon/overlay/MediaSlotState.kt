package com.itsme.itsanon.overlay

import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * Represents a single pre-loaded media slot (image or video URI).
 */
data class MediaSlot(
    val index: Int,
    var uri: Uri? = null,
    var label: String = "Empty",
    var isVideo: Boolean = false
)

/**
 * Holds the 3 pre-loaded media slots available for in-session slot switching.
 * The active slot index is stored in [OverlayState.activeSlot].
 */
object MediaSlotState {
    const val SLOT_COUNT = 3

    private val _slots = Array(SLOT_COUNT) { i -> MediaSlot(index = i) }

    /** Read-only view of all slots. */
    val slots: List<MediaSlot> get() = _slots.toList()

    /** Get a single slot by index (0–[SLOT_COUNT]). */
    fun getSlot(index: Int): MediaSlot = _slots[index.coerceIn(0, SLOT_COUNT - 1)]

    /** Update a slot's URI and metadata. */
    fun setSlot(index: Int, uri: Uri?, label: String, isVideo: Boolean) {
        val i = index.coerceIn(0, SLOT_COUNT - 1)
        _slots[i] = MediaSlot(index = i, uri = uri, label = label, isVideo = isVideo)
    }

    /** URI of the currently active slot (null if slot is empty). */
    val activeUri: Uri?
        get() = _slots[OverlayState.activeSlot].uri

    /** Clear all slots (e.g. when injection stops). */
    fun clearAll() {
        for (i in 0 until SLOT_COUNT) {
            _slots[i] = MediaSlot(index = i)
        }
    }
}
