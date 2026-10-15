package com.example.simulation

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import java.util.concurrent.Executors
import java.util.concurrent.Future

/** Runs a local model and Binder connection diagnostics on one background queue. */
class MainActivity : Activity() {
    private lateinit var nativeCore: NativeCore
    private lateinit var orchestrator: ExploitOrchestrator
    private lateinit var statusView: TextView
    private lateinit var resultsView: TextView
    private lateinit var progressBar: ProgressBar
    private lateinit var controls: List<Button>

    private val worker = Executors.newSingleThreadExecutor()
    private var activeTask: Future<*>? = null
    private var busy = false
    private var nativeReady = false
    private var diagnosticMode = false
    private var refreshingMode = false
    private var pendingModeRefresh = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        statusView = findViewById(R.id.native_status)
        resultsView = findViewById(R.id.results_view)
        progressBar = findViewById(R.id.progress_bar)
        controls = listOf(
            findViewById(R.id.btn_discovery),
            findViewById(R.id.btn_full_exploit),
            findViewById(R.id.btn_reset),
            findViewById(R.id.btn_device_mode),
            findViewById(R.id.btn_simulation_mode)
        )
        setBusy(false)

        if (savedInstanceState != null) {
            resultsView.text = if (savedInstanceState.getBoolean(STATE_BUSY)) {
                getString(R.string.operation_interrupted)
            } else {
                savedInstanceState.getString(STATE_RESULTS)
                    ?: getString(R.string.results_placeholder)
            }
        }

        try {
            nativeCore = NativeCore()
            orchestrator = ExploitOrchestrator(nativeCore, DiscoveryModule(nativeCore))
            nativeReady = true
        } catch (error: LinkageError) {
            showInitializationError(error)
        } catch (error: RuntimeException) {
            showInitializationError(error)
        }

        controls[0].setOnClickListener { runDiscoveryPhase() }
        controls[1].setOnClickListener { runFullSimulation() }
        controls[2].setOnClickListener { resetSimulation() }
        controls[3].setOnClickListener { runDeviceDiagnostics() }
        controls[4].setOnClickListener { returnToSimulation() }
        refreshNativeMode()
    }

    override fun onResume() {
        super.onResume()
        if (nativeReady && busy) {
            // An in-flight snapshot may predate a different Activity's change.
            pendingModeRefresh = true
        } else {
            refreshNativeMode()
        }
    }

    // Another Activity can change the process-wide mode. Refresh off the main
    // thread because another workflow may currently hold the shared monitor.
    private fun refreshNativeMode() = runOperation(null) { null }

    private fun runDiscoveryPhase() = runOperation(R.string.status_discovery_started) {
        val offset = orchestrator.runDiscoveryOnly()
        getString(R.string.discovery_report, offset, nativeCore.getDetailedProcInfo())
    }

    private fun runFullSimulation() = runOperation(R.string.status_simulation_running) {
        val result = orchestrator.runFullExploit()
        getString(
            R.string.simulation_report,
            getString(if (result.success) R.string.result_yes else R.string.result_no),
            result.optimalOffset?.toString() ?: getString(R.string.offset_not_found),
            result.phasesCompleted.size,
            result.executionTimeMillis,
            result.kernelState,
            result.logs.joinToString("\n")
        )
    }

    private fun resetSimulation() = runOperation(R.string.status_resetting) {
        nativeCore.resetKernelState()
        getString(R.string.reset_report, nativeCore.getDetailedProcInfo())
    }

    private fun runDeviceDiagnostics() = runOperation(R.string.status_device_running) {
        val connected = if (nativeCore.isProductionMode()) {
            nativeCore.probeDevice()
        } else {
            nativeCore.initProductionMode()
        }
        getString(
            R.string.device_report,
            getString(if (connected) R.string.device_connected else R.string.device_unavailable),
            nativeCore.getDetailedProcInfo()
        )
    }

    private fun returnToSimulation() = runOperation(R.string.status_switching_to_simulation) {
        nativeCore.closeDevice()
        getString(R.string.simulation_mode_report, nativeCore.getDetailedProcInfo())
    }

    /** A single queue plus the shared session lock protects multi-call JNI operations. */
    private fun runOperation(loadingMessage: Int?, operation: () -> String?) {
        if (busy || !nativeReady || isDestroyed || isFinishing) return
        val refreshOnly = loadingMessage == null
        refreshingMode = refreshOnly
        setBusy(true)
        if (loadingMessage != null) resultsView.setText(loadingMessage)
        activeTask = worker.submit {
            var failed = false
            var nativeFailure: LinkageError? = null
            var operationError: Throwable? = null
            var updatedStatus: String? = null
            var updatedMode: Boolean? = null
            val report = try {
                synchronized(SimulationSession.operationLock) {
                    if (Thread.currentThread().isInterrupted) throw InterruptedException()
                    try {
                        operation()
                    } finally {
                        updatedMode = nativeCore.isProductionMode()
                        updatedStatus = nativeCore.status()
                    }
                }
            } catch (error: InterruptedException) {
                Thread.currentThread().interrupt()
                failed = true
                operationError = error
                getString(R.string.operation_interrupted)
            } catch (error: Exception) {
                Log.e(LOG_TAG, "Simulation operation failed", error)
                failed = true
                operationError = error
                getString(R.string.operation_failed, error.message ?: error.javaClass.simpleName)
            } catch (error: LinkageError) {
                Log.e(LOG_TAG, "Native call failed", error)
                failed = true
                operationError = error
                nativeFailure = error
                getString(R.string.operation_failed, error.message ?: error.javaClass.simpleName)
            }
            runOnUiThread {
                if (!isDestroyed && !isFinishing) {
                    if (nativeFailure != null || (refreshOnly && failed)) {
                        nativeReady = false
                        statusView.text = getString(R.string.native_error, operationError?.message)
                    } else {
                        updatedMode?.let { diagnosticMode = it }
                        updatedStatus?.let { statusView.text = getString(R.string.native_status, it) }
                    }
                    if (report != null) resultsView.text = report
                    refreshingMode = false
                    setBusy(false)
                    activeTask = null
                    if (failed) {
                        Toast.makeText(this, R.string.status_error, Toast.LENGTH_LONG).show()
                    }
                    if (pendingModeRefresh) {
                        pendingModeRefresh = false
                        refreshNativeMode()
                    }
                }
            }
        }
    }

    private fun setBusy(value: Boolean) {
        busy = value
        progressBar.visibility = if (value) View.VISIBLE else View.GONE
        val enabled = nativeReady && !value
        controls[0].isEnabled = enabled && !diagnosticMode
        controls[1].isEnabled = enabled && !diagnosticMode
        controls[2].isEnabled = enabled
        controls[3].isEnabled = enabled
        controls[4].isEnabled = enabled && diagnosticMode
    }

    private fun showInitializationError(error: Throwable) {
        Log.e(LOG_TAG, "Native simulation initialization failed", error)
        nativeReady = false
        setBusy(false)
        statusView.text = getString(R.string.native_error, error.message ?: error.javaClass.simpleName)
        resultsView.setText(R.string.initialization_failed)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString(STATE_RESULTS, resultsView.text.toString())
        outState.putBoolean(STATE_BUSY, busy && !refreshingMode)
        super.onSaveInstanceState(outState)
    }

    override fun onDestroy() {
        activeTask?.cancel(true)
        worker.shutdownNow()
        super.onDestroy()
    }

    private companion object {
        const val LOG_TAG = "NativeSimulation"
        const val STATE_RESULTS = "simulation.results"
        const val STATE_BUSY = "simulation.busy"
    }
}
