# jar2mod: Java Edition mod -> Mod Host mod scaffold

    python3 tools/jar2mod.py SomeMod.jar -o converted            # Java project (default)
    python3 tools/jar2mod.py SomeMod.jar -o converted --lang lua # Lua mod instead

Needs only Python 3.8+. Writes `converted/<modid>/` with

* `java-project/` (or `lua-mod/`): the scaffold. For Java it also contains `build_mod.yml`, a
  one-file GitHub workflow that builds the mod jar (paste it into an empty repo's
  `.github/workflows/build.yml` and run it).
* `report.md`: mod info, every mixin and whether it maps to a Mod Host event, every setting found.
* `bytecode_notes.txt`: readable bytecode of the mod's hooks, to help port the logic.
* `assets/`: language files and textures, for reference.

What it can do: read Fabric, Quilt, Forge and NeoForge metadata; find settings kept in enum
constants (name, label, default); recognise hooks into attacks, mouse, keyboard and ticks.

What it cannot do: translate the mod's logic. Bedrock has none of Java Minecraft's classes, so
every handler is a TODO to be written again against the Mod Host API. Hooks into rendering,
networking, sound, the world or entities have no Mod Host equivalent yet and are listed as
unsupported. Settings stored in other ways than enums are not detected yet; send me the jar
and I can add the pattern.
