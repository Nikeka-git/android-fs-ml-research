package com.example.simulation

import org.junit.After
import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

class SimulationDomainTest {
    @After
    fun clearInterruption() {
        Thread.interrupted()
    }

    @Test
    fun candidateOrderIsCenteredOnZeroEvenWhenRangeIsNotDivisibleByStep() {
        assertEquals(listOf(0L, -8L, 8L, -16L, 16L), SearchEngine().offsets(18, 8).toList())
        assertEquals(listOf(0L), SearchEngine().offsets(0, 8).toList())
        assertEquals(listOf(0L), SearchEngine().offsets(5, 8).toList())
    }

    @Test
    fun largeCandidateValuesDoNotOverflow() {
        assertEquals(
            listOf(0L, -Int.MAX_VALUE.toLong(), Int.MAX_VALUE.toLong()),
            SearchEngine().offsets(Int.MAX_VALUE, Int.MAX_VALUE).toList()
        )
    }

    @Test
    fun invalidSearchConfigurationIsRejected() {
        assertThrows(IllegalArgumentException::class.java) { DiscoveryModule.Config(searchRange = -1) }
        assertThrows(IllegalArgumentException::class.java) { DiscoveryModule.Config(step = 0) }
        assertThrows(IllegalArgumentException::class.java) { DiscoveryModule.Config(step = -1) }
        assertThrows(IllegalArgumentException::class.java) { DiscoveryModule.Config(maxRetries = 0) }
        assertThrows(IllegalArgumentException::class.java) { SearchEngine().offsets(-1, 8) }
        assertThrows(IllegalArgumentException::class.java) { SearchEngine().offsets(8, 0) }
    }

    @Test
    fun zeroIsAValidOptimalOffsetAndLeavesTheSelectedState() {
        val kernel = FakeKernel(healthyOffsets = setOf(0L, -8L))
        assertEquals(0L, DiscoveryModule(kernel).discoverOptimalOffset(quietConfig()))
        assertEquals(kernel.base, kernel.target)
        assertEquals(listOf(kernel.base), kernel.attemptedTargets)
    }

    @Test
    fun discoveryChoosesNearestHealthyOffsetAndUsesDeterministicTieBreak() {
        val kernel = FakeKernel(healthyOffsets = setOf(-16L, -8L, 8L))
        assertEquals(-8L, DiscoveryModule(kernel).discoverOptimalOffset(quietConfig()))
        assertEquals(kernel.base - 8L, kernel.target)
        assertTrue(kernel.checkSystemHealth())
    }

    @Test
    fun retriesAreBoundedAndCanRecoverATransientFailure() {
        val kernel = FakeKernel(healthyOffsets = setOf(0L), attemptsBeforeAcceptance = 2)
        assertEquals(0L, DiscoveryModule(kernel).discoverOptimalOffset(quietConfig(retries = 2)))
        assertEquals(2, kernel.attemptedTargets.size)
        assertEquals(1, kernel.healthChecks)
        assertEquals(3, kernel.resetCount)
    }

    @Test
    fun noCandidateIsDistinctFromZeroAndRestoresBaseline() {
        val kernel = FakeKernel(healthyOffsets = emptySet())
        val failure = assertThrows(NoValidOffsetException::class.java) {
            DiscoveryModule(kernel).discoverOptimalOffset(quietConfig(range = 8, retries = 2))
        }
        assertTrue(failure.message!!.contains("No healthy simulator offset"))
        assertEquals(6, kernel.attemptedTargets.size)
        assertNull(kernel.target)
    }

    @Test
    fun overflowingTargetAddressesAreSkippedRatherThanWrapped() {
        val kernel = FakeKernel(base = Long.MAX_VALUE - 4L, healthyOffsets = setOf(-16L))
        assertEquals(-16L, DiscoveryModule(kernel).discoverOptimalOffset(quietConfig()))
        assertEquals(listOf(kernel.base, kernel.base - 8, kernel.base - 16), kernel.attemptedTargets)
        assertTrue(kernel.attemptedTargets.all { it > 0L })
    }

    @Test
    fun invalidModelAddressFailsBeforeAnyCandidateIsApplied() {
        val kernel = FakeKernel(base = 0L)
        assertThrows(IllegalStateException::class.java) {
            DiscoveryModule(kernel).discoverOptimalOffset(quietConfig())
        }
        assertTrue(kernel.attemptedTargets.isEmpty())
        assertNull(kernel.target)
    }

