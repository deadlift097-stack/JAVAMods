package org.modhost;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.reflect.Constructor;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.jar.JarFile;

/**
 * Loads Java mods from jar files and delivers events to them. The native side calls the
 * public static methods; mods only ever see {@link ModContext}.
 */
public final class JavaHost {
    public static final int API_VERSION = 1;
    static final String DESCRIPTOR = "modhost-mod.properties";
    private static final int MAX_ERRORS = 5;
    private static final int MAX_TIMERS = 200;
    private static final int MAX_SETTINGS = 64;

    private static final Object LOCK = new Object();
    private static final List<LoadedMod> MODS = new ArrayList<>();
    private static volatile boolean running;
    private static NativeApi nativeApi;
    private static ScheduledThreadPoolExecutor scheduler;
    private static long lastTickNanos;
    private static boolean tickEnabled;

    private JavaHost() {}

    public static void setNativeApi(NativeApi api) {
        nativeApi = api;
    }

    // ------------------------------------------------------------ lifecycle

    public static void start(String modsDir, String dataDir, String cacheDir, boolean tick) {
        synchronized (LOCK) {
            if (running) {
                return;
            }
            running = true;
            tickEnabled = tick;
            File mods = new File(modsDir);
            File data = new File(dataDir);
            File cache = new File(cacheDir);
            mods.mkdirs();
            data.mkdirs();
            cache.mkdirs();
            logLine("INFO", "java", "Java host starting. Mods folder: " + mods);

            scheduler = new ScheduledThreadPoolExecutor(1, r -> {
                Thread t = new Thread(r, "modhost-timer");
                t.setDaemon(true);
                return t;
            });
            scheduler.setRemoveOnCancelPolicy(true);

            File[] jars = mods.listFiles((dir, name) -> name.toLowerCase().endsWith(".jar"));
            if (jars != null) {
                Arrays.sort(jars);
                for (File jar : jars) {
                    loadJar(jar, data, cache);
                }
            }
            logLine("INFO", "java", MODS.size() + " Java mod(s) loaded");

            lastTickNanos = System.nanoTime();
            scheduler.scheduleWithFixedDelay(JavaHost::tick, 50, 50, TimeUnit.MILLISECONDS);
        }
    }

    public static void stop() {
        ScheduledThreadPoolExecutor toStop;
        synchronized (LOCK) {
            if (!running) {
                return;
            }
            running = false;
            for (LoadedMod m : MODS) {
                m.shutdown();
            }
            MODS.clear();
            toStop = scheduler;
            scheduler = null;
            logLine("INFO", "java", "Java host stopped");
        }
        if (toStop != null) {
            toStop.shutdownNow();
        }
    }

    // ------------------------------------------------------------ events from native

    public static boolean onKey(int keyCode, boolean down, int unicodeChar) {
        if (!running) {
            return false;
        }
        boolean consumed = false;
        synchronized (LOCK) {
            for (LoadedMod m : MODS) {
                if (!m.usable()) {
                    continue;
                }
                for (ModContext.KeyHandler h : m.keyHandlers) {
                    try {
                        if (h.onKey(keyCode, down, unicodeChar)) {
                            consumed = true;
                        }
                    } catch (Throwable t) {
                        m.fail("key handler", t);
                    }
                }
            }
        }
        return consumed;
    }

    public static boolean onTouch(int action, int pointerId, float x, float y) {
        if (!running) {
            return false;
        }
        boolean consumed = false;
        synchronized (LOCK) {
            for (LoadedMod m : MODS) {
                if (!m.usable()) {
                    continue;
                }
                for (ModContext.TouchHandler h : m.touchHandlers) {
                    try {
                        if (h.onTouch(action, pointerId, x, y)) {
                            consumed = true;
                        }
                    } catch (Throwable t) {
                        m.fail("touch handler", t);
                    }
                }
            }
        }
        return consumed;
    }

