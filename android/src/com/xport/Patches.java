package com.xport;

import android.content.Context;
import android.net.Uri;
import android.system.Os;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.MessageDigest;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.X509EncodedKeySpec;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// The launcher's Parches: fixes installed from a .zip without a new APK (tools/make-patch.py makes
// and signs them). A patch can carry
//   lib/libgame.so          the game's library, loaded instead of the APK's own (MainActivity
//                           .loadGameLibrary): only with the same Java side (ABI) and a build newer
//                           than the APK's; one at a time;
//   shaders/<HASH>.vs.spv   shaders by hash, read by the renderer instead of its cache's copy
//                           (XPORT_PATCH_SHADERS), for the library build named by for_build;
//   config/opciones.txt     renderer options ("cvar name=value" -> XPORT_PATCH_CVARS) and start
//                           options ("NAME=value").
// The manifest lists every file with its size and SHA-256 and is signed (ECDSA P-256) with a key
// whose public half is in KEYS: only a patch of renderer options alone may come unsigned. The
// APK's own library is never touched, so turning a patch off goes back to it.
//
// Files (getFilesDir()/patches, where code may be loaded from): <id>/ each patch as installed,
// estado.properties (on, confirmed, failed starts), pendiente.txt (the patches in use that have not
// yet run a game for 20 s: if the app dies before that, each counts a failed start, and two turn it
// off) and lista.txt, what the launcher shows (launcher_ui.cpp ScanPatches).
final class Patches {
    // The interface between this Java side and libgame.so: its native methods, the Java methods it
    // calls, the environment it gets. A patch's library loads only with the same number; change it
    // whenever that interface changes (tools/build-apk.sh copies it into assets/build.txt).
    static final int ABI = 1;

    // Public keys (X.509 SubjectPublicKeyInfo, base64) whose signatures make a patch official. The
    // private key is in %USERPROFILE%\.xport\parches on the PC that makes them, never in the project.
    private static final String[] KEYS = {
        "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAElxY3UK64G+yhEXAjb8KUyMD/VPypG8XZpNII1DblDyJxZ/4+u7uSu0TwUk/5R/0FDpnWA4QreiBQVrd+tYtdzQ==",
    };

    private static final int FAILED_STARTS = 2;
    private static final long MAX_BYTES = 512L << 20;
    private static final Pattern ID = Pattern.compile("[a-z0-9][a-z0-9._-]{0,63}");
    private static final Pattern SHADER = Pattern.compile("shaders/[0-9A-F]{16}\\.(vs|ps)(\\.late)?\\.spv");
    private static final Pattern CVAR = Pattern.compile("cvar ([A-Za-z0-9_]+)=([^\\s,]+)");
    private static final Pattern UNSIGNED_CVAR = Pattern.compile("cvar (xnative_[A-Za-z0-9_]+)=([A-Za-z0-9._-]{1,64})");
    private static final Pattern OPTION = Pattern.compile("([A-Z][A-Z0-9_]*)=(\\S+)");

    static final class Patch {
        String id, name, version, description, gpu, created;
        boolean signed, lib, shaders, config;
        int abi, minApi;
        long build, forBuild, bytes, libSize;
        File dir;
    }

    private static final List<Patch> patches = new ArrayList<>();
    private static final Properties state = new Properties();
    private static final List<String> notices = new ArrayList<>();
    private static File root;
    private static long apkBuild;
    private static String apkVersion = "";
    // The patch whose library this process runs, or null for the APK's own; the build it is.
    private static Patch loaded;
    private static long loadedBuild;
    private static boolean gameStarted;
    private static boolean decided;

    private Patches() {
    }

    // ---------- start ----------

