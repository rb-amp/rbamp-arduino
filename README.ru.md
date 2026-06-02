# RbAmp — Arduino-библиотека для модулей rbAmp

[![protocol: 1.2](https://img.shields.io/badge/protocol-1.2-blue)](docs/02_tiers.md)
[![arduino: AVR · ESP32 · ESP8266 · STM32](https://img.shields.io/badge/arduino-AVR%20%C2%B7%20ESP32%20%C2%B7%20ESP8266%20%C2%B7%20STM32-brightgreen)](docs/04_hardware.md)
[![license: MIT](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

`RbAmp` — Arduino-библиотека для модулей **rbAmp**: компактных
аппаратных измерителей переменного тока и напряжения с интерфейсом
I²C. Модуль построен на микроконтроллере Cortex M0+, имеет на борту
изолированный аналоговый тракт и заводскую калибровку.

С точки зрения интегратора rbAmp ведёт себя как обычный I²C
slave-устройство: подключил питание, прочитал регистры — получил
готовые величины в физических единицах (вольты, амперы, ватты).
Никакой обработки сигнала на стороне ведущего не требуется.

То же самое API доступно на других платформах (ESP-IDF /
MicroPython / CPython / STM32 HAL) — переход между ними не требует
переучивания.

```cpp
#include <Wire.h>
#include <RbAmp.h>

RbAmp dev(Wire, 0x50);

void setup() {
    Serial.begin(115200);
    Wire.begin();
    while (!dev.begin()) { delay(1000); }
}

void loop() {
    RbAmpPeriodSnapshot snap;
    if (dev.readPeriodSnapshot(snap)) {
        Serial.print(F("P=")); Serial.print(snap.avg_p[0]); Serial.print(F(" W  "));
        Serial.print(F("Wh=")); Serial.println(dev.energy().wh(0), 4);
    }
    delay(60000);
}
```

## Подключение

Модуль подключается к ведущему четырьмя проводами: `VCC`, `GND`,
`SDA`, `SCL`. Опционально — `DRDY` (для прерываний по готовности
данных каждые ~200 мс).

| Провод | Уровень |
|---|---|
| `VCC` | **5 В (4.5..5.5 В)** — на борту модуля стабилизатор и фильтрация |
| `GND` | общий с ведущим (обязательно) |
| `SDA`, `SCL` | 3.3 В логика, **5 В-толерантны** — работает и с 3.3 В мастером (ESP32), и с 5 В (Arduino UNO/Nano) |
| `DRDY` | open-drain, 3.3 В уровень, ~10 мкс LOW каждые ~200 мс |

На плате установлены **встроенные pull-up резисторы 4.7 kΩ к 3.3 В**
— для одиночного модуля внешние pull-ups не нужны. На многомодульной
шине их отключают перерезанием перемычки `Pull-Up` (см.
[04_hardware.md](docs/04_hardware.md)).

Адрес по умолчанию `0x50` (7-бит), скорость 100 кГц (Standard mode)
или 400 кГц (Fast mode).

## Установка

### Arduino IDE — Library Manager

`Sketch → Include Library → Manage Libraries…` → найти **RbAmp** →
**Install**. Зависимостей нет. Примеры появляются в
`File → Examples → RbAmp` после установки.

### Arduino CLI

```sh
arduino-cli lib install RbAmp
```

### PlatformIO

```ini
[env:esp32dev]
platform   = espressif32
framework  = arduino
lib_deps   = rbamp/RbAmp@^1.0.0
```

### Вручную

```sh
git clone https://github.com/rb-amp/rbamp-arduino.git \
    "$HOME/Arduino/libraries/RbAmp"
```

## Поддерживаемые платформы

| Ядро | Статус | Примечания |
|---|---|---|
| Arduino AVR (Uno / Mega / Nano) | работает | Wh-аккумулятор на 32-битном float (см. ниже) |
| arduino-esp32 (ESP32 / S2 / S3 / C3) | работает | |
| arduino-esp8266 | работает | |
| STM32duino (F1 / F4 / G4) | работает | |
| SAMD / RP2040 (arduino-pico) | должно работать | проверено в ограниченном объёме |

## Что даёт библиотека

Модуль возвращает **только мгновенные/усреднённые величины** —
напряжение, ток, мощность в ваттах. Учёт энергии (Wh) ведёт сама
библиотека по часам ведущего:

```text
E_Wh += PERIOD_AVG_P_W × master_dt_seconds / 3600
```

Это даёт ровно ту же временную базу для всех модулей в системе
(см. [04_period_metering.md](https://github.com/rb-amp/rbamp-spec/blob/main/docs/04_period_metering.md) в каноническом
спецификационном репо), без перерасчёта между внутренними часами устройств.

Дальше:

- **Класс `RbAmp`** — один экземпляр на каждый модуль на шине.
  Именованные методы для всех величин: `readVoltage()`,
  `readPower(ch)`, `readPowerFactor(ch)`, `readFrequency()`,
  `readPeriodSnapshot(&snap)`, `setSensorClass(class)`,
  `setCTModel(code)` и т.д.
- **Конфигурация датчика тока** — два вызова: `setSensorClass(class)`
  выбирает тип сенсора (SCT-013 / встроенный CT / проводной CT),
  `setCTModel(code)` выбирает модель из соответствующего семейства.
  Калибровочные коэффициенты загружаются автоматически из заводской
  таблицы.
- **Накопитель Wh** на канал — `dev.energy().wh(ch)` возвращает
  текущее значение, накопленное библиотекой. Обновляется
  автоматически после каждого успешного `readPeriodSnapshot()`.
  Поведение зависит от тира модуля — см. [02_tiers.md](docs/02_tiers.md).
- **POD-структуры** `RbAmpSnapshot` / `RbAmpPeriodSnapshot` — все
  поля одного снимка в одной структуре.
- **Сокрытие протокольных деталей** — порядок байтов, времена
  выдержки после команд, проверка флагов готовности — всё внутри.
  Пользовательский код вызывает методы, а не пишет в регистры.

## Документация

| Документ | Назначение |
|---|---|
| [01 · Обзор](docs/01_overview.md) | что такое rbAmp, что делает библиотека, сравнение с прямым доступом к регистрам |
| [02 · Тиры модулей](docs/02_tiers.md) | какой тир (BASIC / STANDARD / PRO) под какую задачу |
| [03 · Выбор датчика тока](docs/03_sensor_selection.md) | как выбрать SCT-013 (5A / 10A / 30A / 50A / 100A) и сообщить модулю через `setCTModel()` |
| [04 · Подключение](docs/04_hardware.md) | распиновка, схема включения для разных Arduino-хостов |
| [05 · Quickstart](docs/05_quickstart.md) | первый рабочий скетч за 5 минут |
| [06 · Примеры](docs/06_examples.md) | разбор готовых скетчей из `examples/` |
| [07 · DIY-интеграции](docs/07_diy_integrations.md) | Home Assistant / Node-RED / OpenHAB |
| [08 · Облачные интеграции](docs/08_cloud_integrations.md) | AWS IoT / Azure / GCP / InfluxDB |
| [09 · API reference](docs/09_api_reference.md) | полный публичный API библиотеки |
| [10 · Диагностика](docs/10_troubleshooting.md) | типовые проблемы и как их разобрать |
| [11 · Changelog](docs/11_changelog.md) | история изменений библиотеки |

Описание самого протокола обмена (общего для всех клиентских библиотек)
живёт в репозитории [`rbamp-spec`](https://github.com/rb-amp/rbamp-spec).

## Примеры

Готовые скетчи в [`examples/`](examples/):

1. [`01_QuickRead`](examples/01_QuickRead/) — простое чтение U / I / P / PF раз в секунду
2. [`02_PeriodEnergyOLED`](examples/02_PeriodEnergyOLED/) — счётчик энергии на OLED 128×64
3. [`03_MultiModuleBroadcast`](examples/03_MultiModuleBroadcast/) — 3 модуля на одной шине, синхронные периоды
4. [`04_UI3PerChannelMQTT`](examples/04_UI3PerChannelMQTT/) — модуль UI3 + публикация по каналам в MQTT
5. [`06_BidirectionalEnergy`](examples/06_BidirectionalEnergy/) — раздельный учёт потребления и отдачи (мастер-сторонний)
6. [`07_DeepSleepLogger`](examples/07_DeepSleepLogger/) — логгер на батарейке с deep-sleep

Подробный разбор каждого скетча — в [docs/06_examples.md](docs/06_examples.md).

## Совместимость

Библиотека работает с прошивкой модуля **v1.0..v1.2**:

| Прошивка | REG_VERSION | Новое в этой версии |
|---|---|---|
| v1.0 | `0x01` | базовая публикация |
| v1.1 | `0x02` | `REG_TOPOLOGY` (0x24) — автоопределение числа каналов |
| **v1.2** | **`0x03`** | `REG_SENSOR_CLASS` (0x25), частота семплирования 10 кГц |

Все версии библиотеки работают со всеми версиями прошивки.
Несуществующие на старой прошивке регистры возвращают `0x00`
(для байтовых) или `0.0f` (для float). Например, библиотека v1.2,
читающая `REG_SENSOR_CLASS` (0x25) на v1.0 прошивке, получит
`0x00 = UNSET` и автоматически попадёт в fallback-режим
конструктор-подсказки (дефолт `setSensorClass(SCT_013)` или
topology hint из конструктора).

## Связанные библиотеки

То же API на других платформах:

- **ESP-IDF** — [`rbamp-esp-idf`](https://github.com/rb-amp/rbamp-esp-idf)
- **MicroPython + CPython** — [`rbamp-python`](https://github.com/rb-amp/rbamp-python)
- **STM32 HAL** — [`rbamp-stm32-hal`](https://github.com/rb-amp/rbamp-stm32-hal) *(в работе)*
- **ESPHome** — [`rbamp-esphome`](https://github.com/rb-amp/rbamp-esphome)

## Лицензия

MIT — см. [LICENSE](LICENSE).
