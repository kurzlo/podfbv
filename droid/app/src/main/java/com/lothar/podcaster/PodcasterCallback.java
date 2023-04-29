package com.lothar.podcaster;

import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.widget.RadioButton;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import org.w3c.dom.Text;

import java.util.stream.Stream;

enum PodcasterMessageType {
    FBV_DEV,
    FBV_RX,
    POD_DEV,
    POD_RX,
    POD_TX,
    CTL_CHN,
}

interface PodcasterCallbackInterface {
    public void send(PodcasterMessageType mtype, String message, int status);
}

class PodcasterCallbackRunnable implements Runnable {
    private PodcasterCallback cbk;
    public PodcasterCallbackRunnable(PodcasterCallback cbk) {
        this.cbk = cbk;
    }
    public void run(PodcasterCallback cbk) {}
    @Override
    public void run() {
        this.run(this.cbk);
    }
}

public class PodcasterCallback implements PodcasterCallbackInterface {

    private static final int MAX_OUTPUT_LINES = 100;
    private static final boolean AUTO_SCROLL_BOTTOM = true;

    private Looper looper;
    public ScrollView scrollView;
    public TextView textView;
    public Switch switchFBV;
    public Switch switchPOD;
    public RadioButton buttonFBV;
    public RadioButton buttonPOD;
    public TextView channelView;

    public PodcasterCallback(ScrollView scrollView, TextView textView, Switch switchFBV, Switch switchPOD, RadioButton buttonFBV, RadioButton buttonPOD, TextView channelView, Looper looper) {
        this.looper = looper;
        this.scrollView = scrollView;
        this.textView = textView;
        this.switchFBV = switchFBV;
        this.switchPOD = switchPOD;
        this.buttonFBV = buttonFBV;
        this.buttonPOD = buttonPOD;
        this.channelView = channelView;
    }

    final private void addLinesToTextView(PodcasterCallback cbk, String lineText) {
        if (cbk.textView != null) {
            cbk.textView.append(lineText + "\n");
            int linesToRemove = cbk.textView.getLineCount() - PodcasterCallback.MAX_OUTPUT_LINES;
            for (int i = 0; i < linesToRemove; i++) {
                Editable text = cbk.textView.getEditableText();
                int lineStart = cbk.textView.getLayout().getLineStart(0);
                int lineEnd = cbk.textView.getLayout().getLineEnd(0);
                text.delete(lineStart, lineEnd);
            }
            if ((cbk.scrollView != null) && PodcasterCallback.AUTO_SCROLL_BOTTOM)
                cbk.scrollView.fullScroll(ScrollView.FOCUS_DOWN);
        }
    }

    @Override
    public void send(final PodcasterMessageType mtype, final String str, final int status) {
        new Handler(this.looper).post(new PodcasterCallbackRunnable(this) {
            @Override
            public void run(PodcasterCallback cbk){
                cbk.addLinesToTextView(cbk,mtype.toString() + ": " + str);
                switch (mtype)
                {
                    case FBV_DEV:
                        //cbk.switchFBV.setChecked(status >= 0);
                        cbk.buttonFBV.setChecked(status >= 0);
                        break;
                    case POD_DEV:
                        //cbk.switchPOD.setChecked(status >= 0);
                        cbk.buttonPOD.setChecked(status >= 0);
                        break;
                    case CTL_CHN:
                        cbk.channelView.setText(Integer.toString(status));
                    default:
                        break;
                }
            }
        });
    }
}