    public static boolean onMouse(int button, boolean down) {
        if (!running) {
            return false;
        }
        boolean consumed = false;
        synchronized (LOCK) {
            for (LoadedMod m : MODS) {
                if (!m.usable()) {
                    continue;
                }
                for (ModContext.MouseHandler h : m.mouseHandlers) {
                    try {
                        if (h.onMouse(button, down)) {
                            consumed = true;
                        }
                    } catch (Throwable t) {
                        m.fail("mouse handler", t);
                    }
                }
            }
        }
        return consumed;
    }

    public static void onAttack() {
        if (!running) {
            return;
        }
        long now = System.currentTimeMillis();
        synchronized (LOCK) {
            for (LoadedMod m : MODS) {
                if (!m.usable()) {
                    continue;
                }
                for (ModContext.AttackHandler h : m.attackHandlers) {
                    try {
                        h.onAttack(now);
                    } catch (Throwable t) {
                        m.fail("attack handler", t);
                    }
                }
            }
        }
    }

    public static void onToggle(String moduleId, boolean enabled) {
        synchronized (LOCK) {
            LoadedMod m = byModuleId(moduleId);
            if (m == null || m.failed) {
                return;
            }
            m.enabled = enabled;
            Menu.ToggleListener l = m.menu == null ? null : m.menu.toggleListener();
            if (l != null) {
                try {
                    l.onToggle(enabled);
                } catch (Throwable t) {
                    m.fail("toggle listener", t);
                }
            }
        }
    }

    public static void onConfigChanged(String moduleId, String key, String value) {
        synchronized (LOCK) {
            LoadedMod m = byModuleId(moduleId);
            if (m == null || m.failed || !m.values.containsKey(key)) {
                return;
            }
            m.values.put(key, value);
            m.store.setProperty("setting." + key, value);
            m.saveStore();
            Menu.ChangeListener l = m.menu == null ? null : m.menu.changeListener();
            if (l != null) {
                try {
                    l.onChange(key, m.typedValue(key));
                } catch (Throwable t) {
                    m.fail("change listener", t);
                }
            }
        }
    }

    // ------------------------------------------------------------ internals

    private static LoadedMod byModuleId(String moduleId) {
        for (LoadedMod m : MODS) {
            if (moduleId != null && moduleId.equals(m.moduleId)) {
                return m;
            }
        }
        return null;
    }

    private static void tick() {
        long now = System.nanoTime();
        long dtMs = (now - lastTickNanos) / 1_000_000L;
        lastTickNanos = now;
        if (!running || !tickEnabled) {
            return;
        }
        synchronized (LOCK) {
            for (LoadedMod m : MODS) {
                if (!m.usable()) {
                    continue;
                }
                for (ModContext.TickHandler h : m.tickHandlers) {
                    try {
                        h.onTick(dtMs);
                    } catch (Throwable t) {
                        m.fail("tick handler", t);
                    }
                }
            }
        }
    }

    static void logLine(String level, String modId, String message) {
        NativeApi api = nativeApi;
        if (api != null) {
            try {
                api.log(level, modId, message);
                return;
            } catch (Throwable ignored) {
                // fall through to stdout
            }
        }
        System.out.println("[" + level + "][" + modId + "] " + message);
    }

    private static boolean isAndroid() {
        try {
            Class.forName("dalvik.system.DexClassLoader");
            return true;
        } catch (ClassNotFoundException e) {
            return false;
        }
    }

    private static ClassLoader newClassLoader(File jar, File cacheDir) throws Exception {
        ClassLoader parent = JavaHost.class.getClassLoader();
        if (!isAndroid()) {
            return new URLClassLoader(new URL[] {jar.toURI().toURL()}, parent);
        }
        // Newer Android versions refuse dex files that are writable, so load a read-only copy.
        File copy = new File(cacheDir, jar.getName() + "-" + jar.length() + "-" + jar.lastModified() + ".jar");
        if (!copy.exists()) {
            try (InputStream in = new FileInputStream(jar); OutputStream out = new FileOutputStream(copy)) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
            copy.setReadOnly();
        }
        Class<?> dcl = Class.forName("dalvik.system.DexClassLoader");
        Constructor<?> ctor = dcl.getConstructor(String.class, String.class, String.class, ClassLoader.class);
        return (ClassLoader) ctor.newInstance(copy.getAbsolutePath(), cacheDir.getAbsolutePath(), null, parent);
    }

