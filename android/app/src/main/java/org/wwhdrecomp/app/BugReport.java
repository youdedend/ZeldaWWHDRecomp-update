package org.wwhdrecomp.app;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * "Copy save for bug report" (the options menu's Saves tab): saves a fresh portable state where
 * Link stands (states/bugreport.wwstate: the Quest Log's progress and Link's position, no game
 * code or data) and shares it with cking.sav through Android's share menu. The files go through
 * CrashLogProvider, since other apps can't open the app's files folder.
 */
final class BugReport {
    private BugReport() {}

    /** Saves a fresh state at the next frame boundary, then shares it; runs on the UI thread. */
    static void share(Activity a) {
        final String before = Native.saveStateMessage();
        Native.saveBugReportState();
        final long until = android.os.SystemClock.uptimeMillis() + 15000;
        final android.os.Handler h = new android.os.Handler(android.os.Looper.getMainLooper());
        h.postDelayed(new Runnable() {
            @Override
            public void run() {
                String m = Native.saveStateMessage();
                if (!m.isEmpty() && !m.equals(before)) {
                    // "Bug report state saved (...)" vs "can't save ..." / "not saved ..."
                    if (m.startsWith(a.getString(R.string.bugreport_saved)) ) shareFiles(a);
                    else new GameDialog(a).title(R.string.opt_bugreport).message(m).button(R.string.opt_ok, null).show();
                    return;
                }
                if (android.os.SystemClock.uptimeMillis() < until) h.postDelayed(this, 300);
            }
        }, 300);
    }

    private static void shareFiles(Activity a) {
        String[] paths = Native.bugReportFiles();
        List<File> files = new ArrayList<>();
        for (String p : paths)
            if (p != null && !p.isEmpty() && new File(p).isFile()) files.add(new File(p));
        if (files.isEmpty()) {
            new GameDialog(a).title(R.string.opt_bugreport).message(R.string.bugreport_missing).button(R.string.opt_ok, null).show();
            return;
        }
        ArrayList<Uri> uris = new ArrayList<>();
        for (File f : files) uris.add(CrashLogProvider.uri(f));
        Intent send = new Intent(uris.size() == 1 ? Intent.ACTION_SEND : Intent.ACTION_SEND_MULTIPLE);
        send.setType("*/*");  // the state is text, the game save is binary
        if (uris.size() == 1) send.putExtra(Intent.EXTRA_STREAM, uris.get(0));
        else send.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
        ClipData clip = ClipData.newRawUri(files.get(0).getName(), uris.get(0));
        for (int i = 1; i < uris.size(); i++) clip.addItem(new ClipData.Item(uris.get(i)));
        send.setClipData(clip);
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        send.putExtra(Intent.EXTRA_SUBJECT, a.getString(R.string.bugreport_subject));
        send.putExtra(Intent.EXTRA_TEXT, CrashLogs.about(a));
        a.startActivity(Intent.createChooser(send, a.getString(R.string.bugreport_share)));
    }
}
