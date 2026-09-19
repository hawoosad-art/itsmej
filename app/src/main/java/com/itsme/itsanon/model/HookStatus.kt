package com.itsme.itsanon.model

data class HookStatus(
    val id: String,
    val name: String,
    val category: String,
    val isActive: Boolean
)

object HookStatusRegistry {
    /**
     * V4.4: All Java/Kotlin hooks removed. Injection is handled entirely at the
     * native HAL level by the ptrace-injected hook library. No Java-level hooks are registered.
     */
    fun getAllHooks(): List<HookStatus> = emptyList()
}
