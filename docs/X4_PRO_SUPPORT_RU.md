# Поддержка XTEINK X4 Pro

X4 Pro — отдельная от обычного X4 аппаратная платформа. Обычный X4 использует
ESP32-C3, а X4 Pro — ESP32-S3 с 16 МБ flash и 8 МБ PSRAM. Поэтому образ X3/X4
нельзя безопасно прошивать в X4 Pro: для него добавлено отдельное окружение
PlatformIO `x4pro`.

## Источники и установленная конфигурация

Публичная спецификация производителя подтверждает экран 4,3 дюйма, 219 PPI,
сенсор, физические кнопки, регулируемую тёплую/холодную подсветку, Wi-Fi 2,4 ГГц,
Bluetooth, аккумулятор 1100 мА·ч и microSD до 256 ГБ. Радиомодуль и диапазон
частот также опубликованы в заявке FCC `2BTR9-X4PRO`.

Источники:

- [официальная спецификация и FAQ X4 Pro](https://www.xteink.com/blogs/product/x4-pro-faq-specs-support);
- [документы FCC для X4 Pro](https://fccid.io/2BTR9-X4PRO);
- [даташит ESP32-S3](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf);
- [контроллер сенсора Goodix GT911](https://www.goodix.com/en/product/touch/touch_screen_controller);
- [семейство bistable-драйверов Solomon Systech, включая SSD1677](https://www.solomon-systech.com/product-category/bistable-display/);
- [топливомер CellWise CW2017](https://en.cellwise-semi.com/Home/Index/prod_view?id=26);
- [аппаратный bring-up X4 Pro в FreeInk SDK](https://github.com/Free-Ink/freeink-sdk/blob/main/docs/xteink-x4pro-support.md).

Используемый профиль FreeInk SDK содержит следующую проверенную на устройстве
распиновку:

| Узел | Интерфейс и GPIO |
|---|---|
| Дисплей 800 × 480 | SCLK 12, MOSI 11, CS 13, DC 18, RST 14, BUSY 6 |
| Контроллер панели | SSD1677 или UC8179, определяется при каждой загрузке |
| GT911 | I²C SDA 39 / SCL 38, INT 10, RST 4, питание GPIO2 active-low, адрес `0x5D`/`0x14` |
| microSD | SDMMC 1-bit: CLK 41, CMD 42, DAT0 40, питание GPIO5 active-low |
| Батарея | CW2017, I²C `0x63` |
| RTC | BM8563, I²C `0x51` |
| Кнопки | левая GPIO0, правая GPIO7, Power GPIO3, active-low |
| Подсветка | холодный/тёплый PWM: GPIO8/GPIO9, 10 кГц, 10 бит |
| Общая периферийная линия | GPIO1 удерживается HIGH до запуска шин |

## Реализованная интеграция InkPointX

- добавлена отдельная сборка ESP32-S3 N16R8 с PSRAM;
- до инициализации экрана выполняется безопасное определение SSD1677/UC8179;
- SD-карта работает через нативный SDMMC block device, а не SPI;
- GT911 и ёмкостная Home-кнопка включены в общий ввод; касания по трём зонам и
  свайпы преобразуются в существующую навигацию, поэтому старые экраны доступны
  без переписывания каждого Activity;
- двойное короткое нажатие Power включает/выключает подсветку; яркость и теплота
  сохраняются в NVS;
- перед deep sleep отключаются оба PWM-канала подсветки, сенсор и питание SD;
- panic capture поддерживает Xtensa exception frame ESP32-S3, а не только
  RISC-V frame ESP32-C3;
- recovery-комбинация использует Right + Power и не требует держать strap GPIO0
  во время reset.

Для стендовой проверки подсветки через USB serial доступны команды:

```text
CMD:PROFILE_FRONTLIGHT
CMD:PROFILE_FRONTLIGHT:70
CMD:PROFILE_FRONTLIGHT:70:25
```

Первая переключает подсветку, вторая задаёт яркость, третья — яркость и долю
тёплого канала (0–100).

## Сборка и прошивка

Путь к проекту не должен содержать пробелы: pioarduino при первой компиляции
ESP-IDF библиотек отвергает такой путь.

```bash
git clone --branch dev-x4pro-test --recurse-submodules \
  https://github.com/yokki-vans/InkPointX.git InkPointX-x4pro
cd InkPointX-x4pro
pio run -e x4pro
```

Профиль использует отдельную разметку `partitions_x4pro.csv`: два OTA-раздела
по 7 МиБ и раздел SPIFFS 1,875 МиБ. Это оставляет прошивке запас роста и не
изменяет разметку обычных X3/X4-сборок.

Application image создаётся в `.pio/build/x4pro/firmware.bin` и записывается в
OTA-раздел по адресу `0x10000`. Для чистого устройства следует использовать
factory image из тестового пакета: он также содержит совместимые bootloader и
partition table.

## Аппаратная приёмка

Успешная компиляция подтверждает совместимость типов, линковку и размер образа,
но не может доказать электрическое поведение конкретной ревизии платы. Перед
пометкой сборки как стабильной на физическом X4 Pro нужно пройти:

1. холодный старт и определение контроллера панели в serial log;
2. полный, быстрый и частичный refresh без зависания BUSY;
3. чтение EPUB/FB2/PDF с microSD и повторный mount после deep sleep;
4. обе физические кнопки, Power, Home, касания углов и свайпы;
5. яркость 0/25/50/100 %, направление warm/cool и отсутствие свечения во сне;
6. показания CW2017 при заряде/разряде и сохранение времени BM8563;
7. 30 циклов sleep/wake и измерение тока сна;
8. восстановление после намеренно прерванного обновления через recovery.

Без подключённого X4 Pro честная гарантия прохождения этих пунктов невозможна;
результат сборки следует считать тестовой, а не production-прошивкой.