    @Test
    fun connectivityProbeAlwaysRestoresBaseline() {
        val kernel = FakeKernel()
        assertTrue(DiscoveryModule(kernel).validateKernelConnectivity())
        assertNull(kernel.target)
        kernel.healthyOffsets = emptySet()
        assertFalse(DiscoveryModule(kernel).validateKernelConnectivity())
        assertNull(kernel.target)
    }

    @Test
    fun discoveryCancellationStopsImmediatelyAndResetsState() {
        val kernel = FakeKernel()
        kernel.onExecute = { Thread.currentThread().interrupt() }
        assertThrows(InterruptedException::class.java) {
            DiscoveryModule(kernel).discoverOptimalOffset(quietConfig())
        }
        assertTrue(Thread.currentThread().isInterrupted)
        assertEquals(1, kernel.attemptedTargets.size)
        assertNull(kernel.target)
    }

    @Test
    fun anAlreadyCancelledDiscoveryDoesNotModifyExistingState() {
        val kernel = FakeKernel()
        kernel.executeBadSpin(kernel.base + 64L)
        Thread.currentThread().interrupt()
        assertThrows(InterruptedException::class.java) {
            DiscoveryModule(kernel).discoverOptimalOffset(quietConfig())
        }
        assertEquals(kernel.base + 64L, kernel.target)
        assertEquals(0, kernel.resetCount)
    }

    @Test
    fun fullWorkflowCanRunRepeatedlyAndUsesMonotonicElapsedTime() {
        val kernel = FakeKernel()
        val clock = listOf(1_000_000L, 26_000_000L, 40_000_000L, 52_000_000L).iterator()
        val orchestrator = orchestrator(kernel, clock = { clock.next() })
        val first = orchestrator.runFullExploit()
        val second = orchestrator.runFullExploit()
        assertTrue(first.success)
        assertTrue(second.success)
        assertEquals(25L, first.executionTimeMillis)
        assertEquals(12L, second.executionTimeMillis)
        assertEquals(64L, first.optimalOffset)
        assertEquals(ExploitOrchestrator.Phase.entries.toList(), first.phasesCompleted)
        assertEquals("target=${kernel.base + 64L}", second.kernelState)
        assertEquals(kernel.base + 64L, kernel.target)
        assertFalse(first.logs.any { it.contains("KERNEL READ/WRITE") })
    }

    @Test
    fun noCandidateDoesNotReportDiscoveryAsCompleted() {
        val kernel = FakeKernel(healthyOffsets = emptySet())
        val result = orchestrator(kernel).runFullExploit()
        assertFalse(result.success)
        assertNull(result.optimalOffset)
        assertTrue(result.phasesCompleted.isEmpty())
        assertTrue(result.logs.any { it.contains("NoValidOffsetException") })
        assertEquals("target=baseline", result.kernelState)
    }

    @Test
    fun failedFinalApplicationPreservesOffsetAndCompletedPhasesOnly() {
        val kernel = FakeKernel()
        var successfulTargetApplications = 0
        kernel.acceptTarget = { address ->
            address != kernel.base + 64L || ++successfulTargetApplications == 1
        }
        val result = orchestrator(kernel).runFullExploit()
        assertFalse(result.success)
        assertEquals(64L, result.optimalOffset)
        assertEquals(
            listOf(ExploitOrchestrator.Phase.DISCOVERY, ExploitOrchestrator.Phase.RACE_CONDITION),
            result.phasesCompleted
        )
        assertTrue(result.logs.any { it.contains("rejected the selected target") })
        assertNull(kernel.target)
    }

    @Test
    fun failedFinalHealthValidationDoesNotCompleteValidationPhase() {
        val kernel = FakeKernel(healthyOffsets = setOf(0L))
        kernel.acceptHealth = { kernel.healthChecks == 1 }
        val result = orchestrator(kernel).runFullExploit()
        assertFalse(result.success)
        assertEquals(3, result.phasesCompleted.size)
        assertFalse(result.phasesCompleted.contains(ExploitOrchestrator.Phase.POST_EXPLOITATION))
        assertNull(kernel.target)
    }

    @Test
    fun workerFailuresRetainTheirCauseInDiagnostics() {
        val kernel = FakeKernel(healthyOffsets = setOf(0L))
        val result = orchestrator(kernel, pause = { throw IllegalArgumentException("worker test failure") }).runFullExploit()
        assertFalse(result.success)
        assertEquals(listOf(ExploitOrchestrator.Phase.DISCOVERY), result.phasesCompleted)
        assertTrue(result.logs.any { it.contains("IllegalArgumentException: worker test failure") })
        assertNull(kernel.target)
    }

