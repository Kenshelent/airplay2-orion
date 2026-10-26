# AirPlay 2 для Яндекс Станции 3 (Orion)

Рабочая сборка **AirPlay 2** для колонки **Яндекс Станция 3** (`orion`, aarch64, glibc 2.36).

На iPhone / Mac колонка видна как **Я.Станция 3** (иконка HomePod mini, `AudioAccessory5,1`). Поток идёт по AirPlay 2, RTSP порт **7000**.

Это пересборка пакета AirPlay 2 с Мини 3 (armhf) под Orion: shairport-sync 4.3.7, nqptp 1.2.4, avahi, свой `airplayd` для кольца.

## Что умеет эта версия

- Алиса остаётся включённой. Quasar на загрузке не убивается.
- Звук в общий PCM `speakers` (`hw:0,1`, 48 кГц). Ультразвук — `hw:0,0`, его не трогаем.
- Громкость с телефона крутит тот же ALSA **Master**, что и Алиса.
- Кольцо Алисы не отбирается насовсем: при смене громкости — короткая шкала (~1.6 с), потом прежний кадр кольца.
- Avahi публикует то же `.local`-имя, что и прошивка, без reverse PTR, чтобы не драться с platformd.

## Состав

| Путь | Назначение |
|---|---|
| `airplay.sh` | setup / start / stop / install (автозапуск `S99zairplay`) |
| `airplayd.c` | громкость → Master + вспышка кольца |
| `airplayd.bin` | тот же `airplayd`, собранный в Debian bookworm aarch64 |
| `Dockerfile` | сборка всего стека |
| `ap2-orion-aarch64.tar.gz` | готовый payload для `/data/shairport` |
| `src/*.patch` | патчи shairport-sync 4.3.7 (модель HomePod, next/prev) |
| `LICENSES/`, `NOTICE.txt` | лицензии |

`alice_yield.sh`, `meta_watch.sh`, `vol_ring.sh` — черновики; в рабочем запуске не нужны.

## Сборка

На Raspberry Pi (aarch64) с Docker:

```sh
cd airplay2-orion
DOCKER_BUILDKIT=1 docker build --target out -o /tmp/ap2-orion .
```

Отдельно `airplayd`:

```sh
gcc -O2 -o airplayd airplayd.c -lm   # на bookworm aarch64, glibc 2.36
```

Нативный gcc новее bookworm соберёт бинарь с GLIBC_2.38 — на колонке он не стартует.

## Установка на колонку

Нужен root (SSH или ADB). Распаковать payload в `/data/shairport`:

```sh
adb push ap2-orion-aarch64.tar.gz /data/shairport.tgz
adb shell 'mkdir -p /data/shairport && tar -C /data/shairport -xzf /data/shairport.tgz && chmod +x /data/shairport/airplay.sh /data/shairport/airplayd /data/shairport/ap2/*'
adb shell /data/shairport/airplay.sh setup
adb shell /data/shairport/airplay.sh install
adb shell /data/shairport/airplay.sh start
```

Либо скопировать дерево после `docker build --target out`. Актуальный `airplayd` — `airplayd.bin` (на колонке имя файла `airplayd`).

Проверка: `airplay.sh status` — процесс `shairport-sync-ap2`, слушает `:7000`. В Пункте управления iPhone — **Я.Станция 3**.

## YAM (Y Application Manager)

Сайт https://yam.prismexec.tech/ ставит `yamd` и показывает каталог Yapkg. Загрузить пакет через сайт нельзя: каталог собирается из [nyakokitsu/yapkg](https://github.com/nyakokitsu/yapkg).

Готовый `.yap` (binary service, aarch64): [airplay2-orion-1.0.0.yap](https://github.com/Kenshelent/airplay2-orion/releases/download/v1.0.0/airplay2-orion-1.0.0.yap). В Web UI YAM включите «Разрешить бинарники плагинов».

Реестр этого репозитория:

```
https://raw.githubusercontent.com/Kenshelent/airplay2-orion/main/yam-registry/index.json
```

```sh
# с компьютера, где есть CLI yam и ADB/SSH к колонке
yam install --registry https://raw.githubusercontent.com/Kenshelent/airplay2-orion/main/yam-registry/index.json airplay2-orion
yam service enable airplay2-orion

# или скачать архив и поставить локально
yam install ./airplay2-orion-1.0.0.yap
yam service enable airplay2-orion
```

То же через переменную: `YAM_REGISTRY_URL=<url index.json> yam install airplay2-orion`.

Чтобы пакет попал в общий каталог на yam.prismexec.tech, нужен pull request в ветку `packages` у yapkg.

## Лицензии

shairport-sync — GPL-3.0, nqptp — GPL-2.0, avahi — LGPL-2.1+. Тексты в `LICENSES/`.
