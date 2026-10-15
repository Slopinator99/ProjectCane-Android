package org.projectcane.cane;

import android.content.Intent;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

/** Hosts the native game. SDL's Java glue loads libSDL2 and libmain (ProjectCane). */
public class CaneActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        DebugLog.start(this);   // per-launch log file for bug reports (see SavesActivity > Save debug log)
        super.onCreate(savedInstanceState);
    }

    /** Called from native code (the Options menu): opens the save backup / restore screen. */
    public void openSavesScreen() {
        runOnUiThread(() -> startActivity(new Intent(this, SavesActivity.class)));
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "avutil", "swscale", "avcodec", "avformat",   // FFmpeg (cutscenes)
            "SDL2",
            "main"
        };
    }
}
