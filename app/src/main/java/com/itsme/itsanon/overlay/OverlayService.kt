package com.itsme.itsanon.overlay

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.itsme.itsanon.R
import com.itsme.itsanon.hooks.NativeFrameProducer
import com.itsme.itsanon.utils.Logger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Foreground service that draws a floating pan/zoom control overlay on top of
 * any app using TYPE_APPLICATION_OVERLAY (SYSTEM_ALERT_WINDOW).
 *
 * Design: A circular draggable FAB showing the hacker icon. Tapping toggles a
 * pop-out horizontal control panel to the left of the FAB.
 * Slot buttons S1/S2/S3 hot-swap the injected media between the pre-loaded
 * slots (no need to go back to the app to upload/switch media).
 *
 * Drag implementation:
 *   The touch listener is attached to the FAB only (not the whole root), and
 *   returns true on ACTION_DOWN to claim the touch sequence. Without that,
 *   Android drops ACTION_MOVE events and drag never fires. Panel buttons are
 *   unaffected because they are siblings of the FAB, not its children.
 *   Tap vs drag is distinguished in ACTION_UP by checking the `moved` flag.
 *
 * Notification:
 *   Uses IMPORTANCE_DEFAULT so the notification banner actually appears when
 *   the overlay first shows. Actions: "Hide overlay" / "Show overlay".
 *   Hiding sets FLAG_NOT_TOUCHABLE so Chrome permission dialogs work normally.
 */
class OverlayService : Service() {