    // MainActivity.onCreate, before the library loads: the patch library to load (its path), or
    // null for the APK's. A previous run that died with patches it had not yet confirmed counts a
    // failed start for each.
    static synchronized String startup(Context context, String version) {
        // The activity made again in the same process: its library is already loaded, and a second
        // one would not replace it. A change waits for the restart (the launcher says so).
        if (decided) {
            return loaded == null ? null : new File(loaded.dir, "lib/libgame.so").getAbsolutePath();
        }
        decided = true;
        root = new File(context.getFilesDir(), "patches");
        apkVersion = version == null ? "" : version;
        readApkBuild(context);
        load();
        File pending = new File(root, "pendiente.txt");
        if (pending.isFile()) {
            for (String id : readLines(pending)) {
                Patch patch = find(id);
                if (patch == null || flag(id, "confirmado")) {
                    continue;
                }
                int fails = number(id + ".fallos") + 1;
                state.setProperty(id + ".fallos", String.valueOf(fails));
                android.util.Log.w("xport", "patch " + id + ": the app closed before the game ran with it (" + fails + ")");
                if (fails >= FAILED_STARTS && flag(id, "activo")) {
                    state.setProperty(id + ".activo", "0");
                    state.setProperty(id + ".motivo", "fallo");
                    notices.add("fallo\t" + patch.name);
                }
            }
            pending.delete();
            save();
        }
        loadedBuild = apkBuild;
        Patch lib = wantedLibrary();
        if (lib == null) {
            return null;
        }
        File library = new File(lib.dir, "lib/libgame.so");
        if (library.length() != lib.libSize) {
            android.util.Log.w("xport", "patch " + lib.id + ": its library is damaged, the APK's runs");
            state.setProperty(lib.id + ".activo", "0");
            state.setProperty(lib.id + ".motivo", "carga");
            notices.add("carga\t" + lib.name);
            save();
            return null;
        }
        loaded = lib;
        loadedBuild = lib.build;
        if (!flag(lib.id, "confirmado")) {
            writeLines(pending, java.util.Collections.singletonList(lib.id));
        }
        android.util.Log.i("xport", "patch " + lib.id + " " + lib.version + ": its library replaces the APK's");
        return library.getAbsolutePath();
    }

    // The patch library could not be loaded: off, and the APK's loads instead.
    static synchronized void loadFailed(Throwable error) {
        if (loaded == null) {
            return;
        }
        android.util.Log.w("xport", "patch " + loaded.id + ": its library did not load: " + error);
        state.setProperty(loaded.id + ".activo", "0");
        state.setProperty(loaded.id + ".motivo", "carga");
        notices.add("carga\t" + loaded.name);
        loaded = null;
        loadedBuild = apkBuild;
        new File(root, "pendiente.txt").delete();
        save();
    }

    // The version the launcher shows: the patch's when its library runs.
    static synchronized String shownVersion() {
        return loaded != null ? loaded.version : apkVersion;
    }

    // The launcher closed normally (Salir or a restart): what it ran with did not crash it.
    static synchronized void cleanExit() {
        if (root != null) {
            new File(root, "pendiente.txt").delete();
        }
    }

    // The launcher has been open for a while: the patch library draws it (unless the game started,
    // whose pending list stays until confirm).
    static synchronized void launcherShown() {
        if (!gameStarted) {
            cleanExit();
        }
    }

    // ---------- the game ----------

    // MainActivity.startGame, before nativeStart: the patches' shaders and renderer options go in
    // the environment, their start options are returned (they come after the launcher's). The ones
    // not yet confirmed are written as pending.
    static synchronized String gameStarting() {
        if (root == null) {
            return "";
        }
        gameStarted = true;
        StringBuilder options = new StringBuilder();
        StringBuilder shaders = new StringBuilder();
        StringBuilder cvars = new StringBuilder();
        Set<String> pending = new LinkedHashSet<>();
        if (loaded != null && !flag(loaded.id, "confirmado")) {
            pending.add(loaded.id);
        }
        for (Patch patch : patches) {
            if (!"activo".equals(status(patch)) || (!patch.shaders && !patch.config)) {
                continue;
            }
            if (patch.shaders) {
                if (shaders.length() > 0) {
                    shaders.append(':');
                }
                shaders.append(new File(patch.dir, "shaders").getAbsolutePath());
            }
            if (patch.config) {
                for (String line : readLines(new File(patch.dir, "config/opciones.txt"))) {
                    java.util.regex.Matcher cvar = CVAR.matcher(line);
                    java.util.regex.Matcher option = OPTION.matcher(line);
                    if (cvar.matches()) {
                        if (cvars.length() > 0) {
                            cvars.append(',');
                        }
                        cvars.append(cvar.group(1)).append('=').append(cvar.group(2));
                    } else if (patch.signed && option.matches()) {
                        options.append(' ').append(line);
                    }
                }
            }
            if (!flag(patch.id, "confirmado")) {
                pending.add(patch.id);
            }
            android.util.Log.i("xport", "patch " + patch.id + " " + patch.version + " applied to the game");
        }
        setEnvironment("XPORT_PATCH_SHADERS", shaders.toString());
        setEnvironment("XPORT_PATCH_CVARS", cvars.toString());
        File file = new File(root, "pendiente.txt");
        if (pending.isEmpty()) {
            file.delete();
        } else {
            writeLines(file, new ArrayList<>(pending));
        }
        return options.toString().trim();
    }

