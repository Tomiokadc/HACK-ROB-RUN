# ROB RUN — смещения полей

Unity `6000.3.18f1`, IL2CPP metadata v39.
SHA-256 `GameAssembly.dll`: `03003754353FF08380DC8E91920F2C77F7977B370D413083ACFE9B4BC65C18AB`.

Смещения указаны от начала экземпляра соответствующего класса. Извлечены статически через Cpp2IL.

| Класс | Поле | Смещение |
|---|---|---:|
| `BattleEntitiesModel` | `Player` | `0xD0` |
| `BattleEntitiesModel` | `_models`, словарь сущностей | `0x50` |
| `PlayerEntityModel` | `ClientModel` / `ServerModel` | `0x18` / `0x20` |
| `PlayerEntityModel` | `PublicId` | `0x28` |
| `BattleCharacterModel` | `Id` | `0x38` |
| `BattleCharacterModel` | `Transform` / `LastPositionModel` | `0x58` / `0x68` |
| `BattleCharacterModel` | `LookDirection` / `Velocity` | `0x70` / `0xB8` |
| `LastPositionModel` | `X` / `Y` / `Z`, `float` | `0x10` / `0x14` / `0x18` |
| `Models.Models.CharacterVelocity.CharacterVelocityModel` | `X` / `Y` / `Z`, `float` | `0x10` / `0x14` / `0x18` |
| `BattleCharacterModel` | `PrivateModel` | `0x10` |
| `BattleCharacterPrivateModel` | `Health` | `0x1B0` |
| `HealthModel` | `_health`, ссылка на `IPrimitiveData<int>` | `0x10` |
| `HealthModel` | `_maxAmount`, `int` | `0x20` |

```text
BattleEntitiesModel → +0xD0 PlayerEntityModel
  → +0x20 BattleCharacterModel
    → +0x10 BattleCharacterPrivateModel
      → +0x1B0 HealthModel
        → +0x10 IPrimitiveData<int>
```

Смещение значения текущего HP внутри конкретной реализации `IPrimitiveData<int>` и адрес живого экземпляра `BattleEntitiesModel` не проверены. `LastPositionModel` содержит последнюю позицию. Поле `Transform` ссылается на `ITransformModel`.

## Модель боя и камера

| Класс | Поле | Смещение |
|---|---|---:|
| `BattleUpdater` | `_battleModel` | `0xE0` |
| `BattleUpdater` | `_view` (`BattleScreenView`) | `0x10` |
| `BattleModel` | `BattleEntitiesModel` | `0x90` |
| `BattleScreenView` | `Camera` (`UnityEngine.Camera`) | `0x98` |
| `UnityEngine.Object` | `m_CachedPtr` | `0x10` |

Цепочки начинаются с экземпляра `BattleUpdater`. Адрес этого экземпляра в памяти и расположение матрицы в native-объекте камеры пока не подтверждены.

## Cinemachine и владелец экрана боя

| Класс или структура | Поле | Смещение |
|---|---|---:|
| `StartGameScreenUpdater` | `_battleUpdaters` (`BattleScreenUpdater`) | `0xB8` |
| `BattleScreenUpdater` | `_updater` (`IUpdater`) | `0x18` |
| `BattleScreenView` | `CameraBrain` (`CinemachineBrain`) | `0xA8` |
| `CinemachineBrain` | `m_OutputCamera` | `0x50` |
| `CinemachineBrain` | `CurrentCameraState` (встроенная структура) | `0xB0` |
| `CameraState` | `Lens` / `RawPosition` / `RawOrientation` | `0x00` / `0x4C` / `0x58` |
| `CameraState` | `PositionCorrection` / `OrientationCorrection` | `0x78` / `0x84` |
| `LensSettings` | `FieldOfView` / `OrthographicSize` | `0x00` / `0x04` |

Смещения `CameraState` и `LensSettings` отсчитываются от начала каждой структуры. Корневой адрес экземпляра в процессе ещё не подтверждён. `UnityEngine.Camera` получает `worldToCameraMatrix` и `projectionMatrix` через нативные методы. Смещения этих матриц не установлены.

## Цепочка от ScreenManager

Дизассемблирование подтвердило, что `ScreenManager` при создании `StartGameScreenUpdater` записывает его в `_currentUpdater` по `0x50`. Конструктор `BattleScreenUpdater` создаёт `BattleUpdater` и записывает его в `_updater` по `0x18`.

```text
ScreenManager → +0x50 StartGameScreenUpdater
  → +0xB8 BattleScreenUpdater → +0x18 BattleUpdater
    → +0xE0 BattleModel → +0x90 BattleEntitiesModel
    → +0x10 BattleScreenView → +0xA8 CinemachineBrain
```

Тип `ScreenManager._currentUpdater` зависит от активного экрана. Адрес экземпляра `ScreenManager` в процессе пока не установлен.


## Точка входа Unity

```text
Start.UnityEntry → +0x28 Game.Game
  → +0x90 StartGameProcessor → +0x20 ScreenManager
```

