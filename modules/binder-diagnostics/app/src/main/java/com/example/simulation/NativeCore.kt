package com.example.simulation

/** Contract of the local, in-memory simulation. No method accesses kernel memory. */
interface SimulationKernel {
    fun status(): String
    fun getProcInfo(): Long
    fun executeBadSpin(desiredAddress: Long): Boolean
    fun checkSystemHealth(): Boolean
    fun resetKernelState()
    fun getDetailedProcInfo(): String
}

/** Optional Binder device diagnostics. These methods do not provide kernel memory access. */
interface DeviceKernel : SimulationKernel {
    fun initProductionMode(): Boolean
    fun elevateToRoot(): Boolean
    fun isProductionMode(): Boolean
    fun probeDevice(): Boolean
    fun closeDevice()
}

class UnsupportedSimulationModeException(message: String) : IllegalStateException(message)

/** JNI state is process-wide, so complete workflows must share this monitor. */
object SimulationSession {
    val operationLock = Any()

    internal fun checkCancellation() {
        if (Thread.currentThread().isInterrupted) {
            throw InterruptedException("Simulation interrupted")
        }
    }

    internal fun requireSimulationMode(kernel: SimulationKernel) {
        if (kernel is DeviceKernel && kernel.isProductionMode()) {
            throw UnsupportedSimulationModeException(
                "Device diagnostic mode supports Binder version probing only; local simulation requires closing the device first"
            )
        }
    }
}

/**
 * Bridge to production_core.cpp. The legacy name includes a local model and Binder diagnostics.
 * Every public call shares [SimulationSession.operationLock]. Multi-call workflows hold that
 * same monitor for their complete operation, so device mode changes cannot interrupt a run.
 */
class NativeCore : DeviceKernel {
    companion object {
        init {
            System.loadLibrary("production_core")
        }
    }

    override fun status(): String = withSession { nativeStatus() }

    /** Returns a synthetic address in model mode, or zero in device diagnostic mode. */
    override fun getProcInfo(): Long = withSession { nativeGetProcInfo() }

    /** Attempts a local model transition. Unsupported in device diagnostic mode. */
    override fun executeBadSpin(desiredAddress: Long): Boolean =
        withSession { nativeExecuteBadSpin(desiredAddress) }

    /** Checks model success invariants. A working device probe is not simulation success. */
    override fun checkSystemHealth(): Boolean = withSession { nativeCheckSystemHealth() }

    override fun resetKernelState(): Unit = withSession { nativeResetKernelState() }
    override fun getDetailedProcInfo(): String = withSession { nativeGetDetailedProcInfo() }

    /** Opens a Binder device and queries its protocol version for diagnostics only. */
    override fun initProductionMode(): Boolean = withSession { nativeInitProductionMode() }

    /** Legacy compatibility entry point. Privilege elevation is unsupported and returns false. */
    override fun elevateToRoot(): Boolean = withSession { nativeElevateToRoot() }

    /** Legacy name: true means Binder diagnostic mode, not an exploitation capability. */
    override fun isProductionMode(): Boolean = withSession { nativeIsProductionMode() }

    /** Performs a Binder protocol version query on the open diagnostic device. */
    override fun probeDevice(): Boolean = withSession { nativeProbeDevice() }

    /** Closes the diagnostic device and returns to a clean local simulation. */
    override fun closeDevice(): Unit = withSession { nativeCloseDevice() }

    private inline fun <T> withSession(operation: () -> T): T =
        synchronized(SimulationSession.operationLock) { operation() }

    private external fun nativeStatus(): String
    private external fun nativeGetProcInfo(): Long
    private external fun nativeExecuteBadSpin(desiredAddress: Long): Boolean
    private external fun nativeCheckSystemHealth(): Boolean
    private external fun nativeResetKernelState()
    private external fun nativeGetDetailedProcInfo(): String
    private external fun nativeInitProductionMode(): Boolean
    private external fun nativeElevateToRoot(): Boolean
    private external fun nativeIsProductionMode(): Boolean
    private external fun nativeProbeDevice(): Boolean
    private external fun nativeCloseDevice()
}
