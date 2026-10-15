package com.example.simulation;

import android.test.InstrumentationTestCase;

import org.json.JSONObject;

import java.math.BigInteger;
import java.util.concurrent.Callable;

/** Exercises the packaged library through JNI rather than replacing native calls with a fake. */
@SuppressWarnings("deprecation")
public final class NativeCoreInstrumentedTest extends InstrumentationTestCase {
    private static final long PROC_ADDRESS = 0x10000000L;
    private static final long INITIAL_BASE_PTR = 0xAAAAAAAA0000L;
    private static final long INITIAL_LOCK_STATE = 0xDEADBEEFCAFE0001L;
    private NativeCore core;

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        core = new NativeCore();
        core.closeDevice();
        core.resetKernelState();
    }

    @Override
    protected void tearDown() throws Exception {
        try {
            core.closeDevice();
            core.resetKernelState();
        } finally {
            super.tearDown();
        }
    }

    public void testBaselineHasDocumentedSimulationMetadata() throws Exception {
        assertTrue(core.status().length() > 0);
        assertEquals(PROC_ADDRESS, core.getProcInfo());
        JSONObject state = state();
        assertTrue(state.getBoolean("simulation"));
        assertEquals(64, state.getLong("expectedOffset"));
        assertEquals(256, state.getLong("searchRange"));
        assertEquals(8, state.getLong("addressAlignment"));
        assertBaseline(state);
        assertFalse(core.checkSystemHealth());
        assertBaseline(state());
    }

    public void testInvalidTargetsAreRejectedWithoutChangingAnyState() throws Exception {
        long[] invalidTargets = {
                0L, -1L, Long.MIN_VALUE, Long.MAX_VALUE,
                PROC_ADDRESS - 264L, PROC_ADDRESS + 264L,
                PROC_ADDRESS + 1L, PROC_ADDRESS + 63L
        };
        // Also exercise rejection while there is a successful result to preserve.
        assertTrue(core.executeBadSpin(PROC_ADDRESS + 64L));
        assertTrue(core.checkSystemHealth());
        String before = core.getDetailedProcInfo();
        for (long target : invalidTargets) {
            assertFalse("Accepted invalid target " + target, core.executeBadSpin(target));
            assertEquals("Rejected input mutated state", before, core.getDetailedProcInfo());
        }
    }

    public void testAllowedRangeBoundariesDoNotClaimASuccessfulSimulation() throws Exception {
        long[] offsets = {-256L, 0L, 8L, 56L, 72L, 256L};
        for (long offset : offsets) {
            core.resetKernelState();
            long target = PROC_ADDRESS + offset;
            assertTrue("Rejected valid aligned target", core.executeBadSpin(target));
            assertFalse("Wrong offset passed health", core.checkSystemHealth());
            JSONObject state = state();
            assertEquals(target, hex(state, "basePtr"));
            assertEquals(1L, state.getLong("transactionCount"));
            assertEquals(3L, hex(state, "flags"));
            assertEquals(1, state.getInt("referenceCount"));
        }
    }

    public void testSuccessfulAttackHealthAndResetRoundTrip() throws Exception {
        assertTrue(core.executeBadSpin(PROC_ADDRESS + 64L));
        JSONObject applied = state();
        assertEquals(PROC_ADDRESS + 64L, hex(applied, "basePtr"));
        assertEquals((INITIAL_LOCK_STATE & 0xFFFFFFFF00000000L) | (PROC_ADDRESS + 64L),
                hex(applied, "innerLockState"));
        assertEquals(3L, hex(applied, "flags"));
        assertEquals(1L, applied.getLong("transactionCount"));
        assertTrue(core.checkSystemHealth());
        assertEquals(7L, hex(state(), "flags"));
        assertTrue(core.checkSystemHealth());
        assertEquals(1L, state().getLong("transactionCount"));
        core.resetKernelState();
        assertBaseline(state());
        assertFalse(core.checkSystemHealth());
    }

    public void testNewAttackRequiresANewHealthCheck() throws Exception {
        assertTrue(core.executeBadSpin(PROC_ADDRESS + 64L));
        assertTrue(core.checkSystemHealth());
        assertTrue(core.executeBadSpin(PROC_ADDRESS + 64L));
        assertEquals(3L, hex(state(), "flags"));
        assertEquals(2L, state().getLong("transactionCount"));
        assertTrue(core.checkSystemHealth());
        assertTrue(core.executeBadSpin(PROC_ADDRESS));
        assertFalse(core.checkSystemHealth());
        assertEquals(3L, hex(state(), "flags"));
    }

    public void testStateIsSharedBetweenBridgeInstancesAndCanBeReset() throws Exception {
        NativeCore second = new NativeCore();
        assertTrue(core.executeBadSpin(PROC_ADDRESS + 64L));
        assertTrue(second.checkSystemHealth());
        assertEquals(core.getDetailedProcInfo(), second.getDetailedProcInfo());
        second.resetKernelState();
        assertBaseline(state());
    }

    public void testConcurrentTransactionsAreNotLost() throws Exception {
        final int workerCount = 4;
        final int callsPerWorker = 100;
        try (ConcurrentTestSupport tasks = new ConcurrentTestSupport(workerCount, "native-transactions")) {
            for (int worker = 0; worker < workerCount; worker++) {
                tasks.submit(new Callable<Void>() {
                    @Override
                    public Void call() throws Exception {
                        NativeCore bridge = new NativeCore();
                        for (int call = 0; call < callsPerWorker; call++) {
                            ConcurrentTestSupport.checkCancellation();
                            assertTrue(bridge.executeBadSpin(PROC_ADDRESS + 64L));
                        }
                        return null;
                    }
                });
            }
            tasks.awaitAll();
            assertEquals((long) workerCount * callsPerWorker, state().getLong("transactionCount"));
            assertTrue(core.checkSystemHealth());
        }
    }

    public void testConcurrentSnapshotsNeverContainHalfResetState() throws Exception {
        try (ConcurrentTestSupport tasks = new ConcurrentTestSupport(2, "native-snapshots")) {
            tasks.submit(new Callable<Void>() {
                @Override
                public Void call() throws Exception {
                    for (int call = 0; call < 250; call++) {
                        ConcurrentTestSupport.checkCancellation();
                        core.resetKernelState();
                        core.executeBadSpin(PROC_ADDRESS + 64L);
                        core.checkSystemHealth();
                    }
                    return null;
                }
            });
            tasks.submit(new Callable<Void>() {
                @Override
                public Void call() throws Exception {
                    for (int call = 0; call < 250; call++) {
                        ConcurrentTestSupport.checkCancellation();
                        JSONObject snapshot = state();
                        assertEquals(PROC_ADDRESS, hex(snapshot, "procAddress"));
                        assertEquals(1, snapshot.getInt("referenceCount"));
                        if (snapshot.getLong("transactionCount") == 0L) {
                            assertBaseline(snapshot);
                        } else {
                            assertEquals(1L, snapshot.getLong("transactionCount"));
                            assertEquals(PROC_ADDRESS + 64L, hex(snapshot, "basePtr"));
                            long flags = hex(snapshot, "flags");
                            assertTrue(flags == 3L || flags == 7L);
                        }
                    }
                    return null;
                }
            });
            tasks.awaitAll();
        }
    }

    private JSONObject state() throws Exception {
        return new JSONObject(core.getDetailedProcInfo());
    }

    private static void assertBaseline(JSONObject state) throws Exception {
        assertEquals(PROC_ADDRESS, hex(state, "procAddress"));
        assertEquals(INITIAL_BASE_PTR, hex(state, "basePtr"));
        assertEquals(INITIAL_LOCK_STATE, hex(state, "innerLockState"));
        assertEquals(0L, state.getLong("transactionCount"));
        assertEquals(1L, hex(state, "flags"));
        assertEquals(1, state.getInt("referenceCount"));
    }

    private static long hex(JSONObject state, String key) throws Exception {
        String value = state.getString(key);
        assertTrue("Invalid hex field " + key + ": " + value, value.matches("0x[0-9a-fA-F]{16}"));
        return new BigInteger(value.substring(2), 16).longValue();
    }
}