По нативному коду `UnityEntry.Awake` создаёт `Game.Game` и сохраняет его по `0x28`. При инициализации `Game.Game` создаёт `ScreenManager`, передаёт его в конструктор `StartGameProcessor` и сохраняет процессор по `0x90`. Конструктор процессора записывает ссылку на `ScreenManager` по `0x20`. Адрес живого компонента `Start.UnityEntry` пока не найден.

## Статический корень Cinemachine

Дизассемблирование `CinemachineCore.get_Instance()` и `GetActiveBrain()` подтверждает следующие разыменования для указанной сборки.

```text
klass        = read_ptr(GameAssembly_base + 0x8162748)
staticFields = read_ptr(klass + 0xB8)
core         = read_ptr(staticFields + 0x08)
activeBrains = read_ptr(core + 0x10)
```

`activeBrains` имеет тип `List<CinemachineBrain>`. Состояние камеры встроено в каждый экземпляр `CinemachineBrain` по `0xB0`. Цепочка проверена статически. Список может быть пуст до активации камеры. Выбор камеры боя, значения её состояния и корень `Start.UnityEntry` в процессе ещё не проверены.

## Прямая цепочка до модели боя

```text
Start.UnityEntry → +0x28 Game.Game → +0xD0 Game.GameModel
  → +0x10 BattleModel → +0x90 BattleEntitiesModel
```

Метод `ScreenManager.RegisterBattleModelForTesters` записывает модель боя в `GameModel._battleModel` по `0x10`. Слот зависит от жизненного цикла боя. Корень `Start.UnityEntry` и заполнение слота в запущенной игре ещё не проверены.

## Метаданные GameModel

`GameAssembly.dll + 0x81465C0` — слот метаданных класса `Game.GameModel`. При создании объекта код загружает этот слот в `RCX` перед вызовом аллокатора IL2CPP, а результат сохраняет в `Game.Game + 0xD0`. После инициализации метаданных слот подходит как ориентир для поиска экземпляров по типу. Адрес экземпляра и принадлежность активному бою нужно проверять в процессе.

## Счётчик сущностей

Для `Dictionary<int, IEntityModel>` из `BattleEntitiesModel + 0x50` нативный `get_Count()` вычисляет `int32(dict + 0x20) - int32(dict + 0x28)`. Это число занятых записей. Значение в запущенной сессии ещё не проверено.

## Проверка Cinemachine в процессе

Диагностический клиент прочитал один активный `CinemachineBrain` через цепочку с RVA `0x8162748`. Из `CurrentCameraState` получены FOV 60° и `RawPosition=(-6.47531, 11.5614, 6.33349)`. Эти значения относятся к одному снимку процесса. Проекция на экран и соответствие камеры активному бою ещё не проверены.

## Silent aim (источник: dump.txt, build 2026-11-17)

Код: `game/silent_aim.hpp`, класс `SilentAim`, конфиг `SilentAimConfig`. Работает только пока зажата кнопка огня, при отпускании возвращает исходное значение `LookDirection`.

| Класс | Поле | Смещение | Статус |
|---|---|---:|---|
| `CinemachineBrain` | `CurrentCameraState.Lens.FieldOfView` (FOV, вертикальный, градусы) | `0xB0` | verified (dump §7) |
| `CinemachineBrain` | `RawPosition` (Vector3) | `0xFC` | verified (dump §7) |
| `CinemachineBrain` | `RawOrientation` (Quaternion xyzw) | `0x108` | verified (dump §7) |
| `BattleEntitiesModel` | `Player` (PlayerEntityModel) | `0xD0` | verified |
| `BattleEntitiesModel` | `_models` (Dictionary<int, IEntityModel>) | `0x50` | verified (dump §4) |
| `Dictionary<int,V>` | `_entries` / `_count` | `0x18` / `0x20` | verified (dump §14) |
| `Entry[]` | `max_length` / данные | `0x18` / `0x20` | verified (dump §14) |
| `Entry` | `next` / `key` / `value*` (24 байта) | `0x4` / `0x8` / `0x10` | verified (dump §14) |
| `PlayerEntityModel` | `ServerModel` | `0x20` | verified (dump §4) |
| `BattleCharacterModel` | `LastPositionModel` | `0x68` | FILL: нет в dump 2026-11-17, только старая сборка |
| `LastPositionModel` | `X` / `Y` / `Z` (float) | `0x10` / `0x14` / `0x18` | FILL: нет в dump 2026-11-17 |
| `BattleCharacterModel` | `LookDirection` (Vector3) | `0x70` | FILL: нет в dump 2026-11-17, только старая сборка |

Как найти FILL в новой сборке: у `LookDirection` и `LastPositionModel` поиск по имени поля в Cpp2IL/Il2CppDumper для `Models.Models.BattleCharacter`. Если поля не нашли, pointer scan: поставить живого локального игрока, выписать адрес позиции через CE и проверить, что значения меняются при ходьбе.

Высота точки прицеливания: `SilentAimConfig::headHeight` = 1.6 м, эвристика, подбирается в игре.
