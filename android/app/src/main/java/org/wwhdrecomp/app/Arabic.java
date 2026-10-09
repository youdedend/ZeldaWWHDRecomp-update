package org.wwhdrecomp.app;

import android.content.Context;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The Arabic fan translation (docs/rtl-text.md): the translation's .zip, picked by the user, is
 * unpacked here; the game then loads its text and title logo from these files when the Arabic
 * setting is on, instead of the game dump's own files.
 */
final class Arabic {
    private Arabic() {}

    /** the translation's folder: next to files/save, where the native side looks for it */
    static File dir(Context c) {
        File f = c.getExternalFilesDir(null);
        return new File(f != null ? f : c.getFilesDir(), "arabic");
    }

    /**
     * Unpacks a translation .zip (Hesham's): its permanent_2d_*.pack and Title_00.szs, wherever
     * they sit in the archive. A description of what was installed, or "!reason" when it failed.
     */
    static String install(Context c, Uri uri) {
        File tmp = new File(dir(c), ".install");
        Backup.deleteTree(tmp);
        if (!tmp.mkdirs()) return "!cannot create " + tmp;
        String pack = null, title = null;
        long total = 0;
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("no data");
            ZipInputStream zip = new ZipInputStream(in);
            byte[] buf = new byte[1 << 16];
            for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                String n = e.getName();
                int slash = Math.max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
                String base = slash < 0 ? n : n.substring(slash + 1);
                boolean isPack = base.startsWith("permanent_2d_") && base.endsWith(".pack");
                if (e.isDirectory() || !(isPack || base.equals("Title_00.szs"))) continue;
                if ((isPack && pack != null) || (!isPack && title != null)) continue;  // first one wins
                try (OutputStream out = new FileOutputStream(new File(tmp, base))) {
                    for (int k; (k = zip.read(buf)) > 0; ) {
                        total += k;
                        if (total > (128L << 20)) throw new IOException("too large");
                        out.write(buf, 0, k);
                    }
                }
                if (isPack) pack = base;
                else title = base;
            }
        } catch (IOException e) {
            Backup.deleteTree(tmp);
            return "!" + e.getMessage();
        }
        if (pack == null) {
            Backup.deleteTree(tmp);
            return "!" + c.getString(R.string.arabic_not_package);
        }
        File dst = dir(c);
        //noinspection ResultOfMethodCallIgnored
        dst.mkdirs();
        // the old translation goes only once the new files are complete (a stale pack with another
        // region's name must not stay behind: the game loads the folder's only pack)
        File[] old = dst.listFiles((d, n) -> n.startsWith("permanent_2d_") && n.endsWith(".pack") || n.equals("Title_00.szs"));
        if (old != null)
            for (File f : old) {
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        //noinspection ResultOfMethodCallIgnored
        new File(tmp, pack).renameTo(new File(dst, pack));
        if (title != null) {
            //noinspection ResultOfMethodCallIgnored
            new File(tmp, title).renameTo(new File(dst, title));
        }
        Backup.deleteTree(tmp);
        return c.getString(title != null ? R.string.arabic_both : R.string.arabic_text_only);
    }
}
