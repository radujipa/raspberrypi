package com.uom.pimote;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;

/**
 * Created by Tom on 05/08/2013.
 */
public class SensorManagement implements SensorEventListener {
    float sensorX;
    float sensorY;
    SensorManager mSensorManager;
    Sensor mAccelerometer;
    int speed;
    TCPClient tcp;

    public SensorManagement(Context c, int speedValue, TCPClient tcp) {
        this.tcp = tcp;
        mSensorManager = (SensorManager) c.getSystemService(c.SENSOR_SERVICE);
        mAccelerometer = mSensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        speed = speedValue == 1 ? SensorManager.SENSOR_DELAY_NORMAL : SensorManager.SENSOR_DELAY_GAME;
        mSensorManager.registerListener(this, mAccelerometer, speed);
    }

    public void pause() {
        mSensorManager.unregisterListener(this);
    }

    public void resume() {
        mSensorManager.registerListener(this, mAccelerometer, speed);
    }

    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }

    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() != Sensor.TYPE_ACCELEROMETER)
            return;
        sensorX = event.values[0];
        sensorY = event.values[1];
        tcp.sendMessage(Communicator.SEND_DATA + "," + "8827," + sensorX + "," + sensorY);

    }

    public float[] getValues() {
        float[] values = {sensorX, sensorY};
        return values;
    }
}
