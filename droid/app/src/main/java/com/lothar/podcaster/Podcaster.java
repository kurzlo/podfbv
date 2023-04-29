package com.lothar.podcaster;

import androidx.annotation.RequiresApi;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Build;
import android.os.Bundle;
import android.os.ConditionVariable;

import java.io.IOException;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;


@RequiresApi(api = Build.VERSION_CODES.M)
class FBVReceiver extends MidiReceiver {
    public void onSend(byte[] data, int offset, int count, long timestamp) throws IOException {
        final Podcaster podcaster = Podcaster.getInstance();
        boolean processed = false;
        int i = offset;
        count += offset;
        byte type = data[i++];
        byte[] buf = new byte[4];
        int len = 0;
        byte channel = 0;
        switch (type)
        {
            case (byte)(0xb0):
                if (i < count) {
                    byte param1 = data[i++];
                    if (i < count) {
                        byte param2 = data[i++];
                        switch (param1)
                        {
                            case 0x07: //vol
                                buf[len++] = type;
                                buf[len++] = param1;
                                buf[len++] = param2;
                                break;
                            case 0x0b: //expr
                                buf[len++] = type;
                                buf[len++] = 0x04;
                                buf[len++] = param2;
                                break;
                            case 0x66: //footsw
                                buf[len++] = (byte)(0xb0);
                                buf[len++] = 0x2b;
                                buf[len++] = (byte)((param2 != 0) ? 0x40 : 0x00);
                                break;
                            default:
                                final byte btnOfst = 0x14;
                                if ((param1 >= btnOfst) && (param1 < (btnOfst + Podcaster.fbvButtons))) {
                                    if (param2 > 0) { //press
                                        param1 -= btnOfst;
                                        byte button = (byte) (podcaster.channel % Podcaster.fbvButtons);
                                        if (param1 == button) { //tap
                                            buf[len++] = (byte) (0xb0);
                                            buf[len++] = 0x40;
                                            buf[len++] = 0x7f;
                                        } else { //channel select
                                            podcaster.lock.lock();
                                            podcaster.channel = (byte) ((byte) (podcaster.channel / Podcaster.fbvButtons) * Podcaster.fbvButtons + param1);
                                            buf[len++] = (byte) (0xc0);
                                            buf[len++] = (byte) ((podcaster.channel % Podcaster.fbvButtons) + 1);
                                            channel = (byte)(podcaster.channel + 1);
                                            podcaster.lock.unlock();
                                        }
                                    } //else: release
                                    processed = true;
                                }
                                break;
                        }
                    }
                }
                if (!processed)
                    processed = len > 0;
            case (byte)(0xc0):
            default:
                break;
        }
        if (podcaster.callback != null) {
            String dstr = Podcaster.toHex(data, offset, count);
            podcaster.callback.send(PodcasterMessageType.FBV_RX, dstr, processed ? 0 : -1);
            if (channel > 0)
                podcaster.callback.send(PodcasterMessageType.CTL_CHN, Integer.toString(channel), (int)(channel));
        }
        if (len > 0) {

            podcaster.lock.lock();
            if (podcaster.inpPOD != null) {
                podcaster.inpPOD.send(buf, 0, len);
            }
            podcaster.lock.unlock();

            if (podcaster.callback != null) {
                String dstr = Podcaster.toHex(buf, 0, len);
                podcaster.callback.send(PodcasterMessageType.POD_TX, dstr, 0);
            }

        }
    }
}

@RequiresApi(api = Build.VERSION_CODES.M)
class PODReceiver extends MidiReceiver {
    public void onSend(byte[] data, int offset, int count, long timestamp) throws IOException {
        final Podcaster podcaster = Podcaster.getInstance();
        boolean processed = false;
        int i = offset;
        count += offset;
        byte type = data[i++];
        byte channel = 0;
        switch (type) {
            case (byte)(0xc0):
                if (i < count) {
                    byte param1 = data[i++];
                    podcaster.lock.lock();
                    podcaster.channel = (byte)(param1 - 1);
                    channel = param1;
                    podcaster.lock.unlock();
                    processed = true;
                }
            default:
                break;
        }
        if (podcaster.callback != null) {
            String dstr = Podcaster.toHex(data, offset, count);
            podcaster.callback.send(PodcasterMessageType.POD_RX, dstr, processed ? 0 : -1);
            if (channel > 0)
                podcaster.callback.send(PodcasterMessageType.CTL_CHN, Integer.toString(channel), (int)(channel));
        }
    }
}

public class Podcaster {

    private static Podcaster instance = null;

    protected Podcaster() {
    }

    public static Podcaster getInstance() {
        if (instance == null) {
            instance = new Podcaster();
        }
        return instance;
    }

    final public static String manufacturer = "Line 6";
    final public static String productFBV = "FBV Express Mk II";
    final public static String productPOD = "Line 6 Pocket POD";