    companion object {
        private const val TAG          = "OverlayService"
        private const val NOTIF_ID     = 2001
        private const val CHANNEL_ID   = "facegate_overlay_channel"
        private const val CHANNEL_NAME = "EcomCam Overlay Controls"

        const val ACTION_HIDE_OVERLAY  = "com.itsme.itsanon.HIDE_OVERLAY"
        const val ACTION_SHOW_OVERLAY  = "com.itsme.itsanon.SHOW_OVERLAY"

        @Volatile var isRunning = false
            private set

        fun start(context: Context) {
            if (!Settings.canDrawOverlays(context)) {
                Logger.w("$TAG SYSTEM_ALERT_WINDOW permission not granted — overlay skipped")
                return
            }
            // A new injection session is starting. The native producer has just
            // recreated the ring with manual_rotation=0 / scale=1 / pan=0, so reset
            // the app-side transform state to match — otherwise a stale rotation
            // from a previous media could be pushed later and rotate the frame.
            OverlayState.resetTransform()
            val intent = Intent(context, OverlayService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, OverlayService::class.java))
        }
    }

    private var windowManager: WindowManager?              = null
    private var overlayRoot: View?                         = null
    private var overlayParams: WindowManager.LayoutParams? = null
    private var overlayVisible: Boolean                    = true

    // ── Lifecycle ─────────────────────────────────────────────────────────

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIF_ID, buildNotification(visible = true))
        isRunning = true
        showOverlay()
        Logger.i(Logger.INJECTION, "$TAG overlay shown")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_HIDE_OVERLAY -> setOverlayVisible(false)
            ACTION_SHOW_OVERLAY -> setOverlayVisible(true)
        }
        return START_STICKY
    }

    /**
     * Hide/show the overlay window.
     *
     * HIDDEN: adds FLAG_NOT_TOUCHABLE so Android no longer treats it as a
     * blocking overlay — Chrome and other apps can then show permission
     * dialogs normally ("This site can't ask for your permission" goes away).
     *
     * SHOWN: removes FLAG_NOT_TOUCHABLE and restores full interaction.
     */
    private fun setOverlayVisible(visible: Boolean) {
        overlayVisible = visible
        val root   = overlayRoot  ?: return
        val params = overlayParams ?: return
        val wm     = windowManager ?: return

        if (visible) {
            params.flags = params.flags and WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE.inv()
            root.visibility = View.VISIBLE
        } else {
            params.flags = params.flags or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
            root.visibility = View.INVISIBLE
            isPanelOpen = false
            panelView?.visibility = View.GONE
        }
        wm.updateViewLayout(root, params)
        updateNotification(visible)
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        serviceScope.cancel()
        removeOverlay()
        isRunning = false
        Logger.i(Logger.INJECTION, "$TAG overlay removed")
        super.onDestroy()
    }

    // ── Overlay construction ──────────────────────────────────────────────

    private fun showOverlay() {
        if (!Settings.canDrawOverlays(this)) return

        val wm = getSystemService(WINDOW_SERVICE) as WindowManager
        windowManager = wm

        val (root, fab) = buildControlView()
        overlayRoot = root

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            overlayWindowType(),
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.END
            x = 12
            y = 300
        }

        overlayParams = params

        // Drag is attached to the FAB only — see makeDraggable() for why.
        makeDraggable(dragHandle = fab, root = root, params = params, wm = wm)

        wm.addView(root, params)
    }

    /* [V90] V88 made the button hide-proof via TYPE_ACCESSIBILITY_OVERLAY +
     * an accessibility service — but Play Protect HARD-BLOCKS installation of
     * any APK carrying an accessibility service ("can request access to
     * sensitive data"), and disabling Play Protect is not an option. Service
     * removed again; instead we DETECT the OS-level hiding (see
     * onOverlayWindowVisibility) and surface it via log + notification. */
    private fun overlayWindowType(): Int =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        else @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE

    /* [V90] fired by the root view when the SYSTEM hides our window —
     * Android 12+ does this while a foreground app requested
     * setHideOverlayWindows(true) (banking/security apps). */
    private fun onOverlayWindowVisibility(v: Int) {
        if (v != View.VISIBLE && overlayVisible) {
            Logger.w(TAG, "system hid the overlay — a foreground app requested setHideOverlayWindows; the button returns when you leave that app")
            updateNotification(false)
        }
    }

    private fun removeOverlay() {
        try {
            overlayRoot?.let { windowManager?.removeView(it) }
        } catch (_: Throwable) {}
        overlayRoot = null
        windowManager = null
    }

    // ── View builder ──────────────────────────────────────────────────────

    private var panelView: View? = null
    private var isPanelOpen = false
    private val slotButtons = mutableListOf<TextView>()
    private var scaleLabelView: TextView? = null
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    // ── Play/stop transport ────────────────────────────────────────────────
    private var playPauseBtn: TextView? = null
    private var playing = true           /* true = media playing, false = stopped/frozen */

    /**
     * Returns Pair(root, fab).
     * root  = the window-level view added to WindowManager.
     * fab   = the circular FAB used as the drag handle.
     */
    private fun buildControlView(): Pair<View, View> {
        val ctx = this

        // Root: horizontal row — [panel] [FAB]
        val root = object : LinearLayout(ctx) {
            override fun onWindowVisibilityChanged(visibility: Int) {
                super.onWindowVisibilityChanged(visibility)
                onOverlayWindowVisibility(visibility)
            }
        }.apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Color.TRANSPARENT)
        }

        // ── Panel (hidden initially) ──────────────────────────────────────
        val panel = buildPanel()
        panel.visibility = View.GONE
        panelView = panel
        root.addView(panel, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { marginEnd = dp(8) })

        // ── FAB ───────────────────────────────────────────────────────────
        // Note: FAB has NO setOnClickListener — tap is handled inside
        // makeDraggable's ACTION_UP branch to avoid conflicts with drag.
        val fab = buildFab()
        root.addView(fab, LinearLayout.LayoutParams(dp(48), dp(48)))

        return Pair(root, fab)
    }

    private fun buildFab(): View {
        val ctx = this

        val frame = FrameLayout(ctx).apply {
            clipToOutline = true
            outlineProvider = object : android.view.ViewOutlineProvider() {
                override fun getOutline(view: View, outline: android.graphics.Outline) {
                    outline.setOval(0, 0, view.width, view.height)
                }
            }
            elevation = dp(6).toFloat()
        }

        val bg = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(Color.argb(230, 12, 12, 12))
            setStroke(dp(1), Color.argb(60, 255, 255, 255))
        }
        frame.background = bg

        val icon = ImageView(ctx).apply {
            setImageDrawable(ContextCompat.getDrawable(ctx, R.drawable.splash_hacker))
            scaleType = ImageView.ScaleType.CENTER_CROP
        }
        frame.addView(icon, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ))

        // Active-slot indicator dot (bottom-right)
        val dot = View(ctx).apply {
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(Color.argb(255, 56, 189, 248))
            }
        }
        val dotSize = dp(8)
        frame.addView(dot, FrameLayout.LayoutParams(dotSize, dotSize).apply {
            gravity = Gravity.BOTTOM or Gravity.END
            bottomMargin = dp(2)
            marginEnd = dp(2)
        })

        // No setOnClickListener here — handled in makeDraggable ACTION_UP.
        return frame
    }

    private fun updateScaleLabel() {
        val scale = OverlayState.scaleQ16 / 65536.0f
        scaleLabelView?.text = String.format(java.util.Locale.US, "%.1f×", scale)
    }

    private fun buildPanel(): View {
        val ctx = this

        val panel = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(10), dp(10), dp(10), dp(10))
            background = roundedPanel()
            elevation = dp(8).toFloat()
        }

        // ── D-pad (nested LinearLayouts — GridLayout has unreliable touch
        //    dispatch inside TYPE_APPLICATION_OVERLAY windows) ──────────────
        val dpad = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
        }

        fun dpadBtn(label: String, onClick: () -> Unit): View =
            controlBtn(label, onClick)

        fun dpadRow(vararg views: View?): LinearLayout {
            val row = LinearLayout(ctx).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_HORIZONTAL
            }
            for (v in views) {
                if (v != null) row.addView(v, LinearLayout.LayoutParams(dp(40), dp(40)))
                else           row.addView(View(ctx), LinearLayout.LayoutParams(dp(40), dp(40)))
            }
            return row
        }

        dpad.addView(dpadRow(
            dpadBtn("↺") { OverlayState.rotateLeft();  pushRotation(); pushOverlay() },
            dpadBtn("▲") { OverlayState.panUp();       pushOverlay() },
            dpadBtn("↻") { OverlayState.rotateRight(); pushRotation(); pushOverlay() }
        ))
        dpad.addView(dpadRow(
            dpadBtn("◀") { OverlayState.panLeft();  pushOverlay() },
            dpadBtn("⊙") { OverlayState.reset();    pushRotation(); pushOverlay(); updateScaleLabel(); refreshSlotButtons() },
            dpadBtn("▶") { OverlayState.panRight(); pushOverlay() }
        ))
        dpad.addView(dpadRow(
            null,
            dpadBtn("▼") { OverlayState.panDown(); pushOverlay() },
            null
        ))

        panel.addView(dpad)

        // ── Divider ───────────────────────────────────────────────────────
        panel.addView(View(ctx).apply {
            setBackgroundColor(Color.argb(40, 255, 255, 255))
        }, LinearLayout.LayoutParams(dp(1), ViewGroup.LayoutParams.MATCH_PARENT).apply {
            marginStart = dp(6); marginEnd = dp(6)
        })

        // ── Right column: zoom + slots ────────────────────────────────────
        val rightCol = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
        }

        val zoomRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        zoomRow.addView(controlBtn("−") { OverlayState.zoomOut(); pushOverlay(); updateScaleLabel() })
        val scaleLabel = TextView(ctx).apply {
            text = "1.0×"
            textSize = 8f
            setTextColor(Color.argb(150, 255, 255, 255))
            gravity = Gravity.CENTER
            minWidth = dp(28)
        }
        scaleLabelView = scaleLabel
        updateScaleLabel()
        zoomRow.addView(scaleLabel, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { marginStart = dp(2); marginEnd = dp(2) })
        zoomRow.addView(controlBtn("+") { OverlayState.zoomIn(); pushOverlay(); updateScaleLabel() })
        rightCol.addView(zoomRow, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { bottomMargin = dp(4) })

        val slotRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        for (i in 0 until MediaSlotState.SLOT_COUNT) {
            val b = slotBtn(i)
            slotButtons.add(b)
            slotRow.addView(b, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { if (i > 0) marginStart = dp(3) })
        }
        refreshSlotButtons()
        rightCol.addView(slotRow)

        // ── Play/stop transport button (videos & images) ──────────────────
        val playBtn = controlBtn(if (playing) "⏸" else "▶") { togglePlayPause() } as TextView
        playPauseBtn = playBtn
        rightCol.addView(playBtn, LinearLayout.LayoutParams(
            dp(52), dp(40)
        ).apply { topMargin = dp(6) })

        panel.addView(rightCol)

        return panel
    }

    /**
     * Toggle play/stop of the injected media. "Stop" freezes the current frame
     * on screen (native decode thread stops writing to the ring); "Play" resumes.
     */
    private fun togglePlayPause() {
        playing = !playing
        NativeFrameProducer.setPaused(!playing)   // paused=true freezes, false plays
        playPauseBtn?.text = if (playing) "⏸" else "▶"
        Logger.i(Logger.INJECTION, "$TAG playback ${if (playing) "playing" else "stopped"}")
    }

    private fun togglePanel() {
        isPanelOpen = !isPanelOpen
        panelView?.visibility = if (isPanelOpen) View.VISIBLE else View.GONE
    }

    // ── Button helpers ────────────────────────────────────────────────────

    private fun controlBtn(label: String, onClick: () -> Unit): View =
        TextView(this).apply {
            text = label
            textSize = 15f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            val normalBg = roundedBtn(Color.argb(50, 255, 255, 255))
            val pressedBg = roundedBtn(Color.argb(120, 255, 255, 255))
            background = normalBg
            setPadding(0, 0, 0, 0)
            isClickable = true
            isFocusable = true
            // Fix: return true on DOWN to claim sequence, otherwise UP never arrives
            // and click becomes unreliable (zoom buttons especially)
            /* [V77] press-and-hold auto-repeat. Single taps alone felt dead:
             * one tap = one 40px step, so users mashed the arrows ("must
             * press many times"). Now: tap fires once immediately on UP as
             * before, and HOLDING repeats the action every 120 ms after a
             * 400 ms delay — smooth continuous pan/zoom. */
            val repeatHandler = android.os.Handler(android.os.Looper.getMainLooper())
            var repeatTask: Runnable? = null
            setOnTouchListener { v, event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> {
                        v.background = pressedBg
                        val task = object : Runnable {
                            override fun run() {
                                v.performClick()
                                repeatHandler.postDelayed(this, 120L)
                            }
                        }
                        repeatTask = task
                        repeatHandler.postDelayed(task, 400L)
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        v.background = normalBg
                        repeatTask?.let { repeatHandler.removeCallbacks(it) }
                        repeatTask = null
                        // Only fire if still inside view bounds
                        val inside = event.x >= 0 && event.x < v.width && event.y >= 0 && event.y < v.height
                        if (inside) {
                            v.performClick()
                        }
                        true
                    }
                    MotionEvent.ACTION_CANCEL -> {
                        v.background = normalBg
                        repeatTask?.let { repeatHandler.removeCallbacks(it) }
                        repeatTask = null
                        true
                    }
                    else -> false
                }
            }
            setOnClickListener { onClick() }
        }

    /** A functional media-slot button: tapping S1/S2/S3 switches the injected
     *  media to that pre-loaded slot (hot-swap via NativeFrameProducer). */
    private fun slotBtn(i: Int): TextView =
        TextView(this).apply {
            text = "S${i + 1}"
            textSize = 10f
            gravity = Gravity.CENTER
            width = dp(28); height = dp(28)
            setPadding(0, 0, 0, 0)
            isClickable = true
            isFocusable = true
            val normalColor = Color.argb(20, 255, 255, 255)
            val pressedColor = Color.argb(120, 255, 255, 255)
            setOnTouchListener { v, event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> {
                        v.background = roundedBtn(pressedColor)
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        refreshSlotButtons()
                        val inside = event.x >= 0 && event.x < v.width && event.y >= 0 && event.y < v.height
                        if (inside) v.performClick()
                        true
                    }
                    MotionEvent.ACTION_CANCEL -> {
                        refreshSlotButtons()
                        true
                    }
                    else -> false
                }
            }
            setOnClickListener {
                val slot = MediaSlotState.getSlot(i)
                val uri = slot.uri
                if (uri == null) {
                    Toast.makeText(
                        this@OverlayService,
                        "S${i + 1} is empty — upload media to it first",
                        Toast.LENGTH_SHORT
                    ).show()
                    return@setOnClickListener
                }
                OverlayState.setActiveSlot(i)
                refreshSlotButtons()

                serviceScope.launch {
                    val ok = withContext(Dispatchers.IO) {
                        NativeFrameProducer.startWithUri(this@OverlayService, uri)
                    }
                    if (ok) {
                        OverlayState.resetTransform()
                        pushRotation()
                        pushOverlay()
                        updateScaleLabel()
                        refreshSlotButtons()
                        Toast.makeText(this@OverlayService, "Switched to S${i + 1}", Toast.LENGTH_SHORT).show()
                        Logger.i(Logger.INJECTION, "$TAG slot switched to S${i + 1} (${slot.label})")
                    } else {
                        Toast.makeText(this@OverlayService, "Failed to switch to S${i + 1}", Toast.LENGTH_SHORT).show()
                    }
                }
            }
        }

    /** Re-paint the slot buttons: active slot highlighted, filled slots tinted. */
    private fun refreshSlotButtons() {
        slotButtons.forEachIndexed { i, tv ->
            val active = OverlayState.activeSlot == i
            val filled = MediaSlotState.getSlot(i).uri != null
            tv.background = roundedBtn(
                when {
                    active -> Color.argb(255, 108, 99, 255)   // violet
                    filled -> Color.argb(70, 74, 222, 128)    // green tint
                    else   -> Color.argb(20, 255, 255, 255)
                }
            )
            tv.setTextColor(
                when {
                    active -> Color.WHITE
                    filled -> Color.argb(220, 255, 255, 255)
                    else   -> Color.argb(80, 255, 255, 255)
                }
            )
        }
    }

    private fun roundedBtn(color: Int): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(8).toFloat()
        setColor(color)
    }

    private fun roundedPanel(): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dp(14).toFloat()
        setColor(Color.argb(224, 10, 10, 10))
        setStroke(dp(1), Color.argb(30, 255, 255, 255))
    }

    // ── Drag support ──────────────────────────────────────────────────────

    /**
     * Attaches drag handling to [dragHandle] (the FAB), moving the entire
     * [root] window via [params] + [wm].
     *
     * Why the touch listener is on the FAB, not root:
     *   A ViewGroup touch listener that returns false on ACTION_DOWN causes
     *   Android to drop all subsequent ACTION_MOVE/ACTION_UP events for that
     *   listener — so drag never fires. Returning true on ACTION_DOWN claims
     *   the touch sequence, but then child views inside the same ViewGroup
     *   never receive it. By attaching to the FAB (a leaf view that has no
     *   interactive children), we can safely return true on ACTION_DOWN and
     *   still let the panel buttons (which are siblings of the FAB, not its
     *   children) handle their own touches normally.
     *
     * Tap vs drag:
     *   A move threshold of 10 dp distinguishes a tap from a drag. If the
     *   finger lifted without crossing the threshold, ACTION_UP calls
     *   togglePanel() — exactly what a click listener would have done.
     */
    private fun makeDraggable(
        dragHandle: View,
        root: View,
        params: WindowManager.LayoutParams,
        wm: WindowManager
    ) {
        var initialX = 0; var initialY = 0
        var touchX = 0f;  var touchY = 0f
        var moved = false

        dragHandle.setOnTouchListener { _, event ->
            when (event.action) {
                MotionEvent.ACTION_DOWN -> {
                    initialX = params.x; initialY = params.y
                    touchX = event.rawX; touchY = event.rawY
                    moved = false
                    // Must return true to claim this touch sequence.
                    // Without this, Android never delivers ACTION_MOVE here.
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = (event.rawX - touchX).toInt()
                    val dy = (event.rawY - touchY).toInt()
                    if (!moved && (Math.abs(dx) > dp(10) || Math.abs(dy) > dp(10))) {
                        moved = true
                    }
                    if (moved) {
                        params.x = initialX + dx
                        params.y = initialY + dy
                        wm.updateViewLayout(root, params)
                    }
                    true
                }
                MotionEvent.ACTION_UP -> {
                    // Finger lifted without significant movement = tap → toggle panel
                    if (!moved) togglePanel()
                    true
                }
                else -> false
            }
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    /**
     * Push ONLY pan/zoom overlay params to the native producer.
     *
     * IMPORTANT: this must NOT send rotation. Zoom/pan buttons previously went
     * through pushParams(), which always re-sent setRotation(rotationDeg). After
     * switching media the native ring resets manual_rotation to 0 but the app's
     * OverlayState.rotationDeg keeps its stale value, so pressing zoom re-wrote
     * that stale rotation and made the frames rotate unexpectedly. Decoupling the
     * two means zoom/pan can never change the injected orientation.
     */
    private fun pushOverlay() {
        NativeFrameProducer.setOverlayParams(
            OverlayState.panX,
            OverlayState.panY,
            OverlayState.scaleQ16
        )
    }

    /** Push ONLY the manual rotation to the native producer. */
    private fun pushRotation() {
        NativeFrameProducer.setRotation(OverlayState.rotationDeg)
    }

    private fun dp(value: Int) = TypedValue.applyDimension(
        TypedValue.COMPLEX_UNIT_DIP, value.toFloat(), resources.displayMetrics
    ).toInt()

    // ── Notification ──────────────────────────────────────────────────────

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                CHANNEL_ID,
                CHANNEL_NAME,
                // IMPORTANCE_DEFAULT: shows a banner when the overlay first appears
                // so the user immediately sees the Hide/Show controls.
                NotificationManager.IMPORTANCE_DEFAULT
            ).apply {
                setShowBadge(false)
                description = "Controls for the EcomCam floating pan/zoom overlay"
            }
            getSystemService(NotificationManager::class.java)?.createNotificationChannel(ch)
        }
    }

    private fun buildNotification(visible: Boolean = true): Notification {
        val hidePi = PendingIntent.getService(
            this, 1,
            Intent(this, OverlayService::class.java).setAction(ACTION_HIDE_OVERLAY),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val showPi = PendingIntent.getService(
            this, 2,
            Intent(this, OverlayService::class.java).setAction(ACTION_SHOW_OVERLAY),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("EcomCam Overlay")
            .setContentText(
                if (visible) "Pan/zoom controls active — tap Hide to allow camera permissions"
                else "Overlay hidden — tap Show to restore controls"
            )
            .setSmallIcon(R.drawable.ic_notification)
            .setPriority(NotificationCompat.PRIORITY_DEFAULT)
            .setOngoing(true)

        // Always show both actions; the current state one is highlighted by position
        if (visible) {
            builder.addAction(0, "Hide overlay", hidePi)
        } else {
            builder.addAction(0, "Show overlay", showPi)
        }

        return builder.build()
    }

    private fun updateNotification(visible: Boolean) {
        getSystemService(NotificationManager::class.java)
            ?.notify(NOTIF_ID, buildNotification(visible))
    }
}