    @Test
    fun workerTimeoutCancelsTasksAndLeavesTheOrchestratorReusable() {
        val kernel = FakeKernel(healthyOffsets = setOf(0L))
        val blockWorkers = AtomicBoolean(true)
        val orchestrator = ExploitOrchestrator(
            kernel, DiscoveryModule(kernel), pause = { if (blockWorkers.get()) Thread.sleep(10_000L) },
            phaseTimeoutMillis = 200L
        )
        val failure = orchestrator.runFullExploit()
        assertFalse(failure.success)
        assertEquals(1, failure.phasesCompleted.size)
        assertTrue(failure.logs.any { it.contains("TimeoutException") })
        assertNull(kernel.target)
        blockWorkers.set(false)
        assertTrue(orchestrator.runFullExploit().success)
    }

    @Test
    fun interruptionCancelsBothWorkersBeforeTheWorkflowReturns() {
        val kernel = FakeKernel(healthyOffsets = setOf(0L))
        val entered = CountDownLatch(2)
        val exited = CountDownLatch(2)
        val neverReleased = CountDownLatch(1)
        val result = AtomicReference<ExploitOrchestrator.ExploitResult>()
        val interruptedAtReturn = AtomicBoolean()
        val orchestrator = orchestrator(kernel, pause = {
            entered.countDown()
            try {
                neverReleased.await()
            } finally {
                exited.countDown()
            }
        })
        val caller = Thread {
            result.set(orchestrator.runFullExploit())
            interruptedAtReturn.set(Thread.currentThread().isInterrupted)
        }
        caller.start()
        try {
            assertTrue(entered.await(3L, TimeUnit.SECONDS))
            caller.interrupt()
            caller.join(3_000L)
            assertFalse(caller.isAlive)
            assertEquals(0L, exited.count)
            assertTrue(interruptedAtReturn.get())
            assertFalse(result.get().success)
            assertEquals(1, result.get().phasesCompleted.size)
            assertNull(kernel.target)
        } finally {
            caller.interrupt()
            neverReleased.countDown()
            caller.join(3_000L)
        }
    }

    @Test
    fun sharedWorkflowLockPreventsASecondDiscoveryFromChangingStateDuringTheFirst() {
        val kernel = FakeKernel(healthyOffsets = setOf(0L))
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val otherStarted = CountDownLatch(1)
        val otherFinished = CountDownLatch(1)
        val firstResult = AtomicReference<ExploitOrchestrator.ExploitResult>()
        val failure = AtomicReference<Throwable>()
        val first = Thread {
            firstResult.set(orchestrator(kernel, pause = { entered.countDown(); release.await() }).runFullExploit())
        }
        val second = Thread {
            otherStarted.countDown()
            try {
                DiscoveryModule(kernel).discoverOptimalOffset(quietConfig())
            } catch (error: Throwable) {
                failure.set(error)
            } finally {
                otherFinished.countDown()
            }
        }
        first.start()
        try {
            assertTrue(entered.await(3L, TimeUnit.SECONDS))
            val resetCountBefore = kernel.resetCount
            second.start()
            assertTrue(otherStarted.await(3L, TimeUnit.SECONDS))
            assertFalse(otherFinished.await(100L, TimeUnit.MILLISECONDS))
            assertEquals(resetCountBefore, kernel.resetCount)
            release.countDown()
            first.join(3_000L)
            second.join(3_000L)
            assertFalse(first.isAlive)
            assertFalse(second.isAlive)
            assertTrue(firstResult.get().success)
            assertNull(failure.get())
        } finally {
            release.countDown()
            first.interrupt()
            second.interrupt()
            first.join(3_000L)
            second.join(3_000L)
        }
    }

    @Test
    fun anAlreadyCancelledFullWorkflowDoesNotResetExistingState() {
        val kernel = FakeKernel()
        kernel.executeBadSpin(kernel.base + 64L)
        Thread.currentThread().interrupt()
        val result = orchestrator(kernel).runFullExploit()
        assertFalse(result.success)
        assertTrue(Thread.currentThread().isInterrupted)
        assertEquals(kernel.base + 64L, kernel.target)
        assertEquals(0, kernel.resetCount)
    }

