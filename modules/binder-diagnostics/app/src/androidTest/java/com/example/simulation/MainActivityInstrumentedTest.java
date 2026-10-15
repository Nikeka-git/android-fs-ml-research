package com.example.simulation;

import android.app.Activity;
import android.app.Application;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import android.test.ActivityInstrumentationTestCase2;
import android.view.View;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONObject;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Regression checks for repeated operations and Activity recreation with the real native library. */
@SuppressWarnings("deprecation")
public final class MainActivityInstrumentedTest extends ActivityInstrumentationTestCase2<MainActivity> {
    private static final long OPERATION_TIMEOUT_MS = 15000L;
    private MainActivity current;

    public MainActivityInstrumentedTest() {
        super(MainActivity.class);
    }

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        new NativeCore().closeDevice();
        new NativeCore().resetKernelState();
        current = getActivity();
        getInstrumentation().waitForIdleSync();
        awaitIdle();
    }

    @Override
    protected void tearDown() throws Exception {
        try {
            if (current != null) {
                final Activity activity = current;
                getInstrumentation().runOnMainSync(new Runnable() {
                    @Override
                    public void run() {
                        activity.finish();
                    }
                });
                getInstrumentation().waitForIdleSync();
            }
        } finally {
            super.tearDown();
        }
    }

    public void testRepeatedFullWorkflowAndDiscoveryRemainUsable() throws Exception {
        for (int run = 0; run < 2; run++) {
            startAndAssertBusy(R.id.btn_full_exploit);
            awaitIdle();
            assertTrue("Repeated full workflow failed", new NativeCore().checkSystemHealth());
            String fullReport = resultText();
            assertTrue("Full workflow did not produce a report", fullReport.length() > 0);

            startAndAssertBusy(R.id.btn_discovery);
            awaitIdle();
            assertTrue("Discovery report is missing", resultText().contains("64"));
            assertTrue("Discovery did not replace the previous report", !fullReport.equals(resultText()));
        }
        click(R.id.btn_reset);
        awaitIdle();
        assertFalse(new NativeCore().checkSystemHealth());
        assertEquals(0L, new JSONObject(new NativeCore().getDetailedProcInfo()).getLong("transactionCount"));
    }

    public void testCompletedReportSurvivesRecreation() throws Exception {
        startAndAssertBusy(R.id.btn_full_exploit);
        awaitIdle();
        String completedReport = resultText();
        recreateActivity();
        awaitIdle();
        assertEquals("Recreation lost the completed report", completedReport, resultText());
        startAndAssertBusy(R.id.btn_full_exploit);
        awaitIdle();
        assertTrue("Recreated Activity cannot execute a workflow", new NativeCore().checkSystemHealth());
    }

    public void testRecreationDuringWorkRestoresUsableControls() throws Exception {
        // Start and request recreation on the same UI turn so completion cannot hide the lifecycle case.
        final Instrumentation.ActivityMonitor monitor = getInstrumentation()
                .addMonitor(MainActivity.class.getName(), null, false);
        final MainActivity previous = current;
        try {
            getInstrumentation().runOnMainSync(new Runnable() {
                @Override
                public void run() {
                    button(R.id.btn_full_exploit).performClick();
                    assertControls(false);
                    previous.recreate();
                }
            });
            Activity replacement = getInstrumentation().waitForMonitorWithTimeout(monitor, OPERATION_TIMEOUT_MS);
            assertNotNull("Activity was not recreated", replacement);
            assertNotSame(previous, replacement);
            current = (MainActivity) replacement;
            awaitIdle();
            assertFalse("A stale running message survived recreation",
                    resultText().equals(current.getString(R.string.status_simulation_running)));
            startAndAssertBusy(R.id.btn_full_exploit);
            awaitIdle();
            assertTrue("Workflow failed after recreation during work", new NativeCore().checkSystemHealth());
        } finally {
            getInstrumentation().removeMonitor(monitor);
        }
    }

    public void testDeviceModeControlsProbeResetAndReturnToSimulation() throws Exception {
        getInstrumentation().runOnMainSync(new Runnable() {
            @Override
            public void run() {
                assertFalse(button(R.id.btn_simulation_mode).isEnabled());
            }
        });
        final MainActivity first = current;
        launchAdditionalActivity();
        awaitIdle();
        startAndAssertBusy(R.id.btn_device_mode);
        awaitOperationFinished();
        NativeCore core = new NativeCore();
        final boolean opened = core.isProductionMode();
        assertModeControls(opened);
        assertTrue("Device result must include the actual diagnostic state", resultText().contains("lastError"));
        if (opened) {
            startAndAssertBusy(R.id.btn_device_mode);
            awaitOperationFinished();
            assertTrue(core.isProductionMode());
            assertModeControls(true);
            assertTrue(resultText().contains("protocolVersion"));

            startAndAssertBusy(R.id.btn_reset);
            awaitOperationFinished();
            assertTrue("Reset switched away from the connected diagnostic mode", core.isProductionMode());
            assertModeControls(true);

            recreateActivity();
            awaitOperationFinished();
            assertModeControls(true);
        }

        finishCurrentActivityAndResume(first);
        assertModeControls(opened);
        if (opened) {
            startAndAssertBusy(R.id.btn_simulation_mode);
            awaitIdle();
        }
        assertFalse(core.isProductionMode());
        assertModeControls(false);
        startAndAssertBusy(R.id.btn_full_exploit);
        awaitIdle();
        assertTrue("Returning from diagnostics broke the simulation", core.checkSystemHealth());
    }

    private void launchAdditionalActivity() {
        Instrumentation.ActivityMonitor monitor = getInstrumentation()
                .addMonitor(MainActivity.class.getName(), null, false);
        final MainActivity previous = current;
        try {
            getInstrumentation().runOnMainSync(new Runnable() {
                @Override
                public void run() {
                    previous.startActivity(new Intent(previous, MainActivity.class));
                }
            });
            Activity second = getInstrumentation().waitForMonitorWithTimeout(monitor, OPERATION_TIMEOUT_MS);
            assertNotNull("Second Activity did not start", second);
            assertNotSame(previous, second);
            current = (MainActivity) second;
            getInstrumentation().waitForIdleSync();
        } finally {
            getInstrumentation().removeMonitor(monitor);
        }
    }

    private void finishCurrentActivityAndResume(final MainActivity previous) throws Exception {
        final CountDownLatch resumed = new CountDownLatch(1);
        final Application.ActivityLifecycleCallbacks callbacks = new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityCreated(Activity activity, Bundle state) { }
            @Override public void onActivityStarted(Activity activity) { }
            @Override public void onActivityResumed(Activity activity) {
                if (activity == previous) resumed.countDown();
            }
            @Override public void onActivityPaused(Activity activity) { }
            @Override public void onActivityStopped(Activity activity) { }
            @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) { }
            @Override public void onActivityDestroyed(Activity activity) { }
        };
        final MainActivity finishing = current;
        final Application application = previous.getApplication();
        try {
            getInstrumentation().runOnMainSync(new Runnable() {
                @Override
                public void run() {
                    application.registerActivityLifecycleCallbacks(callbacks);
                    finishing.finish();
                }
            });
            assertTrue("First Activity did not resume", resumed.await(OPERATION_TIMEOUT_MS, TimeUnit.MILLISECONDS));
            current = previous;
            getInstrumentation().waitForIdleSync();
            awaitOperationFinished();
        } finally {
            application.unregisterActivityLifecycleCallbacks(callbacks);
        }
    }

    private void recreateActivity() {
        Instrumentation.ActivityMonitor monitor = getInstrumentation()
                .addMonitor(MainActivity.class.getName(), null, false);
        final MainActivity previous = current;
        try {
            getInstrumentation().runOnMainSync(new Runnable() {
                @Override
                public void run() {
                    previous.recreate();
                }
            });
            Activity replacement = getInstrumentation().waitForMonitorWithTimeout(monitor, OPERATION_TIMEOUT_MS);
            assertNotNull("Activity was not recreated", replacement);
            assertNotSame(previous, replacement);
            current = (MainActivity) replacement;
            getInstrumentation().waitForIdleSync();
        } finally {
            getInstrumentation().removeMonitor(monitor);
        }
    }

    private void startAndAssertBusy(final int id) {
        getInstrumentation().runOnMainSync(new Runnable() {
            @Override
            public void run() {
                assertTrue("Control is disabled before operation", button(id).isEnabled());
                button(id).performClick();
                assertControls(false);
                assertEquals(View.VISIBLE, progress().getVisibility());
            }
        });
    }

    private void click(final int id) {
        getInstrumentation().runOnMainSync(new Runnable() {
            @Override
            public void run() {
                assertTrue(button(id).isEnabled());
                button(id).performClick();
            }
        });
    }

    private void awaitIdle() {
        awaitOperationFinished();
        assertModeControls(false);
    }

    private void awaitOperationFinished() {
        long deadline = SystemClock.uptimeMillis() + OPERATION_TIMEOUT_MS;
        final boolean[] idle = {false};
        while (SystemClock.uptimeMillis() < deadline) {
            getInstrumentation().runOnMainSync(new Runnable() {
                @Override
                public void run() {
                    idle[0] = progress().getVisibility() != View.VISIBLE
                            && button(R.id.btn_reset).isEnabled()
                            && button(R.id.btn_device_mode).isEnabled();
                }
            });
            if (idle[0]) {
                getInstrumentation().waitForIdleSync();
                return;
            }
            SystemClock.sleep(25L);
        }
        fail("Activity remained busy or disabled after " + OPERATION_TIMEOUT_MS + " ms");
    }

    private String resultText() {
        final String[] result = {null};
        getInstrumentation().runOnMainSync(new Runnable() {
            @Override
            public void run() {
                result[0] = ((TextView) current.findViewById(R.id.results_view)).getText().toString();
            }
        });
        return result[0];
    }

    private void assertControls(boolean enabled) {
        assertEquals(enabled, button(R.id.btn_full_exploit).isEnabled());
        assertEquals(enabled, button(R.id.btn_discovery).isEnabled());
        assertEquals(enabled, button(R.id.btn_reset).isEnabled());
        assertEquals(enabled, button(R.id.btn_device_mode).isEnabled());
        assertEquals(enabled, button(R.id.btn_simulation_mode).isEnabled());
    }

    private void assertModeControls(final boolean deviceMode) {
        getInstrumentation().runOnMainSync(new Runnable() {
            @Override
            public void run() {
                assertEquals(!deviceMode, button(R.id.btn_full_exploit).isEnabled());
                assertEquals(!deviceMode, button(R.id.btn_discovery).isEnabled());
                assertTrue(button(R.id.btn_reset).isEnabled());
                assertTrue(button(R.id.btn_device_mode).isEnabled());
                assertEquals(deviceMode, button(R.id.btn_simulation_mode).isEnabled());
            }
        });
    }

    private Button button(int id) {
        return (Button) current.findViewById(id);
    }

    private ProgressBar progress() {
        return (ProgressBar) current.findViewById(R.id.progress_bar);
    }
}