    // A library change waits for the app to start again.
    static synchronized boolean restartNeeded() {
        return root != null && wantedLibrary() != loaded;
    }

    static synchronized boolean pending() {
        return root != null && new File(root, "pendiente.txt").isFile();
    }

    // The game has drawn for a while with the pending patches: they work, and are never turned off
    // by a later crash.
    static synchronized void confirm() {
        File file = new File(root, "pendiente.txt");
        for (String id : readLines(file)) {
            if (find(id) != null) {
                state.setProperty(id + ".confirmado", "1");
                state.setProperty(id + ".fallos", "0");
                android.util.Log.i("xport", "patch " + id + " confirmed: the game ran with it");
            }
        }
        file.delete();
        save();
        writeList();
    }

    // ---------- the launcher's actions (worker threads) ----------

    // Importar parche: the .zip from the file picker. Returns "import\t<name>" or "error\t<code>".
    static String importPatch(Context context, Uri uri) {
        File temp;
        synchronized (Patches.class) {
            temp = new File(root, ".import");
        }
        try {
            deleteTree(temp);
            if (!temp.mkdirs()) {
                throw new PatchException("carpeta");
            }
            Map<String, String> digests = new HashMap<>();
            Map<String, Long> sizes = new HashMap<>();
            String base = temp.getCanonicalPath() + File.separator;
            long total = 0;
            byte[] buffer = new byte[1 << 16];
            try (InputStream input = context.getContentResolver().openInputStream(uri);
                    ZipInputStream zip = new ZipInputStream(input)) {
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    String name = entry.getName();
                    if (entry.isDirectory()) {
                        continue;
                    }
                    if (!name.equals("manifest.json") && !name.equals("manifest.sig") && !name.equals("lib/libgame.so")
                            && !name.equals("config/opciones.txt") && !SHADER.matcher(name).matches()) {
                        throw new PatchException("integridad");
                    }
                    File out = new File(temp, name);
                    if (!out.getCanonicalPath().startsWith(base) || digests.containsKey(name)) {
                        throw new PatchException("integridad");
                    }
                    out.getParentFile().mkdirs();
                    MessageDigest digest = MessageDigest.getInstance("SHA-256");
                    long size = 0;
                    try (OutputStream output = new FileOutputStream(out)) {
                        int read;
                        while ((read = zip.read(buffer)) > 0) {
                            total += read;
                            size += read;
                            if (total > MAX_BYTES) {
                                throw new PatchException("tamano");
                            }
                            digest.update(buffer, 0, read);
                            output.write(buffer, 0, read);
                        }
                    }
                    digests.put(name, hex(digest.digest()));
                    sizes.put(name, size);
                }
            } catch (java.util.zip.ZipException e) {
                throw new PatchException("zip");
            }
            File manifestFile = new File(temp, "manifest.json");
            if (!manifestFile.isFile()) {
                throw new PatchException("manifest");
            }
            byte[] manifestBytes = java.nio.file.Files.readAllBytes(manifestFile.toPath());
            Patch patch;
            try {
                patch = parse(new org.json.JSONObject(new String(manifestBytes, StandardCharsets.UTF_8)), temp);
            } catch (org.json.JSONException e) {
                throw new PatchException("manifest");
            }
            File signatureFile = new File(temp, "manifest.sig");
            if (signatureFile.isFile()) {
                if (!verify(manifestBytes, java.nio.file.Files.readAllBytes(signatureFile.toPath()))) {
                    throw new PatchException("firma");
                }
                patch.signed = true;
            }
            // Every file the manifest lists, and nothing else, with its size and hash.
            org.json.JSONArray files = new org.json.JSONObject(new String(manifestBytes, StandardCharsets.UTF_8))
                    .getJSONArray("files");
            Set<String> listed = new LinkedHashSet<>();
            for (int i = 0; i < files.length(); ++i) {
                org.json.JSONObject file = files.getJSONObject(i);
                String path = file.getString("path");
                listed.add(path);
                if (!digests.containsKey(path) || sizes.get(path) != file.getLong("size")
                        || !digests.get(path).equalsIgnoreCase(file.getString("sha256"))) {
                    throw new PatchException("integridad");
                }
            }
            for (String name : digests.keySet()) {
                if (!name.startsWith("manifest.") && !listed.contains(name)) {
                    throw new PatchException("integridad");
                }
            }
            patch.lib = listed.contains("lib/libgame.so");
            patch.config = listed.contains("config/opciones.txt");
            for (String path : listed) {
                patch.shaders |= path.startsWith("shaders/");
            }
            if (!patch.lib && !patch.config && !patch.shaders) {
                throw new PatchException("vacio");
            }
            if (!patch.signed && (patch.lib || patch.shaders)) {
                throw new PatchException("sin_firma");
            }
            if (patch.config) {
                checkConfig(readLines(new File(temp, "config/opciones.txt")), patch.signed);
            }
            if (patch.shaders && patch.forBuild == 0) {
                throw new PatchException("manifest");
            }
            String refused;
            synchronized (Patches.class) {
                if (!context.getPackageName().equals(patchPackage(manifestBytes))) {
                    throw new PatchException("paquete");
                }
                refused = refusal(patch);
            }
            if (refused != null) {
                throw new PatchException(refused);
            }
            if (patch.lib) {
                new File(temp, "lib/libgame.so").setReadOnly();
            }
            synchronized (Patches.class) {
                File target = new File(root, patch.id);
                deleteTree(target);
                if (!temp.renameTo(target)) {
                    throw new PatchException("mover");
                }
                for (String key : new String[] {"confirmado", "fallos", "motivo"}) {
                    state.remove(patch.id + "." + key);
                }
                state.setProperty(patch.id + ".fecha", new java.text.SimpleDateFormat("dd/MM/yyyy HH:mm",
                        java.util.Locale.ROOT).format(new java.util.Date()));
                enable(patch.id, patch.lib);
                List<String> order = order();
                order.remove(patch.id);
                order.add(patch.id);
                state.setProperty("orden", String.join(",", order));
                save();
                load();
                writeList();
            }
            android.util.Log.i("xport", "patch imported: " + patch.id + " " + patch.version
                    + (patch.signed ? " (signed)" : " (unsigned)"));
            return "import\t" + patch.name;
        } catch (PatchException e) {
            deleteTree(temp);
            android.util.Log.w("xport", "patch import refused: " + e.getMessage());
            return "error\t" + e.getMessage();
        } catch (org.json.JSONException e) {
            deleteTree(temp);
            android.util.Log.w("xport", "patch import refused: manifest: " + e);
            return "error\tmanifest";
        } catch (Exception e) {
            deleteTree(temp);
            android.util.Log.w("xport", "patch import failed: " + e);
            return "error\tzip";
        }
    }

    // Activar / Desactivar / Borrar from the launcher ("on:<id>", "off:<id>", "del:<id>").
    static synchronized String action(String action) {
        String result = doAction(action);
        writeList();
        return result;
    }

    private static String doAction(String action) {
        int colon = action.indexOf(':');
        String verb = colon < 0 ? "" : action.substring(0, colon);
        Patch patch = colon < 0 ? null : find(action.substring(colon + 1));
        if (patch == null) {
            return "error\tfalta";
        }
        if (verb.equals("on")) {
            String refused = refusal(patch);
            if (refused != null) {
                return "error\t" + refused;
            }
            state.remove(patch.id + ".motivo");
            state.setProperty(patch.id + ".fallos", "0");
            enable(patch.id, patch.lib);
            save();
            return "on\t" + patch.name;
        }
        if (verb.equals("off")) {
            state.setProperty(patch.id + ".activo", "0");
            state.remove(patch.id + ".motivo");
            save();
            return "off\t" + patch.name;
        }
        if (verb.equals("del")) {
            // A library in use stays mapped until the app closes; its file can go now.
            deleteTree(patch.dir);
            for (String key : new String[] {"activo", "confirmado", "fallos", "motivo", "fecha"}) {
                state.remove(patch.id + "." + key);
            }
            List<String> order = order();
            order.remove(patch.id);
            state.setProperty("orden", String.join(",", order));
            save();
            load();
            return "del\t" + patch.name;
        }
        return "error\tfalta";
    }

    // The list for the launcher (launcher_ui.cpp ScanPatches), tab-separated:
    //   #  <APK version> <APK build> <running library: patch id or -> <restart needed 0/1>
    //   !  <fallo|carga> <patch name>                 (a patch turned off at this start)
    //   <id> <status> <signed 0/1> <kinds> <name> <version> <bytes> <date> <description>
    static synchronized void writeList() {
        if (root == null) {
            return;
        }
        StringBuilder text = new StringBuilder();
        Patch wanted = wantedLibrary();
        boolean restart = wanted != loaded;
        text.append("#\t").append(clean(apkVersion)).append('\t').append(apkBuild).append('\t')
                .append(loaded == null ? "-" : loaded.id).append('\t').append(restart ? 1 : 0).append('\n');
        for (String notice : notices) {
            text.append("!\t").append(notice.replace('\n', ' ')).append('\n');
        }
        for (int i = patches.size() - 1; i >= 0; --i) {
            Patch p = patches.get(i);
            List<String> kinds = new ArrayList<>();
            if (p.lib) {
                kinds.add("lib");
            }
            if (p.shaders) {
                kinds.add("shaders");
            }
            if (p.config) {
                kinds.add("config");
            }
            text.append(p.id).append('\t').append(status(p)).append('\t').append(p.signed ? 1 : 0).append('\t')
                    .append(String.join(",", kinds)).append('\t').append(clean(p.name)).append('\t')
                    .append(clean(p.version)).append('\t').append(p.bytes).append('\t')
                    .append(clean(state.getProperty(p.id + ".fecha", p.created))).append('\t')
                    .append(clean(p.description)).append('\n');
        }
        root.mkdirs();
        writeAtomically(new File(root, "lista.txt"), text.toString().getBytes(StandardCharsets.UTF_8));
    }

    // ---------- state ----------

    // activo   on and in use;
    // reiniciar-on / reiniciar-off  turned on (off) but its library is not (still is) the one
    //          running: the app must start again;
    // desactivado / fallo (turned off after failed starts) / carga (its library did not load);
    // obsoleto  the APK is newer than its library; abi (needs a newer APK); gpu, api (not this
    //          phone); base (shaders made for another library build).
    private static String status(Patch p) {
        if (p.gpu != null && !p.gpu.equals("all") && !p.gpu.equals(gpuFamily())) {
            return "gpu";
        }
        if (p.minApi > android.os.Build.VERSION.SDK_INT) {
            return "api";
        }
        if (p.lib && p.abi != ABI) {
            return "abi";
        }
        if (p.lib && p.build <= apkBuild) {
            return "obsoleto";
        }
        boolean on = flag(p.id, "activo");
        if (p.lib && (on != (loaded == p))) {
            return on ? "reiniciar-on" : "reiniciar-off";
        }
        if (!on) {
            String reason = state.getProperty(p.id + ".motivo", "");
            return reason.equals("fallo") || reason.equals("carga") ? reason : "desactivado";
        }
        if (p.shaders && p.forBuild != loadedBuild) {
            return "base";
        }
        return "activo";
    }

    // Why a patch cannot be turned on here, or null.
    private static String refusal(Patch p) {
        if (p.gpu != null && !p.gpu.equals("all") && !p.gpu.equals(gpuFamily())) {
            return "gpu";
        }
        if (p.minApi > android.os.Build.VERSION.SDK_INT) {
            return "api";
        }
        if (p.lib && p.abi != ABI) {
            return "abi";
        }
        if (p.lib && p.build <= apkBuild) {
            return "obsoleto";
        }
        return null;
    }

    // The patch library that should run: the newest one turned on that fits this APK.
    private static Patch wantedLibrary() {
        Patch best = null;
        for (Patch p : patches) {
            if (p.lib && flag(p.id, "activo") && refusal(p) == null && (best == null || p.build > best.build)) {
                best = p;
            }
        }
        return best;
    }

    // One library at a time: turning one on turns the others off.
    private static void enable(String id, boolean lib) {
        if (lib) {
            for (Patch other : patches) {
                if (other.lib && !other.id.equals(id)) {
                    state.setProperty(other.id + ".activo", "0");
                }
            }
        }
        state.setProperty(id + ".activo", "1");
    }

    private static void load() {
        patches.clear();
        state.clear();
        File file = new File(root, "estado.properties");
        if (file.isFile()) {
            try (InputStream input = new java.io.FileInputStream(file)) {
                state.load(input);
            } catch (IOException e) {
                android.util.Log.w("xport", "patches: state unreadable: " + e);
            }
        }
        File[] folders = root.listFiles();
        Map<String, Patch> found = new HashMap<>();
        if (folders != null) {
            for (File folder : folders) {
                if (!folder.isDirectory() || folder.getName().startsWith(".")) {
                    continue;
                }
                try {
                    byte[] bytes = java.nio.file.Files.readAllBytes(new File(folder, "manifest.json").toPath());
                    Patch patch = parse(new org.json.JSONObject(new String(bytes, StandardCharsets.UTF_8)), folder);
                    if (!patch.id.equals(folder.getName())) {
                        continue;
                    }
                    patch.signed = new File(folder, "manifest.sig").isFile();
                    patch.lib = new File(folder, "lib/libgame.so").isFile();
                    patch.shaders = new File(folder, "shaders").isDirectory();
                    patch.config = new File(folder, "config/opciones.txt").isFile();
                    found.put(patch.id, patch);
                } catch (Exception e) {
                    android.util.Log.w("xport", "patches: " + folder.getName() + " unreadable: " + e);
                }
            }
        }
        for (String id : order()) {
            Patch patch = found.remove(id);
            if (patch != null) {
                patches.add(patch);
            }
        }
        patches.addAll(found.values());
        // A deleted folder takes its state with it.
        List<String> ids = new ArrayList<>();
        for (Patch patch : patches) {
            ids.add(patch.id);
        }
        state.setProperty("orden", String.join(",", ids));
        // The running patch, unless it was deleted or replaced by another build of the same id (then
        // the one listed is not the one running, and a restart is needed).
        if (loaded != null) {
            Patch again = find(loaded.id);
            if (again != null && again.build == loaded.build && again.libSize == loaded.libSize) {
                loaded = again;
            }
        }
    }

    private static void save() {
        java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
        try {
            state.store(bytes, "Patches.java");
        } catch (IOException e) {
            return;
        }
        root.mkdirs();
        writeAtomically(new File(root, "estado.properties"), bytes.toByteArray());
    }

    private static Patch parse(org.json.JSONObject json, File dir) throws org.json.JSONException, PatchException {
        if (json.optInt("format", 0) != 1) {
            throw new PatchException("formato");
        }
        Patch patch = new Patch();
        patch.id = json.getString("id");
        if (!ID.matcher(patch.id).matches()) {
            throw new PatchException("manifest");
        }
        patch.name = json.optString("name", patch.id);
        patch.version = json.optString("version", "");
        patch.description = json.optString("description", "");
        patch.gpu = json.optString("gpu", "all");
        patch.created = json.optString("created", "");
        patch.abi = json.optInt("abi", 0);
        patch.minApi = json.optInt("min_api", 0);
        patch.build = json.optLong("build", 0);
        patch.forBuild = json.optLong("for_build", 0);
        patch.dir = dir;
        org.json.JSONArray files = json.optJSONArray("files");
        for (int i = 0; files != null && i < files.length(); ++i) {
            org.json.JSONObject file = files.getJSONObject(i);
            patch.bytes += file.optLong("size", 0);
            if (file.optString("path").equals("lib/libgame.so")) {
                patch.libSize = file.optLong("size", -1);
            }
        }
        return patch;
    }

    private static String patchPackage(byte[] manifest) throws org.json.JSONException {
        return new org.json.JSONObject(new String(manifest, StandardCharsets.UTF_8)).optString("package", "");
    }

    // Signed patches may also set start options; unsigned ones only renderer options of the
    // renderer (cvar xnative_*), plain values. Names the app itself sets are refused.
    private static void checkConfig(List<String> lines, boolean signed) throws PatchException {
        int count = 0;
        for (String raw : lines) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) {
                continue;
            }
            ++count;
            if (signed ? CVAR.matcher(line).matches() : UNSIGNED_CVAR.matcher(line).matches()) {
                continue;
            }
            java.util.regex.Matcher option = OPTION.matcher(line);
            if (!signed || !option.matches() || option.group(1).startsWith("XPORT_PATCH_")
                    || option.group(1).equals("XPORT_NATIVE_LIB_DIR")) {
                throw new PatchException("opciones");
            }
        }
        if (count == 0) {
            throw new PatchException("opciones");
        }
    }

    private static boolean verify(byte[] data, byte[] signature) {
        for (String key : KEYS) {
            try {
                PublicKey publicKey = KeyFactory.getInstance("EC").generatePublic(
                        new X509EncodedKeySpec(android.util.Base64.decode(key, android.util.Base64.DEFAULT)));
                Signature check = Signature.getInstance("SHA256withECDSA");
                check.initVerify(publicKey);
                check.update(data);
                if (check.verify(signature)) {
                    return true;
                }
            } catch (Exception e) {
                android.util.Log.w("xport", "patch signature check: " + e);
            }
        }
        return false;
    }

    // assets/build.txt (tools/build-apk.sh): build=<yyyymmddHHMMSS>, abi=, game=, version=.
    private static void readApkBuild(Context context) {
        apkBuild = 0;
        try (InputStream input = context.getAssets().open("build.txt")) {
            Properties build = new Properties();
            build.load(input);
            apkBuild = Long.parseLong(build.getProperty("build", "0").trim());
        } catch (IOException | NumberFormatException e) {
            android.util.Log.w("xport", "no assets/build.txt: " + e);
        }
    }

    private static String gpuFamily() {
        if (new File("/dev/kgsl-3d0").exists()) {
            return "adreno";
        }
        return new File("/dev/mali0").exists() ? "mali" : "other";
    }

    private static Patch find(String id) {
        for (Patch patch : patches) {
            if (patch.id.equals(id)) {
                return patch;
            }
        }
        return null;
    }

    private static boolean flag(String id, String key) {
        return "1".equals(state.getProperty(id + "." + key));
    }

    private static int number(String key) {
        try {
            return Integer.parseInt(state.getProperty(key, "0"));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static List<String> order() {
        List<String> ids = new ArrayList<>();
        for (String id : state.getProperty("orden", "").split(",")) {
            if (!id.isEmpty() && !ids.contains(id)) {
                ids.add(id);
            }
        }
        return ids;
    }

    private static void setEnvironment(String name, String value) {
        try {
            if (value.isEmpty()) {
                Os.unsetenv(name);
            } else {
                Os.setenv(name, value, true);
            }
        } catch (android.system.ErrnoException e) {
            android.util.Log.w("xport", "patches: " + name + " not set: " + e);
        }
    }

    private static String clean(String text) {
        return text == null ? "" : text.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder();
        for (byte b : bytes) {
            text.append(String.format(java.util.Locale.ROOT, "%02x", b & 0xFF));
        }
        return text.toString();
    }

    private static List<String> readLines(File file) {
        List<String> lines = new ArrayList<>();
        try {
            for (String line : new String(java.nio.file.Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).split("\n")) {
                if (!line.trim().isEmpty()) {
                    lines.add(line.trim());
                }
            }
        } catch (IOException e) {
            // none
        }
        return lines;
    }

    private static void writeLines(File file, List<String> lines) {
        writeAtomically(file, (String.join("\n", lines) + "\n").getBytes(StandardCharsets.UTF_8));
    }

    private static void writeAtomically(File file, byte[] bytes) {
        File temp = new File(file.getParentFile(), "." + file.getName() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temp)) {
            output.write(bytes);
            output.getFD().sync();
        } catch (IOException e) {
            android.util.Log.w("xport", "patches: " + file + " not written: " + e);
            return;
        }
        if (!temp.renameTo(file)) {
            android.util.Log.w("xport", "patches: " + file + " not replaced");
        }
    }

    static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }

    private static final class PatchException extends Exception {
        PatchException(String code) {
            super(code);
        }
    }
}
