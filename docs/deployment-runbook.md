# NAD Deployment Runbook

Один NAD-хост, единый CA, два независимых пула сертификатов —
`students` (Linux, LXD-контейнеры студентов) и `labs` (Windows, машины
лабораторий). Хаб-энд-спок: все агенты шлют трафик напрямую на
`pt-nad-rt.edtechlab.local`, без своих локальных CA/receiver'ов.

Живая версия с диаграммой: [NAD Deployment Runbook (artifact)](https://claude.ai/code/artifact/45e8620a-df79-4b6e-9551-b0a872834c0f).

```text
                         students · Linux
        host-a (LXD×10)  host-b (LXD×10)  host-c (LXD×10)
                    \            |            /
                     \           |           /
                      v          v          v
                     +--------------------------+
                     |           NAD             |
                     | pt-nad-rt.edtechlab.local  |
                     |     CA · mirror-receiver   |
                     +--------------------------+
                      ^                        ^
                     /                          \
              golden-образ                    клоны ×N
                         labs · Windows

              ~/mirror-certs/ — students/, labs/, ca.crt
```

## Фаза 0 · NAD — единая настройка

Один раз за всё время жизни стенда. Всё — на самом NAD-хосте, от root.

**1. Собрать и установить бинарники приёмника**

```sh
# mirror-receiver + mirror-interface + systemd unit'ы
./scripts/build.sh --clean --install
```

**2. Выпустить CA и серверный сертификат**

```sh
./scripts/nad-provision.sh init --nad-host 10.255.254.70
```

CN/SAN серверного сертификата — `pt-nad-rt.edtechlab.local`.
Идемпотентно: повторный запуск с тем же `--nad-host` ничего не
пересоздаёт.

**3. Засеять оба пула сертификатов**

```sh
./scripts/nad-provision.sh seed --pool students --count 100
./scripts/nad-provision.sh seed --pool labs     --count 100
```

> Размер пула фиксирован навсегда. Повторный `seed` с другим `--count`
> — ошибка, не рост. Если 100 когда-нибудь не хватит — это отдельное
> решение, не флаг.

## Фаза 1 · students — машины студентов (LXD)

Повторяется на каждой из 3 хост-машин. Пилот: по 10 студентов на
хост, 30 из 100 засеянных слотов.

**1. Занять и экспортировать партию слотов** (на NAD)

```sh
./scripts/nad-provision.sh occupy --pool students --slots 1-10
./scripts/nad-provision.sh export --pool students --slots 1-10 --out host-a.tar.gz
# перенести host-a.tar.gz на host-a (scp/USB — вне этого скрипта)
```

**2. Настроить lxd-lab и учётные данные** (на host-a)

```sh
$EDITOR /etc/lxd-lab/lab.conf         # student_from=1, student_to=10
$EDITOR /etc/lxd-lab/credentials.conf # заранее: name TAB password, по одному студенту на строку
lxd-lab doctor
lxd-lab image build
```

Пароли, уже вписанные в `credentials.conf`, не перегенерируются —
`deploy` заполнит только отсутствующие.

**3. Развернуть контейнеры** (на host-a)

```sh
lxd-lab plan    # посмотреть, что изменится, ничего не трогая
lxd-lab deploy
```

**4. Поставить агента в каждый контейнер** (на host-a)

```sh
tar xzf host-a.tar.gz
lxc file push student1_<uuid>.sh student1/root/
lxc exec student1 -- sh /root/student1_<uuid>.sh
# повторить для student2 .. student10
```

Скрипт сам разложит серты, настроит `agent.conf` и
`mirror-agent.service`, определит `capture_iface` по умолчанному
маршруту.

> Скрипт содержит приватный ключ агента в открытом виде — удалить
> после использования.

## Фаза 2 · labs — машины лабораторий (Windows)

Golden-образ собирается один раз; дальше — только перенос сертов в
каждый клон.

**1. Собрать golden-образ** (на эталонной Windows VM, один раз)

```powershell
.\scripts\deploy-windows.ps1 `
  -InstallersDir C:\deps\installers `
  -CertsPath C:\deps\certs `
  -AgentUuid <тестовый-uuid> -ReceiverHost 10.255.254.70
```

Регистрирует Scheduled Task `TraffMirrorAgent`, которая на каждой
загрузке запускает `start-agent.ps1`. Дальше — снять образ этой VM.

**2. Занять и экспортировать партию слотов** (на NAD)

```sh
./scripts/nad-provision.sh occupy --pool labs --slots 1-5
./scripts/nad-provision.sh export --pool labs --slots 1-5 --out labs-batch1.tar.gz
```

В архиве на слот: `machineN_<uuid>.crt/.key/.uuid`, плюс один общий
`ca.crt` на весь архив.

**3. Разложить серты по клонам** (при разворачивании каждого клона)

В `C:\deps\certs` клона (или другой `-CertsPath`):

```text
ca.crt
client.crt          # переименованный machineN_<uuid>.crt
client.key          # переименованный machineN_<uuid>.key
mirror-agent.uuid    # переименованный machineN_<uuid>.uuid
```

> Как именно файлы попадают на клон — решает внешний процесс
> разворачивания, не этот раннбук. Дальше `start-agent.ps1` сам
> подхватит их на следующей загрузке и переопределит `capture_iface`.

## Эксплуатация — учёт слотов и повторное использование

По мере того как потоки студентов заканчиваются, а лабораторные
машины выводятся из ротации.

**1. Посмотреть, что сейчас занято** (на NAD)

```sh
./scripts/nad-provision.sh status --pool students
./scripts/nad-provision.sh status --pool labs
```

**2. Освободить слоты отучившегося потока** (на NAD)

```sh
./scripts/nad-provision.sh release --pool students --slots 1-10
```

Сертификат и UUID слота не меняются — тот же слот просто станет
доступен для следующего потока через `occupy`.

| Команда | Что делает | Меняет статус? |
|---|---|---|
| `seed` | Выпускает пул раз и навсегда | — |
| `occupy` / `release` | Ручная пометка занят/свободен | да |
| `status` | Таблица free/occupied по пулу | нет |
| `export` | Упаковка файлов для переноса | нет, статус не трогает |

## Шпаргалка команд

### `nad-provision.sh`

```text
init    --nad-host HOST [--state-dir DIR]
seed    --pool NAME --count N [--state-dir DIR]
occupy  --pool NAME --slots SPEC [--state-dir DIR]
release --pool NAME --slots SPEC [--state-dir DIR]
status  --pool NAME [--state-dir DIR]
export  --pool NAME [--slots SPEC] --out FILE [--state-dir DIR]

# SPEC: "7" | "1-10" | "1,3,5-9". --state-dir по умолчанию $HOME/mirror-certs.
```

### `lxd-lab`

```text
lxd-lab config show   # эффективный конфиг и источник значений
lxd-lab doctor         # предусловия хоста
lxd-lab routes         # что прописать в таблицу маршрутизации YC
lxd-lab plan           # diff желаемого с фактическим
lxd-lab deploy         # привести стенд к желаемому состоянию
lxd-lab status         # фактическое состояние контейнеров
lxd-lab credentials show <имя>
lxd-lab credentials rotate <имя>
```

## Где что лежит

- Дизайн системы пулов сертификатов —
  `docs/superpowers/specs/2026-09-06-cert-pool-provisioning-design.md`
- План реализации students-пула —
  `docs/superpowers/plans/2026-09-06-cert-pool-provisioning.md`
- Полная ручная установка на три VM (для отладки) — `README.md` →
  «Полная установка на три VM»
- Тестирование на виртуальных машинах — `docs/vm-testing.md`
- История проблем и ограничений стенда — `docs/problem-log.md`
- lxd-lab: развёртывание и сеть студентов —
  `../Attackermachine2-master/README.md`
