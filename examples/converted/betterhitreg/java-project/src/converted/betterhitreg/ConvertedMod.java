package converted.betterhitreg;

import org.modhost.*;

/**
 * Converted from BetterHitreg by jar2mod.py 0.1.
 *
 * The menu and event wiring below are generated. The behaviour of the original Java Edition
 * mod is NOT translated: Bedrock has none of Java Minecraft's classes. Port the logic by hand,
 * using report.md and bytecode_notes.txt as a guide.
 */
public class ConvertedMod implements Mod {
    @Override
    public void onLoad(ModContext ctx) throws Exception {
        Menu menu = new Menu("BetterHitreg")
                .description("Clientside mod that registers hits before the server to make hitreg feel faster (use /hitreg to configure)")
                .slider("hitreg", "Hitreg", 0, 0, 10)
                .slider("total_fights", "Total Fights", 0, 0, 10)
                .slider("fight_playtime__seconds_", "Fight Playtime Seconds", 0, 0, 10)
                .slider("muffle_amount", "Muffle Amount", 0, 0, 10)
                .slider("sharpen_amount", "Sharpen Amount", 0, 0, 10)
                .slider("metronome", "Metronome", 0, 0, 10)
                .slider("floor_grid_size", "Grid Size", 0, 0, 10)
                .slider("floor_grid_range", "Grid Range", 16, 0, 48)
                .floatSlider("ground_height", "Ground Height", 0.01, 0.0, 1.0)
                .slider("ground_size", "Ground Size", 512, 0, 1536)
                .slider("score_x", "Score X", 1, 0, 10)
                .slider("score_y", "Score Y", 1, 0, 10)
                .slider("approach_hitbox_range", "Approach Hitbox Range", 6, 0, 18)
                .slider("sound_hit_window", "Sound Hit Window", 150, 0, 450)
                .slider("sound_recency_threshold", "Sound Recency Threshold", 50, 0, 150)
                .slider("render", "Ground", 101010, 0, 303030)
                .slider("ui", "Background", 0, 0, 10)
                .toggle("toggle", "custom hitreg", true)
                .toggle("safeRegsOnly", "safe regs only", true)
                .toggle("ignoreShieldHolders", "ignore shield holders", false)
                .toggle("debugInputs", "debug inputs", false)
                .toggle("debugSounds", "debug sounds", false)
                .toggle("alertDelays", "alert delays", false)
                .toggle("alertGhosts", "alert ghosts", false)
                .toggle("alertInconsistencies", "alert inconsistencies", false)
                .toggle("alertFights", "alert fights", false)
                .toggle("AlertJumpResets", "alert jump resets", false)
                .toggle("legacySounds", "1.8 sounds", false)
                .toggle("hideAnimations", "hide animations", false)
                .toggle("hideArmor", "hide armor", false)
                .toggle("hideAllParticles", "hide crit particles", false)
                .toggle("hideOtherParticles", "only crit/sweep particles", false)
                .toggle("particlesEveryHit", "particles on every hit", false)
                .toggle("silenceOtherFights", "silence other fights", false)
                .toggle("silenceSelf", "silence your hits", false)
                .toggle("silenceThem", "silence their hits", false)
                .toggle("silenceNonHits", "silence non-hits", false)
                .toggle("hideOtherFights", "hide other fights", false)
                .toggle("renderHitbox", "render target hitbox", false)
                .toggle("renderCross", "render target cross", false)
                .toggle("RenderServerHitbox", "render server hitbox", false)
                .toggle("RenderApproachHitbox", "render approach hitbox", false)
                .toggle("RenderYourReach", "render your reach", false)
                .toggle("RenderTheirReach", "render their reach", false)
                .toggle("RenderYourJump", "render your jump range", false)
                .toggle("RenderTheirJump", "render their jump range", false)
                .toggle("PerfectHitColor", "color first tick hits", false)
                .toggle("JumpResetColor", "color jump resets", false)
                .toggle("VoidWorld", "void world", false)
                .toggle("CustomGround", "custom ground", false)
                .slider("FFB02E", "Halloween", 0, 0, 10)
                .slider("D9A82E", "Chinese New Year", 280707, 0, 842121)
                .slider("BFA7F2", "Easter", 151020, 0, 453060)
                .onChange((key, value) -> {
                    // TODO: react to a setting change (value is a Boolean, Integer or Double)
                    ctx.log("setting " + key + " = " + value);
                });
        ctx.registerMenu(menu);

        // Event 'attack' (from 1 Java hook(s))
        ctx.onAttack(timeMs -> {
            // Java: AttackMixin -> MultiPlayerGameMode.attack
            // TODO: port the logic here (see bytecode_notes.txt)
        });

        // Event 'mouse' (from 1 Java hook(s))
        ctx.onMouse((button, down) -> {
            // Java: MouseMixin -> MouseHandler.handleAccumulatedMovement/onMove
            // TODO: port the logic here (see bytecode_notes.txt)
            return false;
        });

    }
}