    final public static byte fbvButtons = 4;

    static String toHex(byte []b, int offset, int count) {
        String str = "0x";
        for (int i = offset; i < count; i++) {
            int []digits = { (b[i] >> 4) & 0xf, b[i] & 0xf };
            for (int j = 0; j < 2; j++) {
                str += Integer.toHexString(digits[j]);
            }
        }
        return str;
    };

    PodcasterCallback callback = null;

    public Lock lock = null;

    public MidiManager mm = null;

    public boolean FBVenabled = false;
    public boolean PODenabled = false;

    public MidiDevice devFBV = null;
    public MidiOutputPort outFBV = null;
    public MidiDevice devPOD = null;
    public MidiInputPort inpPOD = null;
    public MidiOutputPort outPOD = null;

    public byte channel = 0;

    @RequiresApi(api = Build.VERSION_CODES.M)
    final public void init(MidiManager mm, PodcasterCallback callback) {

        final Podcaster podcaster = Podcaster.getInstance();

        this.callback = callback;

        this.lock = new ReentrantLock();

        this.mm = mm;

        mm.registerDeviceCallback(new MidiManager.DeviceCallback() {
            public void onDeviceAdded(MidiDeviceInfo info) {
                podcaster.addDeviceHandler(info);
            }
            public void onDeviceRemoved(MidiDeviceInfo info) {
                podcaster.removeDeviceHandler(info);
            }
        }, null);
    }

