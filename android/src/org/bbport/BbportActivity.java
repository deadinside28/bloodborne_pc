// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
package org.bbport;

import android.os.Build;
import android.os.Bundle;
import android.view.WindowManager;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.libsdl.app.SDLActivity;

/**
 * bbport on Android: SDL's activity running bb-probe (libmain.so, src/android_main.c).
 *
 * The game's files live in this app's external files folder (no permission needed; adb can push
 * there): boot-linked.bin, content.bin and patches.bin as run.sh prepares them on a PC, the game
 * dump in app0/, saves and caches in user/. The recompiled game (libbbaot.so) comes with the APK:
 * Android loads code only from the app's own library folder.
 */
public class BbportActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // The picture also under the camera cutout: the whole panel (3120x1440 on the S25 Ultra).
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowManager.LayoutParams attrs = getWindow().getAttributes();
            attrs.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            getWindow().setAttributes(attrs);
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };  // libmain.so needs libbbgpu.so: loaded together
    }

    @Override
    protected String[] getArguments() {
        final File files = getExternalFilesDir(null);
        final String dir = files.getAbsolutePath();
        final String libs = getApplicationInfo().nativeLibraryDir;
        new File(files, "user").mkdirs();
        // KEY=VALUE before "--": the environment run.sh would set (src/android_main.c); more from
        // the launch (adb shell am start -n org.bbport/.BbportActivity --es env "KEY=VALUE ...")
        final List<String> args = new ArrayList<>();
        args.add("BB_AOT=" + libs + "/libbbaot.so");
        args.add("BB_DATA_DIR=" + dir);
        final String env = getIntent() != null ? getIntent().getStringExtra("env") : null;
        if (env != null) {
            for (String kv : env.trim().split("\\s+")) {
                if (kv.indexOf('=') > 0) args.add(kv);
            }
        }
        args.addAll(Arrays.asList("--",
            dir + "/boot-linked.bin", "--content-profile", dir + "/content.bin",
            "--patches", dir + "/patches.bin", "--app0", dir + "/app0", "--user", dir + "/user",
            "--timeout", "0"));
        return args.toArray(new String[0]);
    }
}
