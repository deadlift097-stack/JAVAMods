# Java mods for Mod Host

Mod Host can load mods written in Java from `.jar` files. These are Java mods for **this
host's API** (`org.modhost`), running inside the launcher's Android process. They are not Java
Edition (Fabric/Forge) mods: Bedrock has none of Java Minecraft's classes, so those cannot run.
Use `tools/jar2mod.py` to turn a Java Edition mod into a starting point.

## Where jars go

    /storage/emulated/0/Android/media/<launcher package>/mod_host/java_mods/

(normally `.../Android/media/org.levimc.launcher/mod_host/java_mods/`). Restart the game after
adding or changing a jar. Messages and errors go to `mod_host/mod_host.log`.

## What a jar must contain

* `classes.dex`: your compiled code, converted with the Android `d8` tool. A plain jar of
  `.class` files is not enough on Android; the host says so in the log if the dex is missing.
* `modhost-mod.properties`:

      main=com.example.MyMod        (required: a public class implementing org.modhost.Mod)
      id=my_mod
      name=My Mod
      version=1.0
      author=you
      description=What it does
      api=1

Do not put the `org.modhost` classes in your jar; the host provides them.

## Requirements and safety

* Android 8.0 or newer.
* **Java mods are not sandboxed.** They run with everything the launcher app can do (files,
  network). Only install jars you trust. Lua mods are sandboxed; Java mods are not.
* Handlers run on the game's input thread: keep them short. A mod that throws 5 times is stopped.

## Writing a mod

    public class MyMod implements Mod {
        @Override public void onLoad(ModContext ctx) {
            ctx.registerMenu(new Menu("My Mod")
                .toggle("loud", "Loud mode", false)
                .slider("volume", "Volume", 7, 0, 10)
                .onChange((key, value) -> ctx.log(key + " = " + value)));
            ctx.onKey((code, down, ch) -> false);       // true consumes the key
            ctx.every(1000, () -> ctx.log("tick"));
        }
    }

`ModContext` offers: `log/warn/error`, `onKey`, `onTouch`, `onMouse`, `onAttack`, `onTick`,
`after`, `every`, `registerMenu`, `settings()`, `storage()`, `isEnabled()`, `dir()`, `dataDir()`.
The events and limits are the same as for Lua mods (see `LUA_API.md`): `onAttack` needs
`"attackEvent": true` in the host's config, `onTick` is a host timer (about 20 Hz) and not the
game tick, and there is no HUD, sound, world or entity access yet.

## Building a mod jar

**Easiest (works from a phone):** make an empty GitHub repository, create the file
`.github/workflows/build.yml` and paste in the contents of `build_mod.yml` that `jar2mod.py`
generated (or the one for the Hello example). It recreates the sources and builds
`<id>.jar` as a downloadable artifact in the Actions tab.

**On a computer** (JDK 8+ and Android build-tools):

    javac --release 8 -cp modhost-api.jar -d classes src/com/example/MyMod.java
    d8 --release --min-api 26 --lib $ANDROID_HOME/platforms/android-34/android.jar \
       --classpath modhost-api.jar --output dex $(find classes -name '*.class')
    cp modhost-mod.properties dex/
    (cd dex && jar --create --file ../my_mod.jar classes.dex modhost-mod.properties)

`modhost-api.jar` is built by the Mod Host workflow (artifact `modhost-api`).