    @Test
    fun deviceDiagnosticModeRejectsDiscoveryBeforeAnyModelMutation() {
        val device = FakeDeviceKernel()
        device.model.executeBadSpin(device.model.base + 64L)
        val discovery = DiscoveryModule(device)
        val error = assertThrows(UnsupportedSimulationModeException::class.java) {
            discovery.discoverOptimalOffset(quietConfig())
        }
        assertTrue(error.message!!.contains("Binder version probing only"))
        assertThrows(UnsupportedSimulationModeException::class.java) {
            discovery.validateKernelConnectivity()
        }
        assertEquals(0, device.model.resetCount)
        assertEquals(0, device.model.addressQueries)
        assertEquals(0, device.model.healthChecks)
        assertEquals(1, device.model.attemptedTargets.size)
        assertEquals(device.model.base + 64L, device.model.target)
        assertTrue(device.isProductionMode())
        assertEquals(0, device.probeCalls)
    }

    @Test
    fun aSuccessfulDeviceProbeCannotBeReportedAsSimulationSuccess() {
        val device = FakeDeviceKernel()
        device.model.executeBadSpin(device.model.base + 64L)
        assertTrue(device.probeDevice())
        val result = ExploitOrchestrator(device, DiscoveryModule(device), pause = {}).runFullExploit()
        assertFalse(result.success)
        assertNull(result.optimalOffset)
        assertTrue(result.phasesCompleted.isEmpty())
        assertTrue(result.logs.any { it.contains("UnsupportedSimulationModeException") })
        assertTrue(result.logs.any { it.contains("Device diagnostic mode") })
        assertEquals(0, device.model.resetCount)
        assertEquals(0, device.model.addressQueries)
        assertEquals(0, device.model.healthChecks)
        assertEquals(1, device.probeCalls)
        assertEquals("target=${device.model.base + 64L}", result.kernelState)
    }

    @Test
    fun closingDiagnosticDeviceAllowsANewLocalSimulation() {
        val device = FakeDeviceKernel()
        val orchestrator = ExploitOrchestrator(device, DiscoveryModule(device), pause = {})
        assertFalse(orchestrator.runFullExploit().success)
        synchronized(SimulationSession.operationLock) { device.closeDevice() }
        val result = orchestrator.runFullExploit()
        assertFalse(device.isProductionMode())
        assertTrue(result.success)
        assertEquals(64L, result.optimalOffset)
        assertEquals(ExploitOrchestrator.Phase.entries.toList(), result.phasesCompleted)
        assertEquals(0, device.probeCalls)
    }

    private fun quietConfig(range: Int = 24, retries: Int = 1) =
        DiscoveryModule.Config(searchRange = range, maxRetries = retries, verbose = false)

    private fun orchestrator(
        kernel: FakeKernel,
        clock: () -> Long = System::nanoTime,
        pause: (Long) -> Unit = {}
    ) = ExploitOrchestrator(kernel, DiscoveryModule(kernel), nanoTime = clock, pause = pause)

    private class FakeKernel(
        val base: Long = 0x10000000L,
        var healthyOffsets: Set<Long> = setOf(64L),
        private val attemptsBeforeAcceptance: Int = 1
    ) : SimulationKernel {
        var target: Long? = null
        val attemptedTargets = mutableListOf<Long>()
        private val attemptCounts = mutableMapOf<Long, Int>()
        var resetCount = 0
        var addressQueries = 0
        var healthChecks = 0
        var onExecute: (Long) -> Unit = {}
        var acceptTarget: (Long) -> Boolean = { true }
        var acceptHealth: () -> Boolean = { true }

        override fun status() = "Fake local simulator"
        override fun getProcInfo(): Long {
            addressQueries++
            return base
        }
        override fun executeBadSpin(desiredAddress: Long): Boolean {
            attemptedTargets.add(desiredAddress)
            val attempts = (attemptCounts[desiredAddress] ?: 0) + 1
            attemptCounts[desiredAddress] = attempts
            target = desiredAddress
            onExecute(desiredAddress)
            return attempts >= attemptsBeforeAcceptance && acceptTarget(desiredAddress)
        }

        override fun checkSystemHealth(): Boolean {
            healthChecks++
            return target?.let { it - base in healthyOffsets && acceptHealth() } ?: false
        }

        override fun resetKernelState() {
            resetCount++
            target = null
        }

        override fun getDetailedProcInfo() = "target=${target ?: "baseline"}"
    }

    private class FakeDeviceKernel(val model: FakeKernel = FakeKernel()) :
        DeviceKernel, SimulationKernel by model {
        private var deviceMode = true
        var probeCalls = 0

        override fun initProductionMode(): Boolean {
            deviceMode = true
            return probeDevice()
        }

        override fun elevateToRoot() = false
        override fun isProductionMode() = deviceMode
        override fun probeDevice(): Boolean {
            probeCalls++
            return deviceMode
        }

        override fun closeDevice() {
            deviceMode = false
            model.resetKernelState()
        }
    }
}
