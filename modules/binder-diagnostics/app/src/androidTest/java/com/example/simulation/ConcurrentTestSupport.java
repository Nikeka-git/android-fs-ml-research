package com.example.simulation;

import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;

/** A shared work deadline; try-with-resources preserves failures if cleanup also fails. */
final class ConcurrentTestSupport implements AutoCloseable {
    private static final long WORK_TIMEOUT_SECONDS = 60L;
    private static final long CLEANUP_TIMEOUT_SECONDS = 10L;
    private static final String LOG_TAG = "ConcurrentRegression";
    private final String label;
    private final long workDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(WORK_TIMEOUT_SECONDS);
    private final List<Thread> workers = Collections.synchronizedList(new ArrayList<Thread>());
    private final List<Future<?>> futures = new ArrayList<>();
    private final ExecutorService executor;
    private boolean restoreCallerInterrupt;

    ConcurrentTestSupport(int workerCount, final String label) {
        this.label = label;
        final AtomicInteger workerNumber = new AtomicInteger();
        executor = Executors.newFixedThreadPool(workerCount, new ThreadFactory() {
            @Override
            public Thread newThread(Runnable runnable) {
                Thread worker = new Thread(runnable, label + "-" + workerNumber.incrementAndGet());
                workers.add(worker);
                return worker;
            }
        });
    }

    void submit(Callable<Void> task) {
        futures.add(executor.submit(task));
    }

    void awaitAll() throws Exception {
        try {
            for (Future<?> future : futures) {
                long remaining = workDeadline - System.nanoTime();
                if (remaining <= 0L) throw new TimeoutException("Shared work deadline expired");
                future.get(remaining, TimeUnit.NANOSECONDS);
            }
        } catch (TimeoutException timeout) {
            TimeoutException diagnostic = new TimeoutException(label + " exceeded the shared "
                    + WORK_TIMEOUT_SECONDS + "s work deadline.\n" + workerStacks());
            diagnostic.initCause(timeout);
            // Keep evidence available even if a truly blocked JNI call prevents tearDown.
            Log.e(LOG_TAG, diagnostic.getMessage(), diagnostic);
            throw diagnostic;
        } catch (InterruptedException interrupted) {
            restoreCallerInterrupt = true;
            throw interrupted;
        }
    }

    static void checkCancellation() throws InterruptedException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedException("Concurrent regression worker cancelled");
        }
    }

    @Override
    public void close() throws InterruptedException {
        for (Future<?> future : futures) future.cancel(true);
        executor.shutdownNow();
        boolean interrupted = Thread.interrupted();
        interrupted |= restoreCallerInterrupt;
        InterruptedException cleanupInterruption = null;
        long cleanupDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(CLEANUP_TIMEOUT_SECONDS);
        boolean stopped = executor.isTerminated();
        try {
            while (!stopped) {
                long remaining = cleanupDeadline - System.nanoTime();
                if (remaining <= 0L) break;
                try {
                    stopped = executor.awaitTermination(remaining, TimeUnit.NANOSECONDS);
                } catch (InterruptedException error) {
                    interrupted = true;
                    if (cleanupInterruption == null) cleanupInterruption = error;
                }
            }
            if (!stopped) {
                AssertionError failure = new AssertionError(label + " workers did not stop within "
                        + CLEANUP_TIMEOUT_SECONDS + "s after cancellation.\n" + workerStacks());
                if (cleanupInterruption != null) failure.addSuppressed(cleanupInterruption);
                Log.e(LOG_TAG, failure.getMessage(), failure);
                throw failure;
            }
            if (cleanupInterruption != null) throw cleanupInterruption;
        } finally {
            if (interrupted) Thread.currentThread().interrupt();
        }
    }

    private String workerStacks() {
        StringBuilder result = new StringBuilder();
        synchronized (workers) {
            for (Thread worker : workers) {
                result.append(worker.getName()).append(": ").append(worker.getState()).append('\n');
                for (StackTraceElement frame : worker.getStackTrace()) {
                    result.append("  at ").append(frame).append('\n');
                }
            }
        }
        return result.toString();
    }
}
