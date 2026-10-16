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

    /** SDL calls finish() when the game exits; also remove the card from the Recents list so no stale "active app" is left. */
    @Override
    public void finish() {
        finishAndRemoveTask();
    }

    /**
     * "Exit Game" ends the native main loop and SDL finishes the activity, but the process (and the game's global state) would
     * stay alive, and tapping the icon again started a second run on top of it. End the process with the activity.
     */
    @Override
    protected void onDestroy() {
        final boolean finishing = isFinishing();
        super.onDestroy();
        if (finishing)
            android.os.Process.killProcess(android.os.Process.myPid());
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