    @RequiresApi(api = Build.VERSION_CODES.M)
    final public boolean enableSpecific(boolean enable, String manufacturer, String product)
    {
        final Podcaster podcaster = Podcaster.getInstance();
        boolean available = false;
        MidiDeviceInfo[] infos = podcaster.mm.getDevices();
        for (int i = 0; i < infos.length; i++) {
            MidiDeviceInfo info = infos[i];
            Bundle properties = info.getProperties();
            String man = properties.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER);
            if (man.equals(manufacturer)) {
                String prod = properties.getString(MidiDeviceInfo.PROPERTY_PRODUCT);
                if (prod.equals(product)) {
                    if (enable)
                        podcaster.addDeviceHandler(info);
                    else
                        podcaster.removeDeviceHandler(info);
                    available = true;
                }
            }
        }
        return available;
    }

    @RequiresApi(api = Build.VERSION_CODES.M)
    final public void enableFBV(boolean enable)
    {
        final Podcaster podcaster = Podcaster.getInstance();
        if (enable != podcaster.FBVenabled) {
            podcaster.FBVenabled = enable;
            if (!enableSpecific(enable, Podcaster.manufacturer, Podcaster.productFBV) && enable && (podcaster.callback != null))
                podcaster.callback.send(PodcasterMessageType.FBV_DEV, "FBV not available", -1);
        }
    }

    @RequiresApi(api = Build.VERSION_CODES.M)
    final public void enablePOD(boolean enable)
    {
        final Podcaster podcaster = Podcaster.getInstance();
        if (enable != podcaster.PODenabled) {
            podcaster.PODenabled = enable;
            if (!enableSpecific(enable, Podcaster.manufacturer, Podcaster.productPOD) && enable && (podcaster.callback != null))
                podcaster.callback.send(PodcasterMessageType.POD_DEV, "POD not available", -1);
        }
    }

    @RequiresApi(api = Build.VERSION_CODES.M)
    final public void addDeviceHandler(MidiDeviceInfo info) {
        final Podcaster podcaster = Podcaster.getInstance();
        podcaster.lock.lock();
        final boolean fbvWasEnabled = podcaster.outFBV != null;
        final boolean podWasEnabled = podcaster.devPOD != null;
        podcaster.lock.unlock();
        if (podcaster.FBVenabled || podcaster.PODenabled)
        {
            Bundle properties = info.getProperties();
            final String man = properties.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER);
            if (man.equals(podcaster.manufacturer)) {
                String prod = properties.getString(MidiDeviceInfo.PROPERTY_PRODUCT);
                if (podcaster.FBVenabled && prod.equals(podcaster.productFBV)) {
                    final ConditionVariable cond = new ConditionVariable();
                    podcaster.mm.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
                        @Override
                        public void onDeviceOpened(MidiDevice device) {
                            MidiDeviceInfo.PortInfo[] portInfos = device.getInfo().getPorts();
                            int i;
                            for (i = 0; i < portInfos.length; i++) {
                                MidiDeviceInfo.PortInfo portInfo = portInfos[i];
                                if (portInfo.getType() == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                                    break;
                                }
                            }
                            if (i < portInfos.length) {
                                MidiDeviceInfo.PortInfo portInfo = portInfos[i];
                                MidiOutputPort outPort = device.openOutputPort(portInfo.getPortNumber());
                                if (outPort != null)
                                    outPort.connect(new FBVReceiver());
                                podcaster.lock.lock();
                                podcaster.outFBV = outPort;
                                podcaster.devFBV = device;
                                podcaster.lock.unlock();
                            }
                            cond.open();
                        }
                    }, null);
                    cond.block();
                } else if (podcaster.PODenabled && prod.equals(podcaster.productPOD)) {
                    final ConditionVariable cond = new ConditionVariable();
                    podcaster.mm.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
                        @Override
                        public void onDeviceOpened(MidiDevice device) {
                            MidiDeviceInfo.PortInfo[] portInfos = device.getInfo().getPorts();
                            int i, j;
                            for (i = 0; i < portInfos.length; i++) {
                                MidiDeviceInfo.PortInfo portInfo = portInfos[i];
                                if (portInfo.getType() == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                                    break;
                                }
                            }
                            for (j = 0; j < portInfos.length; j++) {
                                MidiDeviceInfo.PortInfo portInfo = portInfos[j];
                                if (portInfo.getType() == MidiDeviceInfo.PortInfo.TYPE_INPUT) {
                                    break;
                                }
                            }
                            if ((i < portInfos.length) && (j < portInfos.length)) {
                                MidiDeviceInfo.PortInfo outPortInfo = portInfos[i];
                                MidiOutputPort outPort = device.openOutputPort(outPortInfo.getPortNumber());
                                MidiDeviceInfo.PortInfo inpPortInfo = portInfos[j];
                                MidiInputPort inpPort = device.openInputPort(inpPortInfo.getPortNumber());
                                outPort.connect(new PODReceiver());
                                podcaster.lock.lock();
                                podcaster.outPOD = outPort;
                                podcaster.inpPOD = inpPort;
                                podcaster.devPOD = device;
                                podcaster.lock.unlock();
                            }
                            cond.open();
                        }
                    }, null);
                    cond.block();
                }
            }
        }
        podcaster.lock.lock();
        final boolean fbvIsEnabled = podcaster.devFBV != null;
        final boolean podIsEnabled = podcaster.devPOD != null;
        podcaster.lock.unlock();
        if (podcaster.callback != null) {
            final int fbvStat = fbvIsEnabled ? 0 : -1;
            if (fbvIsEnabled != podcaster.FBVenabled)
                podcaster.callback.send(PodcasterMessageType.FBV_DEV, "Failed to connect to FBV", fbvStat);
            else if (fbvIsEnabled && !fbvWasEnabled)
                podcaster.callback.send(PodcasterMessageType.FBV_DEV, "FBV connected", fbvStat);
            final int podStat = podIsEnabled ? 0 : -1;
            if (podIsEnabled != podcaster.PODenabled)
                podcaster.callback.send(PodcasterMessageType.POD_DEV, "Failed to connect to POD", podStat);
            else if (podIsEnabled && !podWasEnabled)
                podcaster.callback.send(PodcasterMessageType.POD_DEV, "POD connected", podStat);
        }
    }

    @RequiresApi(api = Build.VERSION_CODES.M)
    final public void removeDeviceHandler(MidiDeviceInfo info) {
        final Podcaster podcaster = Podcaster.getInstance();
        Bundle properties = info.getProperties();
        String man = properties.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER);
        if (man.equals(podcaster.manufacturer)) {
            String prod = properties.getString(MidiDeviceInfo.PROPERTY_PRODUCT);
            if (prod.equals(podcaster.productFBV)) {
                podcaster.lock.lock();
                if (podcaster.outFBV != null) {
                    try {
                        podcaster.outFBV.close();
                    } catch (IOException e) {
                        e.printStackTrace();
                    }
                }
                podcaster.outFBV = null;
                if (podcaster.devFBV != null) {
                    try {
                        podcaster.devFBV.close();
                    } catch (IOException e) {
                        e.printStackTrace();
                    }
                }
                podcaster.devFBV = null;
                if (podcaster.callback != null)
                    podcaster.callback.send(PodcasterMessageType.FBV_DEV, "FBV disconnected",-1);
                podcaster.lock.unlock();
            } else if (prod.equals(podcaster.productPOD)) {
                podcaster.lock.lock();
                if (podcaster.outPOD != null) {
                    try {
                        podcaster.outPOD.close();
                    } catch (IOException e) {
                        e.printStackTrace();
                    }
                }
                podcaster.outPOD = null;
                if (podcaster.inpPOD != null) {
                    try {
                        podcaster.inpPOD.close();
                    } catch (IOException e) {
                        e.printStackTrace();
                    }
                }
                podcaster.inpPOD = null;
                if (podcaster.devPOD != null) {
                    try {
                        podcaster.devPOD.close();
                    } catch (IOException e) {
                        e.printStackTrace();
                    }
                }
                podcaster.devPOD = null;
                if (podcaster.callback != null)
                    podcaster.callback.send(PodcasterMessageType.POD_DEV, "POD disconnected",-1);
                podcaster.lock.unlock();
            }
        }
    }
}
