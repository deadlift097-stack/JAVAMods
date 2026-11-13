package example.hello;

import org.modhost.*;

/** A small Java mod for Mod Host. Build it with the workflow in docs/JAVA_MODS.md. */
public class HelloJavaMod implements Mod {
    private int hits;

    @Override
    public void onLoad(ModContext ctx) {
        ctx.registerMenu(new Menu("Hello Java")
                .description("Example Java mod: logs key presses and greets you.")
                .toggle("logKeys", "Log key presses", false)
                .slider("greetDelay", "Greeting delay (s)", 3, 1, 30));

        ctx.after(ctx.settings().getInt("greetDelay") * 1000L,
                () -> ctx.log("Hello from Java! This is " + ctx.name()));

        ctx.onKey((code, down, unicodeChar) -> {
            if (down && ctx.settings().getBoolean("logKeys")) {
                ctx.log("key " + code);
            }
            return false; // do not consume the key
        });

        ctx.onAttack(timeMs -> {
            hits++;
            ctx.storage().putLong("hits", hits);
        });
    }
}
