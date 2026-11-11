"""Generate the reader's supplemental field offsets from the supplied CppSDK."""

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SDK = ROOT / 'ROB RUN_20261108_193627_[unknowncheats.me]_' / 'ROB RUN_20261108_193627' / 'CppSDK'
DEFAULT_HEADER = ROOT / '2_src_port' / 'robrun_sdk_offsets.h'
DEFAULT_REPORT = ROOT / 'offsets' / 'robrun_sdk_offsets.json'
SDK_FILES = ('Models.hpp', 'Battle.hpp', 'Game.hpp', 'Global.hpp')

# macro, assembly, fully qualified managed class, field (before backing-field mangling).
# Generic payload and native Unity layouts are deliberately excluded: the CppSDK
# does not establish those layouts.
FIELD_SPEC = [
    ('RR_OFF_WEM_CONTAINERS', 'Models', 'Models.Models.WorldEntities.WorldEntitiesModel', '_containers'),
    ('RR_OFF_WEM_DROPPED_LOOT', 'Models', 'Models.Models.WorldEntities.WorldEntitiesModel', '_loots'),
    ('RR_OFF_WEM_VEHICLE_LOOT', 'Models', 'Models.Models.WorldEntities.WorldEntitiesModel', '_vehicleLoots'),
    ('RR_OFF_WEM_DOORS', 'Models', 'Models.Models.WorldEntities.WorldEntitiesModel', '_doors'),
    ('RR_OFF_WEM_CARS', 'Models', 'Models.Models.WorldEntities.WorldEntitiesModel', '_cars'),
    ('RR_OFF_WEM_HELICOPTERS', 'Models', 'Models.Models.WorldEntities.WorldEntitiesModel', '_helicopters'),
    ('RR_OFF_WEM_HIDE_OBJECTS', 'Models', 'Models.Models.WorldEntities.WorldEntitiesModel', '_hideObjects'),
    ('RR_OFF_CONTAINER_POS', 'Models', 'Models.Models.Container.ContainerModel', 'PositionModel'),
    ('RR_OFF_DOOR_STATE', 'Models', 'Models.Models.Door.DoorModel', 'State'),
    ('RR_OFF_DOOR_POS', 'Models', 'Models.Models.Door.DoorModel', 'Position'),
    ('RR_OFF_DOOR_STATE_DATA', 'Models', 'Models.Models.DoorState.DoorStateModel', '_state'),
    ('RR_OFF_DROPPED_LOOT_POS', 'Models', 'Models.Models.LootModel.LootModel', 'InteractPositionModel'),
    ('RR_OFF_VEHICLE_LOOT_POS', 'Models', 'Models.Models.VehicleLootModel.VehicleLootModel', 'InteractPositionModel'),
    ('RR_OFF_POS_X', 'Models', 'Models.Models.PositionModel.PositionModel', 'X'),
    ('RR_OFF_POS_Y', 'Models', 'Models.Models.PositionModel.PositionModel', 'Y'),
    ('RR_OFF_POS_Z', 'Models', 'Models.Models.PositionModel.PositionModel', 'Z'),
    ('RR_OFF_INTERACT_POS_X', 'Models', 'Models.Models.InteractPosition.InteractPositionModel', 'X'),
    ('RR_OFF_INTERACT_POS_Y', 'Models', 'Models.Models.InteractPosition.InteractPositionModel', 'Y'),
    ('RR_OFF_INTERACT_POS_Z', 'Models', 'Models.Models.InteractPosition.InteractPositionModel', 'Z'),
    ('RR_OFF_LOOK_MODEL', 'Models', 'Models.Models.BattleCharacter.BattleCharacterModel', 'LookDirection'),
    ('RR_OFF_CHAR_SKELETON', 'Models', 'Models.Models.BattleCharacter.BattleCharacterModel', 'SkeletonModel'),
    ('RR_OFF_CHAR_INVEH', 'Models', 'Models.Models.BattleCharacter.BattleCharacterModel', 'InVehicleModel'),
    ('RR_OFF_CHAR_STATE', 'Models', 'Models.Models.BattleCharacter.BattleCharacterModel', 'StateModel'),
    ('RR_OFF_COMMON_ITEM_IN_HANDS', 'Models', 'Models.Data.BattleCharacterCommonData', 'ItemInHands'),
    ('RR_OFF_INVEH_ID', 'Models', 'Models.Models.InVehicle.InVehicleModel', '_entityId'),
    ('RR_OFF_INVEH_TYPE', 'Models', 'Models.Models.InVehicle.InVehicleModel', '_vehicleType'),
    ('RR_OFF_STATE_HIDDEN', 'Models', 'Models.Models.BattleCharacterStateModel.BattleCharacterStateModel', '_characterHiddenStateModel'),
    ('RR_OFF_HIDDEN_ID_DATA', 'Models', 'Models.Models.CharacterHideObjectState.CharacterHideObjectStateModel', '_hideObjectEntityIdData'),
    ('RR_OFF_HIDE_DESC', 'Models', 'Models.Models.HideObject.HideObjectModel', 'Description'),
    ('RR_OFF_HIDE_DESC_ID', 'Models', 'Models.References.HideObject.LocationHideObjectDescription', 'EntityId'),
    ('RR_OFF_HIDE_DESC_POS', 'Models', 'Models.References.HideObject.LocationHideObjectDescription', 'Position'),
    ('RR_OFF_HIDE_DESC_TYPE', 'Models', 'Models.References.HideObject.LocationHideObjectDescription', 'HideObjectDescriptionFunc'),
    ('RR_OFF_HIDE_OCCUPANT', 'Models', 'Models.Models.HideObject.HideObjectModel', '_entityId'),
    ('RR_OFF_HIDE_DESC_ROT', 'Models', 'Models.References.HideObject.LocationHideObjectDescription', 'Orientation'),
    ('RR_OFF_HIDE_DESC_INTERACT', 'Models', 'Models.References.HideObject.LocationHideObjectDescription', 'InteractPoint'),
    ('RR_OFF_BATTLE_VOICE_ROOM', 'Battle', 'Game.Battle.BattleModel', 'VoiceChatBattleRoomModel'),
    ('RR_OFF_BATTLE_PLAYER_SLOTS', 'Battle', 'Game.Battle.BattleModel', 'BattleCharacterSlotsModel'),
    ('RR_OFF_VOICE_ROOM_PARTICIPANTS', 'Global', 'Game.Global.VoiceChat.VoiceChatRoomModel', 'ParticipantsModel'),
    ('RR_OFF_VOICE_STRING_MAP', 'Global', 'Game.Global.VoiceChat.VoiceChatParticipantsModel', '_participantStringIdToModels'),
    ('RR_OFF_VOICE_SLOT', 'Battle', 'Game.Battle.VoiceChat.VoiceChatBattleParticipantModel', 'SlotDescription'),
    ('RR_OFF_VOICE_HIDE_ID', 'Battle', 'Game.Battle.VoiceChat.VoiceChatBattleParticipantModel', 'HideObjectEntityId'),
    ('RR_OFF_VOICE_USER_ID', 'Global', 'Game.Global.VoiceChat.VoiceChatRoomParticipantModel', 'ProductUserIdString'),
    ('RR_OFF_VOICE_SPEAKING', 'Global', 'Game.Global.VoiceChat.VoiceChatRoomParticipantModel', 'IsSpeaking'),
    ('RR_OFF_SLOTS_DICTIONARY', 'Models', 'Models.Models.BattleCharacterSlots.BattleCharacterSlotsModel', '_slots'),
    ('RR_OFF_SLOT_DATA', 'Models', 'Models.Models.BattleCharacterSlots.BattleCharacterSlotModel', '_data_1'),
    ('RR_OFF_SLOT_DESC', 'Models', 'Models.Models.BattleCharacterSlots.BattleCharacterSlotModel', 'Description'),
    ('RR_OFF_SLOT_ENTITY_ID', 'Models', 'Models.Data.BattleCharacterSlotData', 'EntityId'),
    ('RR_OFF_SLOT_NICKNAME', 'Models', 'Models.Data.BattleCharacterSlotData', 'Nickname'),
    ('RR_OFF_SLOT_USER_ID', 'Models', 'Models.Data.BattleCharacterSlotData', 'EOSProductUserId'),
    ('RR_OFF_SEATS_DATA', 'Models', 'Models.Models.VehicleSeats.VehicleSeatsModel', '_data_1'),
    ('RR_OFF_SEAT_OCCUPIED', 'Models', 'Models.Data.Vehicle.VehicleSeatData', 'Occupied'),
    ('RR_OFF_SEAT_ENTITY_ID', 'Models', 'Models.Data.Vehicle.VehicleSeatData', 'EntityId'),
    ('RR_OFF_CAR_ID', 'Models', 'Models.Models.Car.CarModel', 'Id'),
    ('RR_OFF_CAR_OWNER_ROLE', 'Models', 'Models.Models.Car.CarModel', 'OwnerRole'),
    ('RR_OFF_CAR_DESC', 'Models', 'Models.Models.Car.CarModel', 'Description'),
    ('RR_OFF_CAR_TRANSFORM', 'Models', 'Models.Models.Car.CarModel', 'Transform'),
    ('RR_OFF_CAR_SEATS', 'Models', 'Models.Models.Car.CarModel', 'Seats'),
    ('RR_OFF_HELI_ID', 'Models', 'Models.Models.Helicopter.HelicopterModel', 'Id'),
    ('RR_OFF_HELI_OWNER_ROLE', 'Models', 'Models.Models.Helicopter.HelicopterModel', 'OwnerRole'),
    ('RR_OFF_HELI_DESC', 'Models', 'Models.Models.Helicopter.HelicopterModel', 'Description'),
    ('RR_OFF_HELI_TRANSFORM', 'Models', 'Models.Models.Helicopter.HelicopterModel', 'Transform'),
    ('RR_OFF_HELI_SEATS', 'Models', 'Models.Models.Helicopter.HelicopterModel', 'Seats'),
    ('RR_OFF_ENTRY_GAME', 'Game', 'Start.UnityEntry', '_game'),
    ('RR_OFF_GAME_MODEL', 'Game', 'Game.Game', 'GameModel'),
    ('RR_OFF_GAME_BATTLE', 'Game', 'Game.GameModel', '_battleModel'),
    ('RR_OFF_BATTLE_ENTITIES', 'Battle', 'Game.Battle.BattleModel', 'BattleEntitiesModel'),
    ('RR_OFF_BATTLE_CAMERA', 'Battle', 'Game.Battle.BattleModel', 'BattleCameraModel'),
    ('RR_OFF_BATTLE_AIM', 'Battle', 'Game.Battle.BattleModel', 'AimTargetModel'),
    ('RR_OFF_BATTLE_WORLD', 'Battle', 'Game.Battle.BattleEntitiesModel', '_worldEntitiesModel'),
    ('RR_OFF_BATTLE_MODELS', 'Battle', 'Game.Battle.BattleEntitiesModel', '_models'),
    ('RR_OFF_BATTLE_SLOTS', 'Battle', 'Game.Battle.BattleEntitiesModel', '_characterSlotModel'),
    ('RR_OFF_VIEWS_BATTLE', 'Battle', 'Game.Battle.BattleEntitiesUpdater', '_battleEntitiesModel'),
    ('RR_OFF_VIEWS_MODEL', 'Battle', 'Game.Battle.BattleEntitiesUpdater', '_battleModel'),
    ('RR_OFF_VIEWS_CHARACTERS', 'Battle', 'Game.Battle.BattleEntitiesUpdater', '_characterEntityViews'),
    ('RR_OFF_VIEWS_CARS', 'Battle', 'Game.Battle.BattleEntitiesUpdater', '_carEntityViews'),
    ('RR_OFF_VIEWS_HELICOPTERS', 'Battle', 'Game.Battle.BattleEntitiesUpdater', '_helicopterEntityViews'),
    ('RR_OFF_AIM_BATTLE', 'Battle', 'Game.Battle.Entities.Player.Updaters.BattleAimTargetUpdater', '_battleEntitiesModel'),
    ('RR_OFF_AIM_BRAIN', 'Battle', 'Game.Battle.Entities.Player.Updaters.BattleAimTargetUpdater', '_cinemachineBrain'),
]

