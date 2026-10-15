package com.example.simulation;

import android.os.Process;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.test.InstrumentationTestCase;

import org.json.JSONObject;

import java.io.File;
import java.math.BigInteger;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Checks the real BINDER_VERSION diagnostic without assuming the app has device access. */
@SuppressWarnings("deprecation")
public final class DeviceModeInstrumentedTest extends InstrumentationTestCase {
    private NativeCore core;

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        core = new NativeCore();
        core.closeDevice();
    }

    @Override
    protected void tearDown() throws Exception {
        try {
            core.closeDevice();
        } finally {
            super.tearDown();
        }
    }

    public void testInitializationAndProbeReportActualConnectionResult() throws Exception {
        assertFalse(core.isProductionMode());
        assertFalse("A closed connection must not probe successfully", core.probeDevice());
        assertTrue(diagnostics(snapshot()).getJSONObject("lastError").getInt("code") != 0);

        boolean opened = core.initProductionMode();
        JSONObject state = snapshot();
        JSONObject device = diagnostics(state);
        assertEquals(opened, core.isProductionMode());
        assertEquals(opened, device.getBoolean("deviceOpen"));
        assertRealIdentity(device);
        assertFalse(device.getBoolean("kernelAddressAvailable"));
        assertFalse(device.getBoolean("rootElevationSupported"));
        if (opened) {
            assertEquals("device_diagnostics", state.getString("mode"));
            assertFalse(state.getBoolean("simulation"));
            assertTrue(device.getString("devicePath").startsWith("/dev/"));
            assertTrue(device.getInt("expectedProtocolVersion") > 0);
            assertEquals(device.getInt("expectedProtocolVersion"), device.getInt("protocolVersion"));
            assertTrue(core.probeDevice());
            device = diagnostics(snapshot());
            assertTrue(device.getBoolean("lastProbeSucceeded"));
            assertEquals(2L, device.getLong("probeCount"));
            assertEquals(2L, device.getLong("successfulProbeCount"));
            assertEquals(0, device.getJSONObject("lastError").getInt("code"));
        } else {
            assertEquals("simulation", state.getString("mode"));
            assertTrue(state.getBoolean("simulation"));
            assertTrue(device.getJSONObject("lastError").getInt("code") != 0);
            assertTrue(device.getJSONObject("lastError").getString("message").length() > 0);
            assertEquals(0x10000000L, core.getProcInfo());
            assertFalse(core.probeDevice());
        }
    }

    public void testDeviceModeNeverClaimsKernelAddressesOrPrivilegeChanges() throws Exception {
        final int uid = Os.getuid();
        final int euid = Os.geteuid();
        final int pid = Process.myPid();
        boolean opened = core.initProductionMode();
        assertFalse("Privilege elevation is unsupported", core.elevateToRoot());
        assertRealIdentity(diagnostics(snapshot()));
        if (opened) {
            assertEquals(0L, core.getProcInfo());
            assertFalse(core.executeBadSpin(0x10000040L));
            assertFalse(core.checkSystemHealth());
            assertTrue(core.isProductionMode());
            assertTrue(diagnostics(snapshot()).getJSONObject("lastError").getInt("code") != 0);
        }
        core.resetKernelState();
        assertEquals(uid, Os.getuid());
        assertEquals(euid, Os.geteuid());
        assertEquals(pid, Process.myPid());
        assertRealIdentity(diagnostics(snapshot()));
    }

    public void testResetPreservesConnectionAndIdentityWhileCloseRestoresSimulation() throws Exception {
        boolean opened = core.initProductionMode();
        JSONObject before = diagnostics(snapshot());
        core.resetKernelState();
        JSONObject after = diagnostics(snapshot());
        assertEquals(opened, core.isProductionMode());
        assertEquals(opened, after.getBoolean("deviceOpen"));
        assertEquals(before.getString("devicePath"), after.getString("devicePath"));
        assertEquals(before.get("protocolVersion"), after.get("protocolVersion"));
        assertRealIdentity(after);
        if (opened) {
            assertEquals(0L, after.getLong("probeCount"));
            assertEquals(0L, after.getLong("successfulProbeCount"));
            assertTrue(core.probeDevice());
            assertEquals(1L, diagnostics(snapshot()).getLong("successfulProbeCount"));
        }
        core.closeDevice();
        assertCleanSimulation();
        core.closeDevice();
        assertCleanSimulation();
    }

    public void testRepeatedInitializationAndCloseDoNotLeakBinderDescriptors() throws Exception {
        int baseline = binderDescriptorCount();
        for (int attempt = 0; attempt < 12; attempt++) {
            boolean opened = core.initProductionMode();
            int afterOpen = binderDescriptorCount();
            assertEquals("Repeated initialization leaked a Binder descriptor",
                    baseline + (opened ? 1 : 0), afterOpen);
            core.closeDevice();
            assertEquals("Closing diagnostics leaked a Binder descriptor", baseline, binderDescriptorCount());
        }

        boolean opened = core.initProductionMode();
        long initialProbes = diagnostics(snapshot()).getLong("probeCount");
        for (int attempt = 0; attempt < 5; attempt++) {
            assertEquals(opened, core.initProductionMode());
            assertEquals(baseline + (opened ? 1 : 0), binderDescriptorCount());
        }
        if (opened) {
            assertEquals(initialProbes + 5L, diagnostics(snapshot()).getLong("probeCount"));
        }
        core.closeDevice();
        assertEquals(baseline, binderDescriptorCount());
    }

    public void testConcurrentModeTogglesProduceCoherentSnapshotsAndCloseAllDescriptors() throws Exception {
        final int baseline = binderDescriptorCount();
        try (ConcurrentTestSupport tasks = new ConcurrentTestSupport(3, "native-modes")) {
            for (int worker = 0; worker < 3; worker++) {
                final int workerNumber = worker;
                tasks.submit(new Callable<Void>() {
                    @Override
                    public Void call() throws Exception {
                        NativeCore bridge = new NativeCore();
                        for (int call = 0; call < 24; call++) {
                            ConcurrentTestSupport.checkCancellation();
                            switch ((call + workerNumber) % 4) {
                                case 0: bridge.initProductionMode(); break;
                                case 1: bridge.probeDevice(); break;
                                case 2: bridge.resetKernelState(); break;
                                default: bridge.closeDevice(); break;
                            }
                            JSONObject state = new JSONObject(bridge.getDetailedProcInfo());
                            JSONObject device = diagnostics(state);
                            assertRealIdentity(device);
                            boolean simulation = state.getBoolean("simulation");
                            assertEquals(simulation ? "simulation" : "device_diagnostics", state.getString("mode"));
                            assertEquals(!simulation, device.getBoolean("deviceOpen"));
                            assertTrue(device.getLong("probeCount") >= device.getLong("successfulProbeCount"));
                            assertTrue(device.getLong("successfulProbeCount") >= 0L);
                        }
                        return null;
                    }
                });
            }
            tasks.awaitAll();
        } finally {
            core.closeDevice();
        }
        assertEquals(baseline, binderDescriptorCount());
        assertCleanSimulation();
    }

    public void testReturningFromDeviceModeStartsWithCleanModelState() throws Exception {
        assertTrue(core.executeBadSpin(0x10000040L));
        assertTrue(core.checkSystemHealth());
        core.initProductionMode();
        core.closeDevice();
        assertCleanSimulation();
        assertTrue(core.executeBadSpin(0x10000040L));
        assertTrue(core.checkSystemHealth());
    }

    public void testRawModeCallsWaitForAnActiveSimulationSession() throws Exception {
        assertTrue(core.executeBadSpin(0x10000040L));
        assertTrue(core.checkSystemHealth());
        final CountDownLatch started = new CountDownLatch(1);
        final CountDownLatch finished = new CountDownLatch(1);
        Thread closer = new Thread(new Runnable() {
            @Override
            public void run() {
                started.countDown();
                try {
                    core.closeDevice();
                } finally {
                    finished.countDown();
                }
            }
        }, "diagnostic-session-lock-test");
        try {
            synchronized (SimulationSession.INSTANCE.getOperationLock()) {
                closer.start();
                assertTrue(started.await(5, TimeUnit.SECONDS));
                long deadline = SystemClock.uptimeMillis() + 5000L;
                while (closer.getState() != Thread.State.BLOCKED && closer.isAlive()
                        && SystemClock.uptimeMillis() < deadline) {
                    SystemClock.sleep(10L);
                }
                assertEquals("A raw mode change bypassed the workflow lock", Thread.State.BLOCKED, closer.getState());
                assertTrue("The blocked mode change reset an active workflow", core.checkSystemHealth());
            }
            assertTrue("The mode change did not finish after releasing the session", finished.await(10, TimeUnit.SECONDS));
            assertCleanSimulation();
        } finally {
            closer.interrupt();
            closer.join(10000L);
            assertFalse("Session lock worker leaked", closer.isAlive());
        }
    }

    private JSONObject snapshot() throws Exception {
        return new JSONObject(core.getDetailedProcInfo());
    }

    private static JSONObject diagnostics(JSONObject state) throws Exception {
        return state.getBoolean("simulation") ? state.getJSONObject("deviceDiagnostics") : state;
    }

    private static void assertRealIdentity(JSONObject device) throws Exception {
        assertEquals(Process.myPid(), device.getInt("pid"));
        assertEquals(Os.getuid(), device.getInt("uid"));
        assertEquals(Os.geteuid(), device.getInt("euid"));
    }

    private void assertCleanSimulation() throws Exception {
        assertFalse(core.isProductionMode());
        JSONObject state = snapshot();
        assertTrue(state.getBoolean("simulation"));
        assertEquals("simulation", state.getString("mode"));
        assertEquals(0x10000000L, core.getProcInfo());
        assertEquals(0L, state.getLong("transactionCount"));
        assertEquals(1L, hex(state.getString("flags")));
        assertEquals(0xAAAAAAAA0000L, hex(state.getString("basePtr")));
        JSONObject device = diagnostics(state);
        assertFalse(device.getBoolean("deviceOpen"));
        assertEquals(0L, device.getLong("probeCount"));
        assertEquals(0L, device.getLong("successfulProbeCount"));
        assertEquals(0, device.getJSONObject("lastError").getInt("code"));
        assertRealIdentity(device);
        assertFalse(core.checkSystemHealth());
    }

    private static long hex(String value) {
        return new BigInteger(value.substring(2), 16).longValue();
    }

    private static int binderDescriptorCount() throws Exception {
        String[] descriptors = new File("/proc/self/fd").list();
        assertNotNull("Cannot inspect the app's own descriptors", descriptors);
        int count = 0;
        for (String descriptor : descriptors) {
            try {
                String target = Os.readlink("/proc/self/fd/" + descriptor);
                if (target.matches("/dev/(?:binderfs/)?(?:binder|hwbinder|vndbinder)")) count++;
            } catch (ErrnoException ignored) {
                // The directory enumeration itself can hold a transient descriptor.
            }
        }
        return count;
    }
}
