# OpenKombat

OpenKombat is an Unreal Engine 5.8 rebuild of the fighting game *Mortal Kombat: Komplete Edition* (PC). It runs the
game's own scripts and reads its packages from your installed copy at run time. This repository holds only
OpenKombat's own code: **none of the game's files, assets, scripts or code are here, and none may be added.** You
need your own copy of the game.

It's a fan project, not affiliated with or endorsed by Warner Bros. Games or NetherRealm Studios.

## Long-term goals

MKKE comes first, but the aim is a single engine that runs the whole NetherRealm Studios series on PC from each
player's own copy:

* **Mortal Kombat (2011) / Komplete Edition**: the current target.
* **Injustice: Gods Among Us (Ultimate Edition)**, built on the same technology as MK 2011.
* **Mortal Kombat X / XL**.
* **Injustice 2**.
* **Mortal Kombat 11** and later games, as far as their formats allow.

That shapes how the code is written. Readers for packages, scripts, animation, sound and cinemas are kept apart from
game rules, so each game can add its own formats and natives. Where OpenKombat can do something more simply than
the original engine without changing how it plays, it does, so the same code can serve later games. Each game will
still need its own native-function table and per-game fixes. Contributions toward any game in the series are welcome.

## What works

Fights between any two of the roster on the game's stages, driven by the game's scripts. That covers walking,
jumping, normals, strings, specials and EX specials, throws, blocking with chip damage, armour, parries, combo
breakers, airborne reactions, the meter, X-rays (22 of the 34 characters), fatalities, rounds, the HUD, the fight
camera, the AI, sound, particles and cinemas.

## Setting up

1. Install Unreal Engine 5.8 and Visual Studio 2022 with the C++ game workload.
2. Install MKKE (Steam). OpenKombat looks for it at
   `C:/Program Files (x86)/Steam/steamapps/common/MortalKombat_KompleteEdition`. Set the `ok.GameDir` console
   variable if yours is somewhere else.
3. Put the game's extracted scripts (`.mko`) in `<Documents>/OpenKombatData/mko`, with `registry.json` and
   `native-names.tsv` one folder up, or set `ok.MkoDir`. These come from your own install; don't commit them.
4. `Content/` isn't in the repository: it's Epic's Third Person template, which the project uses for its default
   map and game mode (`Config/DefaultEngine.ini`). Make a Third Person (Blueprint) project with UE 5.8 and copy its
   `Content` folder in.
5. Right-click `OpenKombat.uproject`, choose *Generate Visual Studio project files*, and build `OpenKombatEditor`
   (Development, Win64).

## Running a fight

From the editor's console, or with `-ExecCmds=` on the command line:

```
ok.Fight CHAR_Scorpion CHAR_SubZero 150
```

`ok.AI.Side` picks which side the AI plays (`-1` for none), and `ok.Input.Hold <side> <mask> <frame> <frames>`
scripts inputs for tests.

## Contributing

* Keep game content out: no assets, extracted data, decompiled code or screenshots from the game. The ignore
  list blocks the usual file types; check `git status` before you commit anything.
* Match the code around you. Comments mark how a fact is known: `[obs]` observed in the game's code, `[inf]`
  inferred, `[unk]` unknown, `[design]` OpenKombat's own choice.
* Warnings C4458 and C4459 are errors in this module.

## License

OpenKombat's code is under the [MIT License](LICENSE), and so are contributions to it. The license covers only
OpenKombat's own code. The games, their files and their names belong to their owners.
