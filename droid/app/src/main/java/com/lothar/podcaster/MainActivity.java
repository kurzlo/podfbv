package com.lothar.podcaster;

import androidx.annotation.RequiresApi;
import androidx.appcompat.app.AppCompatActivity;

import android.content.Context;
import android.content.pm.PackageManager;
import android.media.midi.MidiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Looper;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.RadioButton;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.view.View;

import java.io.IOException;

public class MainActivity extends AppCompatActivity {

    @RequiresApi(api = Build.VERSION_CODES.M)
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        if (this.getPackageManager().hasSystemFeature(PackageManager.FEATURE_MIDI)) {
            Podcaster podcaster = Podcaster.getInstance();
            final Switch switchFBV = (Switch) findViewById(R.id.switchFBV);
            final Switch switchPOD = (Switch) findViewById(R.id.switchPOD);
            podcaster.init((MidiManager)this.getSystemService(Context.MIDI_SERVICE),
                    new PodcasterCallback(
                            (ScrollView)findViewById(R.id.scrollView), (TextView)findViewById(R.id.textView),
                            switchFBV, switchPOD,
                            (RadioButton)findViewById(R.id.radioButtonFBV), (RadioButton)findViewById(R.id.radioButtonPOD),
                            (TextView)findViewById(R.id.textViewChannel),
                            Looper.getMainLooper()));
            switchFBV.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
                public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                    Podcaster podcaster = Podcaster.getInstance();
                    podcaster.enableFBV(isChecked);
                }
            });
            switchPOD.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
                public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                    Podcaster podcaster = Podcaster.getInstance();
                    podcaster.enablePOD(isChecked);
                }
            });
            final SeekBar seekBar = (SeekBar) findViewById(R.id.seekBar);
            seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                private int lastProgress = 0;
                @Override
                public void onProgressChanged(SeekBar seekBar, int i, boolean b) {
                    if (b)
                        this.lastProgress = i;
                }
                @Override
                public void onStartTrackingTouch(SeekBar seekBar) {}
                @Override
                public void onStopTrackingTouch(SeekBar seekBar) {
                    Podcaster podcaster = Podcaster.getInstance();
                    try {
                        podcaster.setVolumeThresh(.01f * (float)(this.lastProgress));
                    } catch (IOException e) {
                        e.printStackTrace();
                    }
                }
            });
        }
    }
}
