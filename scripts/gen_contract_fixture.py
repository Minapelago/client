"""Writes tests/fixtures/contract.json from the Mina The Hollower apworld.

Usage, with Python 3.12 and the Archipelago requirements installed (pip install -r <world>/requirements.txt):
    python scripts/gen_contract_fixture.py ../world [tests/fixtures/contract.json]
"""

import contextlib
import io
import json
import random
import sys
from pathlib import Path

GAME = "Mina The Hollower"
SEED = 1

# The options that reach slot_data, each away from its default.
NON_DEFAULT_OPTIONS = {
    "goal": 0,
    "starting_weapon": 3,
    "ability_rando": [],
    "bone_up_cap": 1,
    "max_stat_level": 99,
    "kear_rando": 2,
    "astral_switches": 0,
    "death_link": 1,
}


def main() -> None:
    sys.dont_write_bytecode = True
    world_dir = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "tests/fixtures/contract.json"
    sys.path.insert(0, str(world_dir))

    # Importing worlds loads every bundled world, and the ones missing optional deps log tracebacks.
    with contextlib.redirect_stderr(io.StringIO()) as noise:
        from worlds.AutoWorld import AutoWorldRegister
        from worlds.mina_the_hollower import constants
        from worlds.mina_the_hollower.options import Goal
        from test.general import setup_multiworld
    world_type = AutoWorldRegister.world_types.get(GAME)
    if world_type is None:
        sys.exit(noise.getvalue() + f"\n{GAME} did not load from {world_dir}")

    def slot_data(options: dict) -> dict:
        random.seed(SEED)
        multiworld = setup_multiworld(world_type, seed=SEED, options=options)
        return multiworld.worlds[1].fill_slot_data()

    fixture = {
        "items": world_type.item_name_to_id,
        "locations": world_type.location_name_to_id,
        "generator_indexes": constants.repair_generator_indexes,
        "goal_options": Goal.options,
        "offsets": {k: v for k, v in vars(constants).items() if k.startswith(("ITEMS_OFFSET_", "LOCATIONS_OFFSET_"))},
        "non_default_options": NON_DEFAULT_OPTIONS,
        "slot_data": {"default": slot_data({}), "non_default": slot_data(NON_DEFAULT_OPTIONS)},
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(fixture, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
