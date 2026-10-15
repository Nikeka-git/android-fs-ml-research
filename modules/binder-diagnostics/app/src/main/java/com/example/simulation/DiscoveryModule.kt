package com.example.simulation

class NoValidOffsetException(message: String) : IllegalStateException(message)

class DiscoveryModule(
    private val kernel: SimulationKernel,
    private val searchEngine: SearchEngine = SearchEngine()
) {
    data class Config(
        val searchRange: Int = 256,
        val step: Int = 8,
        val maxRetries: Int = 3,
        val verbose: Boolean = true
    ) {
        init {
            require(searchRange >= 0) { "searchRange must be non-negative" }
            require(step > 0) { "step must be positive" }
            require(maxRetries > 0) { "maxRetries must be positive" }
        }
    }

    /**
     * Returns the healthy candidate nearest zero, including zero itself.
     * A success leaves the selected candidate applied. Failure restores the baseline.
     * Offsets are multiples of step centered on zero within the inclusive range.
     */
    fun discoverOptimalOffset(config: Config = Config()): Long =
        synchronized(SimulationSession.operationLock) {
            SimulationSession.checkCancellation()
            SimulationSession.requireSimulationMode(kernel)
            var selected = false
            var failure: Exception? = null
            try {
                kernel.resetKernelState()
                val baseAddress = kernel.getProcInfo()
                check(baseAddress > 0L) { "Simulation returned a non-positive process address" }
                if (config.verbose) {
                    println("[DISCOVERY] Searching simulator offsets: range=±${config.searchRange}, step=${config.step}, retries=${config.maxRetries}")
                }

                for (offset in searchEngine.offsets(config.searchRange, config.step)) {
                    SimulationSession.checkCancellation()
                    val targetAddress = try {
                        Math.addExact(baseAddress, offset)
                    } catch (_: ArithmeticException) {
                        continue
                    }
                    repeat(config.maxRetries) {
                        SimulationSession.checkCancellation()
                        kernel.resetKernelState()
                        val accepted = kernel.executeBadSpin(targetAddress)
                        SimulationSession.checkCancellation()
                        if (accepted && kernel.checkSystemHealth()) {
                            SimulationSession.checkCancellation()
                            selected = true
                            if (config.verbose) println("[DISCOVERY] Healthy simulator offset: $offset")
                            return@synchronized offset
                        }
                    }
                }
                throw NoValidOffsetException("No healthy simulator offset in ±${config.searchRange} with step ${config.step}")
            } catch (error: Exception) {
                failure = error
                if (error is InterruptedException) Thread.currentThread().interrupt()
                throw error
            } finally {
                if (!selected) {
                    try {
                        kernel.resetKernelState()
                    } catch (resetError: Exception) {
                        if (failure == null) throw resetError else failure.addSuppressed(resetError)
                    }
                }
            }
        }

    fun getDiscoveryReport(): String = synchronized(SimulationSession.operationLock) {
        "Discovery Report - Simulator State: ${kernel.getDetailedProcInfo()}"
    }

    /** Checks the model and JNI bridge, leaving a clean baseline after the probe. */
    fun validateKernelConnectivity(): Boolean = synchronized(SimulationSession.operationLock) {
        SimulationSession.checkCancellation()
        SimulationSession.requireSimulationMode(kernel)
        try {
            discoverOptimalOffset(Config(verbose = false))
            true
        } catch (error: InterruptedException) {
            Thread.currentThread().interrupt()
            throw error
        } catch (_: Exception) {
            false
        } finally {
            kernel.resetKernelState()
        }
    }
}