    private static void loadJar(File jar, File dataRoot, File cacheDir) {
        String jarName = jar.getName();
        try {
            Properties props = new Properties();
            boolean hasDex;
            try (JarFile jf = new JarFile(jar)) {
                java.util.zip.ZipEntry desc = jf.getEntry(DESCRIPTOR);
                if (desc == null) {
                    logLine("ERROR", jarName, "skipped: no " + DESCRIPTOR + " inside the jar");
                    return;
                }
                try (InputStream in = jf.getInputStream(desc)) {
                    props.load(in);
                }
                hasDex = jf.getEntry("classes.dex") != null;
            }
            String mainClass = props.getProperty("main", "").trim();
            if (mainClass.isEmpty()) {
                logLine("ERROR", jarName, "skipped: " + DESCRIPTOR + " has no 'main' class");
                return;
            }
            if (isAndroid() && !hasDex) {
                logLine("ERROR", jarName,
                        "skipped: the jar has no classes.dex. Run d8 on it first (see docs/JAVA_MODS.md)");
                return;
            }
            String stem = jarName.substring(0, jarName.length() - 4);
            String id = sanitizeId(props.getProperty("id", stem));
            for (LoadedMod other : MODS) {
                if (other.id.equals(id)) {
                    logLine("WARN", id, "skipped: another Java mod already uses this id");
                    return;
                }
            }
            int api = parseIntOr(props.getProperty("api", "1"), 1);
            if (api > API_VERSION) {
                logLine("ERROR", id, "skipped: needs host API " + api + " but this host has " + API_VERSION);
                return;
            }

            ClassLoader loader = newClassLoader(jar, cacheDir);
            Object instance = loader.loadClass(mainClass).getDeclaredConstructor().newInstance();
            if (!(instance instanceof Mod)) {
                logLine("ERROR", id, "skipped: " + mainClass + " does not implement org.modhost.Mod");
                return;
            }
            File modData = new File(dataRoot, id);
            modData.mkdirs();
            LoadedMod m = new LoadedMod(id, props.getProperty("name", id), props.getProperty("version", "0.0.0"),
                    props.getProperty("author", ""), props.getProperty("description", ""), jar.getParentFile(),
                    modData, (Mod) instance);
            MODS.add(m);
            try {
                m.mod.onLoad(m);
                logLine("INFO", id, "loaded " + m.name + " v" + m.version);
            } catch (Throwable t) {
                m.fail("onLoad", t);
                m.shutdown();
                MODS.remove(m);
            }
        } catch (Throwable t) {
            logLine("ERROR", jarName, "could not load: " + describe(t));
        }
    }

    static String sanitizeId(String s) {
        StringBuilder b = new StringBuilder();
        for (char c : s.toCharArray()) {
            if (Character.isLetterOrDigit(c) && c < 128) {
                b.append(Character.toLowerCase(c));
            } else if (c == '_' || c == '-' || c == '.') {
                b.append(c);
            } else {
                b.append('_');
            }
        }
        return b.length() == 0 ? "mod" : b.toString();
    }

    static int parseIntOr(String s, int def) {
        try {
            return Integer.parseInt(s.trim());
        } catch (Exception e) {
            return def;
        }
    }