CLASS = re.compile(r'^\s*SDK_CLASS\("([^"]*)",\s*"([^"]*)",\s*"([^"]+)",\s*"([^"]+)",\s*(0x[0-9A-Fa-f]+)\)')
FIELD = re.compile(r'^\s*SDK_FIELD\((\w+),\s*(0x[0-9A-Fa-f]+),')


@dataclass(frozen=True)
class SDKField:
    assembly: str
    namespace: str
    klass: str
    full_name: str
    size: int
    name: str
    offset: int
    source: str
    line: int


def parse_fields(text, source):
    owner = None
    fields = []
    for line_no, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith('SDK_CLASS('):
            owner = CLASS.match(line)
            if not owner:
                raise ValueError(f'{source}:{line_no}: unsupported SDK_CLASS declaration')
        field = FIELD.match(line)
        if field:
            if owner is None:
                raise ValueError(f'{source}:{line_no}: SDK_FIELD outside SDK_CLASS')
            fields.append(SDKField(*owner.groups()[:4], int(owner[5], 16),
                                   field[1], int(field[2], 16), source, line_no))
    return fields


def sdk_directory(path):
    return path.parent if path.suffix == '.hpp' else path


def read_sdk(path):
    directory = sdk_directory(path)
    manifest = directory.parent / 'DumpManifest.sha256'
    expected = {}
    for line in manifest.read_text(encoding='utf-8-sig').splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]{64}) [ *](.+)', line)
        if match:
            expected[match[2]] = match[1].lower()
    records, sources = [], {}
    for name in SDK_FILES:
        relative = f'CppSDK/{name}'
        data = (directory / name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if expected.get(relative) != digest:
            raise ValueError(f'{relative}: SHA-256 does not match DumpManifest.sha256')
        sources[relative] = digest
        records.extend(parse_fields(data.decode('utf-8-sig'), relative))
    return records, sources


def resolve_field(records, klass, name, assembly=None):
    candidates = [field for field in records
                  if (field.full_name == klass or field.klass == klass)
                  and (assembly is None or field.assembly == assembly)
                  and field.name in (name, name + '_BackingField')]
    if not candidates:
        raise ValueError(f'MISSING {assembly or "*"}:{klass}.{name}')
    if len(candidates) != 1:
        locations = ', '.join(f'{f.assembly}:{f.full_name}@{f.source}:{f.line}' for f in candidates)
        raise ValueError(f'AMBIGUOUS {klass}.{name}: {locations}')
    field = candidates[0]
    if not 0x10 <= field.offset < field.size:
        raise ValueError(f'{field.source}:{field.line}: {klass}.{name} is outside its instance layout')
    return field


def build_artifacts(records, sources):
    fields = {}
    for macro, assembly, klass, name in FIELD_SPEC:
        field = resolve_field(records, klass, name, assembly)
        fields[macro] = dict(assembly=assembly, klass=klass, field=name,
                             offset=field.offset, instance_size=field.size,
                             source=field.source, line=field.line)
    lines = [
        '/* GENERATED by tools/sdk_offsets.py. Do not edit.',
        ' * Supplemental managed instance fields from the supplied UnityDumper CppSDK.',
        ' * Source files verified against DumpManifest.sha256.',
        ' * Native Unity offsets, generic payloads and method RVAs are not covered.',
    ]
    for source, digest in sources.items():
        lines.append(f' * {source} SHA-256: {digest}')
    lines += [' */', '#ifndef ROBRUN_SDK_OFFSETS_H', '#define ROBRUN_SDK_OFFSETS_H', '']
    for macro, field in fields.items():
        lines.append(f'#define {macro:32s} 0x{field["offset"]:X}u  /* {field["klass"]}.{field["field"]} */')
    lines += ['', '#endif /* ROBRUN_SDK_OFFSETS_H */', '']
    report = dict(schema=1, sources_sha256=sources, fields=fields)
    return '\n'.join(lines), json.dumps(report, indent=2) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=DEFAULT_SDK, help='CppSDK directory or Models.hpp')
    parser.add_argument('--header', type=Path, default=DEFAULT_HEADER)
    parser.add_argument('--report', type=Path, default=DEFAULT_REPORT)
    parser.add_argument('--check', action='store_true', help='Verify generated files without writing')
    args = parser.parse_args()
    try:
        records, sources = read_sdk(args.sdk)
        header, report = build_artifacts(records, sources)
        if args.check:
            for path, expected in ((args.header, header), (args.report, report)):
                if path.read_text(encoding='utf-8') != expected:
                    raise ValueError(f'{path}: differs from the SDK; regenerate with tools/sdk_offsets.py')
            print(f'Generated files match the SDK ({len(FIELD_SPEC)} fields).')
        else:
            for path, content in ((args.header, header), (args.report, report)):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding='utf-8', newline='\n')
            print(f'Generated {len(FIELD_SPEC)} fields: {args.header}, {args.report}')
    except (OSError, UnicodeError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
