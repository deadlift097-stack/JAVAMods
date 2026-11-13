# Conversion report: BetterHitreg

* Source: `BetterHitreg-1_0_8-BETA_26_3.jar` (Fabric mod, id `betterhitreg`, version `1.0.8-BETA+26.3`)
* Output: java mod scaffold in `java-project/`
* Classes read: 52; mixins: 17; settings found: 53; assets copied: 1

## What this tool did

* Read the mod's metadata and its settings (from enum constants) and put them in the generated menu.
* Mapped the Java hooks it recognised to Mod Host events (below) and left an empty handler for each.
* Wrote readable bytecode of every hook to `bytecode_notes.txt`.

## What it did NOT do

The Java Edition logic is not translated. Bedrock has none of Java Minecraft's classes, so every
handler body is a TODO that has to be written again against the Mod Host API (`docs/`).

## Hooks (mixins)

| Mixin class | Targets | Methods | Mod Host |
|---|---|---|---|
| ArmorMixin | HumanoidArmorLayer | submit | unsupported: entities / players |
| AttackMixin | MultiPlayerGameMode | attack | attack |
| ChunkMixin | LevelRenderer | prepareChunkRenders, prepareChunkRendersIndirect | unsupported: rendering |
| DamageMixin | LivingEntity | handleDamageEvent | unsupported: entities / players |
| EntityMixin | Entity | turn | unsupported: entities / players |
| GLFWMixin | SDLEventHandler | handleMouseMotionEvent, pollEvents | unsupported: other |
| GlowMixin | Entity | getTeamColor, isCurrentlyGlowing | unsupported: entities / players |
| LocalPlayerAccessor | LocalPlayer | getWasSprinting | unsupported: entities / players |
| MouseMixin | MouseHandler | handleAccumulatedMovement, onMove | mouse |
| NetworkMixin | ClientPacketListener | handleDamageEvent | unsupported: network packets |
| ParticleMixin | ParticleEngine | makeParticle | unsupported: particles |
| PlayerMixin | LocalPlayer | crit, magicCrit | unsupported: entities / players |
| RenderMixin | EntityRenderer | shouldRender | unsupported: rendering |
| ServerMixin | ClientPacketListener | handleAnimate, handleDamageEvent, handleSoundEntityEvent, handleSoundEvent | unsupported: network packets |
| SoundMixin | SoundEngine | play | unsupported: sound |
| SourceMixin | Channel | play | unsupported: other |
| WorldMixin | LevelRenderer | - | unsupported: rendering |

## Settings

* `hitreg` (slider, default 0) from Setting.HITREG range guessed
* `total_fights` (slider, default 0) from Setting.TOTAL_FIGHTS range guessed
* `fight_playtime_(seconds)` (slider, default 0) from Setting.FIGHT_PLAYTIME_SECONDS range guessed
* `muffle_amount` (slider, default 0) from Setting.MUFFLE_AMOUNT range guessed
* `sharpen_amount` (slider, default 0) from Setting.SHARPEN_AMOUNT range guessed
* `metronome` (slider, default 0) from Setting.METRONOME range guessed
* `floor_grid_size` (slider, default 0) from Setting.GRID_SIZE range guessed
* `floor_grid_range` (slider, default 16) from Setting.GRID_RANGE range guessed
* `ground_height` (float, default 0.01) from Setting.GROUND_HEIGHT range guessed
* `ground_size` (slider, default 512) from Setting.GROUND_SIZE range guessed
* `score_x` (slider, default 1) from Setting.SCORE_X range guessed
* `score_y` (slider, default 1) from Setting.SCORE_Y range guessed
* `approach_hitbox_range` (slider, default 6) from Setting.APPROACH_HITBOX_RANGE range guessed
* `sound_hit_window` (slider, default 150) from Setting.SOUND_HIT_WINDOW range guessed
* `sound_recency_threshold` (slider, default 50) from Setting.SOUND_RECENCY_THRESHOLD range guessed
* `render` (slider, default 101010) from Style.GROUND range guessed
* `ui` (slider, default 0) from Style.BACKGROUND range guessed
* `toggle` (toggle, default True) from Toggle.TOGGLE
* `safeRegsOnly` (toggle, default True) from Toggle.SAFE_REGS_ONLY
* `ignoreShieldHolders` (toggle, default False) from Toggle.IGNORE_SHIELD_HOLDERS
* `debugInputs` (toggle, default False) from Toggle.DEBUG_INPUTS
* `debugSounds` (toggle, default False) from Toggle.DEBUG_SOUNDS
* `alertDelays` (toggle, default False) from Toggle.ALERT_DELAYS
* `alertGhosts` (toggle, default False) from Toggle.ALERT_GHOSTS
* `alertInconsistencies` (toggle, default False) from Toggle.ALERT_INCONSISTENCIES
* `alertFights` (toggle, default False) from Toggle.ALERT_FIGHTS
* `AlertJumpResets` (toggle, default False) from Toggle.ALERT_JUMP_RESETS
* `legacySounds` (toggle, default False) from Toggle.LEGACY_SOUNDS
* `hideAnimations` (toggle, default False) from Toggle.HIDE_ANIMATIONS
* `hideArmor` (toggle, default False) from Toggle.HIDE_ARMOR
* `hideAllParticles` (toggle, default False) from Toggle.HIDE_ALL_PARTICLES
* `hideOtherParticles` (toggle, default False) from Toggle.HIDE_OTHER_PARTICLES
* `particlesEveryHit` (toggle, default False) from Toggle.PARTICLES_EVERY_HIT
* `silenceOtherFights` (toggle, default False) from Toggle.SILENCE_OTHER_FIGHTS
* `silenceSelf` (toggle, default False) from Toggle.SILENCE_SELF
* `silenceThem` (toggle, default False) from Toggle.SILENCE_THEM
* `silenceNonHits` (toggle, default False) from Toggle.SILENCE_NON_HITS
* `hideOtherFights` (toggle, default False) from Toggle.HIDE_OTHER_FIGHTS
* `renderHitbox` (toggle, default False) from Toggle.RENDER_HITBOX
* `renderCross` (toggle, default False) from Toggle.RENDER_CROSS
* `RenderServerHitbox` (toggle, default False) from Toggle.RENDER_SERVER_HITBOX
* `RenderApproachHitbox` (toggle, default False) from Toggle.RENDER_APPROACH_HITBOX
* `RenderYourReach` (toggle, default False) from Toggle.RENDER_YOUR_REACH
* `RenderTheirReach` (toggle, default False) from Toggle.RENDER_THEIR_REACH
* `RenderYourJump` (toggle, default False) from Toggle.RENDER_YOUR_JUMP
* `RenderTheirJump` (toggle, default False) from Toggle.RENDER_THEIR_JUMP
* `PerfectHitColor` (toggle, default False) from Toggle.PERFECT_HIT_COLOR
* `JumpResetColor` (toggle, default False) from Toggle.JUMP_RESET_COLOR
* `VoidWorld` (toggle, default False) from Toggle.VOID_WORLD
* `CustomGround` (toggle, default False) from Toggle.CUSTOM_GROUND
* `FFB02E` (slider, default 0) from Palette.HALLOWEEN range guessed
* `D9A82E` (slider, default 280707) from Palette.CHINESE_NEW_YEAR range guessed
* `BFA7F2` (slider, default 151020) from Palette.EASTER range guessed

## Mod Host events available today

`key`, `touch`, `mouse` (input), `attack` (needs `attackEvent` in the host config), `tick` (host timer).
There is no HUD drawing, sound, world or entity access yet; hooks that need those stay unsupported.