    static String describe(Throwable t) {
        StringWriter sw = new StringWriter();
        t.printStackTrace(new PrintWriter(sw));
        String[] lines = sw.toString().split("\n");
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < Math.min(lines.length, 8); i++) {
            b.append(lines[i].trim()).append(i + 1 < Math.min(lines.length, 8) ? " | " : "");
        }
        return b.toString();
    }

    // ------------------------------------------------------------ one loaded mod

    private static final class LoadedMod implements ModContext {
        final String id;
        final String name;
        final String version;
        final String author;
        final String description;
        final File dir;
        final File dataDir;
        final Mod mod;

        final List<ModContext.KeyHandler> keyHandlers = new CopyOnWriteArrayList<>();
        final List<ModContext.TouchHandler> touchHandlers = new CopyOnWriteArrayList<>();
        final List<ModContext.MouseHandler> mouseHandlers = new CopyOnWriteArrayList<>();
        final List<ModContext.AttackHandler> attackHandlers = new CopyOnWriteArrayList<>();
        final List<ModContext.TickHandler> tickHandlers = new CopyOnWriteArrayList<>();
        final List<ScheduledFuture<?>> timers = new CopyOnWriteArrayList<>();

        final Properties store = new Properties();
        final File storeFile;
        final Map<String, String> values = new HashMap<>();
        final Map<String, Setting> defs = new HashMap<>();
        Menu menu;
        String moduleId;
        volatile boolean enabled = true;
        int errors;
        volatile boolean failed;

        LoadedMod(String id, String name, String version, String author, String description, File dir,
                File dataDir, Mod mod) {
            this.id = id;
            this.name = name;
            this.version = version;
            this.author = author;
            this.description = description;
            this.dir = dir;
            this.dataDir = dataDir;
            this.mod = mod;
            this.storeFile = new File(dataDir, "storage.properties");
            if (storeFile.exists()) {
                try (InputStream in = new FileInputStream(storeFile)) {
                    store.load(in);
                } catch (IOException ignored) {
                    // start empty
                }
            }
        }

        boolean usable() {
            return !failed && enabled;
        }

        void fail(String where, Throwable t) {
            logLine("ERROR", id, where + " failed: " + describe(t));
            if (++errors >= MAX_ERRORS && !failed) {
                failed = true;
                logLine("ERROR", id, "stopped after too many errors");
            }
        }

        void shutdown() {
            try {
                mod.onUnload();
            } catch (Throwable t) {
                logLine("ERROR", id, "onUnload failed: " + describe(t));
            }
            for (ScheduledFuture<?> f : timers) {
                f.cancel(false);
            }
            timers.clear();
            NativeApi api = nativeApi;
            if (menu != null && api != null && moduleId != null) {
                try {
                    api.unregisterMenu(moduleId);
                } catch (Throwable ignored) {
                    // nothing to do
                }
            }
        }

        void saveStore() {
            try (OutputStream out = new FileOutputStream(storeFile)) {
                store.store(out, "Java mod storage");
            } catch (IOException ignored) {
                // keep running; the value stays in memory
            }
        }

        Object typedValue(String key) {
            Setting s = defs.get(key);
            String v = values.get(key);
            if (s == null || v == null) {
                return null;
            }
            switch (s.type) {
                case Setting.TOGGLE:
                    return Boolean.valueOf("true".equals(v));
                case Setting.SLIDER:
                    return Integer.valueOf((int) parseDoubleOr(v, 0));
                default:
                    return Double.valueOf(parseDoubleOr(v, 0));
            }
        }

        // ---- ModContext

        @Override
        public String id() {
            return id;
        }

        @Override
        public String name() {
            return name;
        }

        @Override
        public String version() {
            return version;
        }

        @Override
        public String author() {
            return author;
        }

        @Override
        public String description() {
            return description;
        }

        @Override
        public File dir() {
            return dir;
        }

        @Override
        public File dataDir() {
            return dataDir;
        }

        @Override
        public void log(String message) {
            logLine("INFO", id, message);
        }

        @Override
        public void warn(String message) {
            logLine("WARN", id, message);
        }

        @Override
        public void error(String message) {
            logLine("ERROR", id, message);
        }

        @Override
        public void onKey(ModContext.KeyHandler handler) {
            keyHandlers.add(handler);
        }

        @Override
        public void onTouch(ModContext.TouchHandler handler) {
            touchHandlers.add(handler);
        }

        @Override
        public void onMouse(ModContext.MouseHandler handler) {
            mouseHandlers.add(handler);
        }

        @Override
        public void onAttack(ModContext.AttackHandler handler) {
            attackHandlers.add(handler);
        }

        @Override
        public void onTick(ModContext.TickHandler handler) {
            tickHandlers.add(handler);
        }

        private Runnable guarded(Runnable task) {
            return () -> {
                synchronized (LOCK) {
                    if (!running || !usable()) {
                        return;
                    }
                    try {
                        task.run();
                    } catch (Throwable t) {
                        fail("timer", t);
                    }
                }
            };
        }

        private TimerHandle track(ScheduledFuture<?> f) {
            timers.add(f);
            return () -> {
                f.cancel(false);
                timers.remove(f);
            };
        }

        @Override
        public TimerHandle after(long delayMs, Runnable task) {
            ScheduledThreadPoolExecutor s = scheduler;
            if (s == null || timers.size() >= MAX_TIMERS) {
                throw new IllegalStateException("cannot schedule more timers");
            }
            return track(s.schedule(guarded(task), Math.max(10, delayMs), TimeUnit.MILLISECONDS));
        }

        @Override
        public TimerHandle every(long periodMs, Runnable task) {
            ScheduledThreadPoolExecutor s = scheduler;
            if (s == null || timers.size() >= MAX_TIMERS) {
                throw new IllegalStateException("cannot schedule more timers");
            }
            long p = Math.max(10, periodMs);
            return track(s.scheduleWithFixedDelay(guarded(task), p, p, TimeUnit.MILLISECONDS));
        }

        @Override
        public void registerMenu(Menu m) {
            if (menu != null) {
                throw new IllegalStateException("registerMenu can only be called once");
            }
            if (m.settings().size() > MAX_SETTINGS) {
                throw new IllegalArgumentException("at most " + MAX_SETTINGS + " settings");
            }
            List<String> encoded = new ArrayList<>();
            for (Setting s : m.settings()) {
                if (!s.key.matches("[A-Za-z0-9_]{1,64}")) {
                    throw new IllegalArgumentException("invalid setting key '" + s.key + "'");
                }
                defs.put(s.key, s);
                String saved = store.getProperty("setting." + s.key);
                String value = saved != null ? saved : s.defaultValue;
                values.put(s.key, value);
                encoded.add(s.key + "\u001f" + s.label + "\u001f" + s.type + "\u001f" + value + "\u001f" + s.min
                        + "\u001f" + s.max);
            }
            menu = m;
            enabled = m.enabled();
            moduleId = id + ".menu";
            NativeApi api = nativeApi;
            boolean ok = api != null
                    && api.registerMenu(moduleId, m.name(), m.description(), m.enabled(),
                            encoded.toArray(new String[0]));
            if (!ok) {
                logLine("WARN", id, "the in-game menu could not be registered");
            }
        }

        @Override
        public Settings settings() {
            return new Settings() {
                @Override
                public boolean getBoolean(String key) {
                    return "true".equals(values.get(key));
                }

                @Override
                public int getInt(String key) {
                    return (int) parseDoubleOr(values.get(key), 0);
                }

                @Override
                public double getDouble(String key) {
                    return parseDoubleOr(values.get(key), 0);
                }

                @Override
                public String getString(String key) {
                    return values.get(key);
                }
            };
        }

        @Override
        public Storage storage() {
            return new Storage() {
                @Override
                public String getString(String key, String defaultValue) {
                    return store.getProperty("u." + key, defaultValue);
                }

                @Override
                public long getLong(String key, long defaultValue) {
                    String v = store.getProperty("u." + key);
                    try {
                        return v == null ? defaultValue : Long.parseLong(v);
                    } catch (NumberFormatException e) {
                        return defaultValue;
                    }
                }

                @Override
                public boolean getBoolean(String key, boolean defaultValue) {
                    String v = store.getProperty("u." + key);
                    return v == null ? defaultValue : "true".equals(v);
                }

                @Override
                public void putString(String key, String value) {
                    store.setProperty("u." + key, value);
                    saveStore();
                }

                @Override
                public void putLong(String key, long value) {
                    putString(key, Long.toString(value));
                }

                @Override
                public void putBoolean(String key, boolean value) {
                    putString(key, Boolean.toString(value));
                }

                @Override
                public void remove(String key) {
                    store.remove("u." + key);
                    saveStore();
                }
            };
        }

        @Override
        public boolean isEnabled() {
            return enabled;
        }
    }

    static double parseDoubleOr(String s, double def) {
        try {
            return s == null ? def : Double.parseDouble(s.trim());
        } catch (NumberFormatException e) {
            return def;
        }
    }
}
