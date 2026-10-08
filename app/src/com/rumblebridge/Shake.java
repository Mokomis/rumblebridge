package com.rumblebridge;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.HandlerThread;

/** Diagnostic only: how much the tablet is shaking, so a test can tell whether the Kishi buzzed. */
final class Shake {
    /** RMS of sample-to-sample change in acceleration (m/s^2) and rotation (rad/s) over the window. */
    static String measure(Context context, int ms) throws InterruptedException {
        SensorManager sm = context.getSystemService(SensorManager.class);
        final double[] sum = new double[2];
        final int[] n = new int[2];
        final float[][] last = new float[2][];
        final double[] axisSum = new double[3], axisSq = new double[3];
        final int[] samples = new int[1];
        SensorEventListener l = new SensorEventListener() {
            @Override public void onSensorChanged(SensorEvent e) {
                int i = e.sensor.getType() == Sensor.TYPE_ACCELEROMETER ? 0 : 1;
                synchronized (sum) {
                    if (i == 0) {
                        for (int k = 0; k < 3; k++) { axisSum[k] += e.values[k]; axisSq[k] += e.values[k] * e.values[k]; }
                        samples[0]++;
                    }
                    if (last[i] != null) {
                        double d = 0;
                        for (int k = 0; k < 3; k++) d += (e.values[k] - last[i][k]) * (e.values[k] - last[i][k]);
                        sum[i] += d;
                        n[i]++;
                    }
                    last[i] = e.values.clone();
                }
            }
            @Override public void onAccuracyChanged(Sensor sensor, int accuracy) { }
        };
        HandlerThread t = new HandlerThread("shake");
        t.start();
        Handler h = new Handler(t.getLooper());
        sm.registerListener(l, sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER), 5000, h);
        sm.registerListener(l, sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE), 5000, h);
        Thread.sleep(ms);
        sm.unregisterListener(l);
        t.quitSafely();
        synchronized (sum) {
            // "accel" is the RMS change between samples, which favours fast shaking; "level" is the
            // spread of the acceleration itself, which does not.
            double var = 0;
            int m = Math.max(1, samples[0]);
            for (int k = 0; k < 3; k++) var += axisSq[k] / m - (axisSum[k] / m) * (axisSum[k] / m);
            return String.format("vib accel %.4f (%d) gyro %.5f (%d) level %.4f",
                Math.sqrt(sum[0] / Math.max(1, n[0])), n[0], Math.sqrt(sum[1] / Math.max(1, n[1])), n[1], Math.sqrt(Math.max(0, var)));
        }
    }
}
