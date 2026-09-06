# Кроссплатформенный агент зеркалирования и локальный L2-приёмник NAD для Linux

В этой экспериментальной реализации не используются ERSPAN и GRE. Linux-агенты
захватывают трафик через AF_PACKET/TPACKET_V3, Windows-агенты используют Npcap,
а затем оба варианта передают Ethernet-кадры в виде версионированных записей с
префиксом длины поверх TCP/mTLS. Приёмник работает только на Linux и инжектирует
восстановленные L2-кадры в локальную veth-пару на сервере NAD.

История обнаруженных на стенде проблем, исправлений и оставшихся ограничений:
[docs/problem-log.md](docs/problem-log.md).

```text
mirror-agent -> записи TCP/mTLS -> mirror-receiver -> AF_PACKET -> mirror-rx <-> nad-mirror -> NAD
```

## Сборка

```sh
./scripts/check-deps.sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cmake --parallel "$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

`Makefile` оставлен для совместимой сборки под Linux, но основной системой
сборки для дальнейшей разработки является CMake. Для Windows требуется CMake.

Полная процедура тестирования на виртуальных машинах приведена в
[docs/vm-testing.md](docs/vm-testing.md). Проверка полноты передачи пакетов с
помощью нумерованного UDP-трафика описана в
[docs/traffic-completeness-testing.md](docs/traffic-completeness-testing.md).

Для захвата и инжекции пакетов во время работы нужны Linux и право
`CAP_NET_RAW`. Обычные тесты выполняются в пользовательском пространстве и не
создают сетевые интерфейсы.

Для Debian/Ubuntu установите следующий набор пакетов:

```sh
sudo apt-get install build-essential cmake libssl-dev libxxhash-dev iproute2 systemd pkg-config
```

Для RHEL/CentOS/Fedora:

```sh
sudo dnf install gcc make cmake openssl-devel xxhash-devel iproute systemd pkgconf-pkg-config
```

## Сборка и запуск Windows-агента

Для Windows 10/11 x64 собирается только `mirror-agent.exe`, который захватывает
Ethernet-кадры через Npcap. `mirror-receiver`, вывод через AF_PACKET и veth-пара
`mirror-rx <-> nad-mirror` продолжают работать на Linux VM с NAD. Сетевой
протокол не менялся: Linux- и Windows-агенты могут одновременно подключаться к
одному приёмнику.

### 1. Необходимые компоненты

На Windows VM установите:

1. Visual Studio 2022 версии 17.5 или новее.
2. Набор компонентов **Desktop development with C++**, Windows SDK и средства CMake.
3. Git for Windows.
4. [Npcap Runtime](https://npcap.com/) — драйвер и динамические библиотеки для захвата.
5. [Npcap SDK](https://npcap.com/#download) — заголовки и библиотеки импорта для
   сборки. Распакуйте SDK, например, в `C:\deps\npcap-sdk`.

Проверьте структуру SDK:

```powershell
Test-Path C:\deps\npcap-sdk\Include\pcap.h
Test-Path C:\deps\npcap-sdk\Lib\x64\wpcap.lib
Test-Path C:\deps\npcap-sdk\Lib\x64\Packet.lib
```

Все три команды должны вернуть `True`. Среда выполнения Npcap и Npcap SDK — разные
компоненты: SDK нужен во время сборки, а среда выполнения должна быть установлена на каждой
Windows VM, где запускается агент.

### 2. Установка OpenSSL через vcpkg

В Developer PowerShell выполните следующие команды:

```powershell
New-Item -ItemType Directory -Force C:\deps | Out-Null
git clone https://github.com/microsoft/vcpkg C:\deps\vcpkg
C:\deps\vcpkg\bootstrap-vcpkg.bat
C:\deps\vcpkg\vcpkg.exe install openssl:x64-windows
$env:VCPKG_ROOT = "C:\deps\vcpkg"
```

Чтобы переменная сохранялась для новых терминалов:

```powershell
[Environment]::SetEnvironmentVariable(
  "VCPKG_ROOT", "C:\deps\vcpkg", "User"
)
```

CMake включает требуемые для MSVC параметры `/std:c11` и
`/experimental:c11atomics`.

### 3. Сборка через PowerShell-скрипт

Откройте **Developer PowerShell for VS 2022** или
**x64 Native Tools Command Prompt for VS 2022**, перейдите в корень исходного кода и
выполните:

```powershell
cd C:\src\traff-mirror

.\scripts\build-windows.ps1 `
  -NpcapRoot C:\deps\npcap-sdk `
  -VcpkgRoot C:\deps\vcpkg `
  -Configuration Release `
  -Clean
```

Скрипт собирает только Windows-агент и, если рабочего конфига ещё нет, копирует
рядом его шаблон. Результат:

```text
build-windows\bin\mirror-agent.exe
build-windows\bin\mirror-agent.conf
```

Повторная сборка без очистки:

```powershell
.\scripts\build-windows.ps1 `
  -NpcapRoot C:\deps\npcap-sdk `
  -VcpkgRoot C:\deps\vcpkg
```

### 4. Ручная CMake-сборка

Эквивалентная сборка без вспомогательного скрипта:

```powershell
cmake -S . -B build-windows `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DNPCAP_ROOT="C:\deps\npcap-sdk" `
  -DBUILD_TESTING=OFF `
  -DMIRROR_BUILD_RECEIVER=OFF `
  -DMIRROR_BUILD_TEST_TOOLS=OFF

cmake --build build-windows --config Release
```

### 5. Выбор интерфейса захвата

Запустите PowerShell от имени администратора:

```powershell
cd C:\src\traff-mirror\build-windows\bin
.\mirror-agent.exe --list-interfaces
```

Пример вывода:

```text
\Device\NPF_{01234567-89AB-CDEF-0123-456789ABCDEF}    Intel(R) Ethernet Adapter
```

В `capture_iface` укажите точное имя из первого столбца, включая
`\Device\NPF_`. Выбирайте Ethernet-адаптер, через который проходит рабочий
трафик VM и маршрут до приёмника. Агент отклоняет интерфейс обратной петли Npcap
и интерфейсы, тип канального уровня которых отличается от Ethernet
`DLT_EN10MB`.

### 6. UUID и mTLS-сертификаты

Для mTLS используйте явный стабильный UUID. CN клиентского сертификата должен
точно совпадать с `agent_uuid`:

```ini
agent_uuid = 33333333-3333-3333-3333-333333333333
```

Для этого UUID сертификат создаётся так же, как для Linux-агентов:

```sh
openssl genrsa -out windows-agent.key 2048
openssl req -new -key windows-agent.key -out windows-agent.csr \
  -subj "/CN=33333333-3333-3333-3333-333333333333"
openssl x509 -req -in windows-agent.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out windows-agent.crt -days 365 -sha256
```

Передайте на Windows VM и поместите в `build-windows\bin`:

```text
ca.crt
client.crt       # windows-agent.crt
client.key       # windows-agent.key
```

`tls_server_name` должен присутствовать в SAN серверного сертификата приёмника.
Для сертификатов из трёх-VM инструкции это `nad-mirror.internal`.

### 7. Настройка `mirror-agent.conf`

Откройте `build-windows\bin\mirror-agent.conf` и укажите реальные значения:

```ini
[agent]
agent_uuid = 33333333-3333-3333-3333-333333333333
capture_iface = \Device\NPF_{01234567-89AB-CDEF-0123-456789ABCDEF}
interface_id = 1

receiver_host = 192.168.1.78
receiver_port = 9443

ca_file = ca.crt
client_cert_file = client.crt
client_key_file = client.key
tls_server_name = nad-mirror.internal
tls_verify_peer = true
insecure_tls = false

max_capture_frame_size = 65575
segmentation_mode = receiver
```

Относительные пути к сертификатам считаются от текущего рабочего каталога,
поэтому запускайте агент из каталога с конфигом либо используйте абсолютные
пути. В конфигурации приёмника должно быть `max_input_frame_size >= 65575`.

### 8. Запуск и проверка

Сначала проверьте приёмник на Linux VM с NAD:

```sh
sudo systemctl restart mirror-interface.service mirror-receiver.service
sudo ss -ltnp | grep 9443
```

На Windows VM проверьте TCP-доступность:

```powershell
Test-NetConnection 192.168.1.78 -Port 9443
```

Затем в PowerShell от имени администратора:

```powershell
cd C:\src\traff-mirror\build-windows\bin

.\mirror-agent.exe .\mirror-agent.conf 2>&1 |
  Tee-Object -FilePath .\mirror-agent.log
```

В логе Windows-агента должны появиться строки:

```text
capture started backend=npcap
connected receiver=192.168.1.78:9443
Npcap self-transport BPF filter updated
connection_state=connected
```

На приёмнике проверьте регистрацию агента и получение записей:

```sh
sudo journalctl -u mirror-receiver.service --since "5 minutes ago" \
  | grep -E 'registered|receiver-summary|receiver-agent'

sudo tcpdump -eni nad-mirror -c 20
```

Остановка консольного агента выполняется через `Ctrl+C`.

### 9. Типовые ошибки Windows

- `Npcap adapter ... not found`: снова выполните `--list-interfaces` и вставьте
  точное имя устройства, а не описание адаптера.
- `Npcap activation/link type failed`: выбран интерфейс обратной петли или не-Ethernet
  интерфейс; выберите обычный Ethernet/vEthernet адаптер.
- `wpcap.dll` или `Packet.dll` не найдена: установите среду выполнения Npcap.
- Ошибка отсутствующего `libssl-3-x64.dll`/`libcrypto-3-x64.dll`: повторите
  сборку с файлом инструментальной цепочки vcpkg и проверьте DLL рядом с EXE
  либо доступность библиотек vcpkg в `PATH`.
- `TLS handshake failed`: проверьте доверие CA, срок сертификатов, совпадение CN
  клиента с `agent_uuid` и SAN сервера с `tls_server_name`.
- Нет пакетов или отказано в доступе: запустите PowerShell от администратора и
  проверьте, что среда выполнения Npcap установлена и её драйвер работает.
- Приёмник отклоняет `REGISTER`: сравните `max_capture_frame_size` агента и
  `max_input_frame_size` приёмника.

Npcap не предоставляет аналоги Linux `TP_STATUS_CSUMNOTREADY` и метаданных VLAN.
Поэтому исходящие кадры консервативно помечаются для завершения расчёта
контрольной суммы на приёмнике, а VLAN сохраняется только тогда, когда Npcap
возвращает тег непосредственно в кадре.

Текущая Windows-версия является консольной программой с обработкой `Ctrl+C`, а
не нативной службой Windows. Её пока нельзя напрямую регистрировать через
`sc.exe`.

## Автоматический деплой (nad-provision.sh)

Сначала соберите и установите бинарники на `nad` (`./scripts/build.sh
--clean --install`), затем один раз:

```sh
# На nad (root):
./scripts/nad-provision.sh init --nad-host 192.168.1.78
```

Команда генерирует CA и серверный сертификат (`CN`/`SAN` —
`pt-nad-rt.edtechlab.local`), устанавливает `/etc/mirror-receiver/*` и
включает `mirror-interface.service`/`mirror-receiver.service`. Повторный
запуск с тем же `--nad-host` — no-op.

Дальше один раз на весь пул нужных сертификатов (сейчас есть пул
`students`; `labs` для Windows-машин лабораторий появится отдельно):

```sh
./scripts/nad-provision.sh seed --pool students --count 100
```

Это выпускает 100 постоянных сертификатов и по одному самодостаточному
скрипту на слот в `/root/mirror-certs/students/student<N>_<uuid>.sh`
(под `sudo` `$HOME` — это `/root`). Повторный запуск с тем же `--count` —
no-op; с другим — ошибка (пул не растёт, см.
`docs/superpowers/specs/2026-09-06-cert-pool-provisioning-design.md`).

Скопируйте нужный `student<N>_<uuid>.sh` на целевую машину (LXD-контейнер
студента) и запустите там от root:

```sh
sudo ./student7_<uuid>.sh
```

Сертификаты, `agent.conf` и `mirror-agent.service` настроятся
автоматически; `capture_iface` определяется по интерфейсу маршрута по
умолчанию (переопределяется через `--iface`). Скрипт содержит приватный
ключ агента в открытом виде — удалите его после использования.

Учёт того, какие слоты сейчас в деле:

```sh
./scripts/nad-provision.sh occupy  --pool students --slots 1-10
./scripts/nad-provision.sh release --pool students --slots 7
./scripts/nad-provision.sh status  --pool students
```

Экспорт пачки скриптов для переноса на другую машину (по умолчанию — весь
пул; `occupy`/`release` не требуются и не меняются экспортом):

```sh
./scripts/nad-provision.sh export --pool students --out students.tar.gz
```

Ниже описан тот же процесс вручную — полезно для отладки или если нужно
изменить шаг вручную.

## Полная установка на три VM

Ниже приведён рабочий сценарий для стенда:

```text
agent-a: 192.168.1.36, ens33, UUID 11111111-1111-1111-1111-111111111111
agent-b: 192.168.1.37, ens33, UUID 22222222-2222-2222-2222-222222222222
nad-vm:  192.168.1.78, mirror-receiver и NAD

agent-a <-- iperf3 --> agent-b
     \                       /
      +---- TCP/mTLS :9443 --+--> mirror-receiver
                                   |
                               mirror-rx
                                   |
                               nad-mirror --> NAD (AF_PACKET)
```

TCP-порт `9443` на `nad-vm` должен быть доступен с обеих агентских VM. Все
команды сборки выполняются из корня проекта.

### 1. Установка пакетов и сборка

На всех трёх VM с Debian/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake libssl-dev libxxhash-dev pkg-config \
  iproute2 systemd openssl tcpdump ethtool iperf3 netcat-openbsd

cd ~/traff-mirror
./scripts/check-deps.sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cmake --parallel "$(nproc)"
ctest --test-dir build-cmake --output-on-failure
```

Собирать бинарники надёжнее на самих целевых VM. Бинарник, собранный на системе
с более новой glibc, может не запуститься на старой системе с ошибкой вида
`GLIBC_2.xx not found`.

Если на VM ещё нет исходников, перенесите их новым скриптом с машины разработки.
Он использует `tar` и `ssh`, поэтому устанавливать `rsync` на удалённой VM не
нужно:

```sh
cd /home/ubuntu/PT/traff-mirror
./scripts/transfer-sources.sh ubuntu@192.168.1.36
./scripts/transfer-sources.sh ubuntu@192.168.1.37
./scripts/transfer-sources.sh debian@192.168.1.78
```

По умолчанию исходники попадут в `~/traff-mirror`. Другой каталог можно передать
вторым аргументом:

```sh
./scripts/transfer-sources.sh \
  debian@192.168.1.78 /home/debian/traff-mirror
```

На каждой целевой VM соберите обе программы и запустите модульные тесты:

```sh
cd ~/traff-mirror
./scripts/build.sh --clean
```

Чтобы сразу установить исполняемые файлы, модули systemd и настроечный скрипт:

```sh
./scripts/build.sh --clean --install
```

### 2. Создание тестового CA и mTLS-сертификатов

Эти команды выполняются на `nad-vm`. CN каждого клиентского сертификата должен
точно совпадать с `agent_uuid` соответствующего агента.

```sh
mkdir -p ~/mirror-certs
chmod 700 ~/mirror-certs
cd ~/mirror-certs
umask 077

openssl genrsa -out ca.key 4096
openssl req -x509 -new -key ca.key -sha256 -days 3650 \
  -out ca.crt -subj "/CN=mirror-test-ca"

openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr \
  -subj "/CN=nad-mirror.internal" \
  -addext "subjectAltName=DNS:nad-mirror.internal,IP:192.168.1.78"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out server.crt -days 365 -sha256 \
  -copy_extensions copy

openssl genrsa -out agent-a.key 2048
openssl req -new -key agent-a.key -out agent-a.csr \
  -subj "/CN=11111111-1111-1111-1111-111111111111"
openssl x509 -req -in agent-a.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out agent-a.crt -days 365 -sha256

openssl genrsa -out agent-b.key 2048
openssl req -new -key agent-b.key -out agent-b.csr \
  -subj "/CN=22222222-2222-2222-2222-222222222222"
openssl x509 -req -in agent-b.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out agent-b.crt -days 365 -sha256
```

Установите серверные файлы на `nad-vm`:

```sh
sudo install -d -m 0755 /etc/mirror-receiver
sudo install -m 0644 ca.crt /etc/mirror-receiver/agents-ca.crt
sudo install -m 0644 server.crt /etc/mirror-receiver/server.crt
sudo install -m 0600 server.key /etc/mirror-receiver/server.key
```

Передайте клиентские файлы агентам:

```sh
scp ca.crt agent-a.crt agent-a.key ubuntu@192.168.1.36:/tmp/
scp ca.crt agent-b.crt agent-b.key ubuntu@192.168.1.37:/tmp/
```

На `agent-a`:

```sh
sudo install -d -m 0755 /etc/mirror-agent
sudo install -m 0644 /tmp/ca.crt /etc/mirror-agent/ca.crt
sudo install -m 0644 /tmp/agent-a.crt /etc/mirror-agent/client.crt
sudo install -m 0600 /tmp/agent-a.key /etc/mirror-agent/client.key
```

На `agent-b`:

```sh
sudo install -d -m 0755 /etc/mirror-agent
sudo install -m 0644 /tmp/ca.crt /etc/mirror-agent/ca.crt
sudo install -m 0644 /tmp/agent-b.crt /etc/mirror-agent/client.crt
sudo install -m 0600 /tmp/agent-b.key /etc/mirror-agent/client.key
```

### 3. Конфигурация приёмника

На `nad-vm` создайте рабочий конфиг:

```sh
sudo cp ~/traff-mirror/examples/receiver.conf /etc/mirror-receiver/receiver.conf
sudoedit /etc/mirror-receiver/receiver.conf
```

Основные значения должны выглядеть так:

```ini
[receiver]
listen_address = 0.0.0.0
listen_port = 9443
server_cert_file = /etc/mirror-receiver/server.crt
server_key_file = /etc/mirror-receiver/server.key
client_ca_file = /etc/mirror-receiver/agents-ca.crt
require_client_certificate = true
insecure_tls = false
max_clients = 1000
max_record_size = 1048576
max_input_frame_size = 65575
max_frame_size = 9216
registration_timeout_sec = 10
heartbeat_timeout_sec = 30

[dedup]
enabled = true
window_ms = 500
shards = 64
max_entries = 1000000
rejected_max_entries = 65536
timestamp_tolerance_ms = 500
mode = cross_agent
transport_max_entries = 4096

[agent_clock_offsets]
# 11111111-1111-1111-1111-111111111111 = 0
# 22222222-2222-2222-2222-222222222222 = 0

[output]
mode = af_packet
output_mode = shared_veth
interface = mirror-rx
required_mtu = 9216
queue_max_packets = 262144
queue_max_bytes = 536870912
reorder_window_ms = 20
reorder_max_flows = 131072
reorder_flow_timeout_sec = 60
scheduler_input_quantum_packets = 4096
scheduler_input_quantum_bytes = 16777216
scheduler_flow_quantum_packets = 64
batch_max_packets = 64
log_interval_sec = 10
```

Запустите `mirror-interface.service`: он автоматически вызовет идемпотентный
скрипт `mirror-interface-setup.sh`, создаст и настроит veth-пару. Затем запустите
приёмник:

```sh
sudo systemctl enable --now mirror-interface.service
sudo systemctl enable --now mirror-receiver.service
ip -br link show mirror-rx
ip -br link show nad-mirror
sudo ss -ltnp | grep 9443
sudo journalctl -u mirror-receiver.service -n 50 --no-pager
```

При обновлении существующего стенда сначала установите и перезапустите новый
приёмник. Только после его успешного запуска обновляйте агенты и включайте
`segmentation_mode = receiver`. Новый приёмник поддерживает как записи с
агрегированными пакетами, так и кадры, заранее сегментированные старым агентом.

Оба veth-интерфейса должны иметь состояние `UP` и MTU `9216`. IP-адреса на
них не нужны.

### 4. Конфигурация агентов

На каждой агентской VM:

```sh
sudo cp ~/traff-mirror/examples/agent.conf /etc/mirror-agent/agent.conf
sudoedit /etc/mirror-agent/agent.conf
```

На `agent-a` установите:

```ini
agent_uuid = 11111111-1111-1111-1111-111111111111
capture_iface = ens33
interface_id = 1
receiver_host = 192.168.1.78
receiver_port = 9443
ca_file = /etc/mirror-agent/ca.crt
client_cert_file = /etc/mirror-agent/client.crt
client_key_file = /etc/mirror-agent/client.key
tls_server_name = nad-mirror.internal
tls_verify_peer = true
insecure_tls = false
```

На `agent-b` используются те же значения, кроме UUID:

```ini
agent_uuid = 22222222-2222-2222-2222-222222222222
```

Остальные параметры кольцевого буфера, очереди и интервалов оставьте как в
примере. Проверьте доступность приёмника и запустите агенты:

Для захвата и программной сегментации агрегатов GRO/GSO в рабочем конфиге
обязательно должны присутствовать новые размеры:

```ini
frame_size = 131072
max_capture_frame_size = 65575
segmentation_mode = receiver
queue_max_packets = 65536
queue_max_bytes = 268435456
batch_max_records = 256
batch_max_bytes = 262144
batch_linger_us = 200
transport_write_timeout_sec = 10
```

Это внутренние размеры кольцевого буфера захвата, они не меняют MTU интерфейса VM. Агент
использует фактический MTU `capture_iface` как максимальный L3-размер создаваемых
TCP-сегментов. Пакетная отправка не меняет сетевой протокол: несколько полных
записей последовательно сериализуются в переиспользуемый буфер. `batch_linger_us`
ограничивает дополнительную задержку при слабом трафике, а тайм-аут записи в сокет
не позволяет отправляющему потоку TLS навсегда зависнуть на недоступном приёмнике.

```sh
nc -vz 192.168.1.78 9443
sudo systemctl enable --now mirror-agent.service
sudo journalctl -u mirror-agent.service -n 50 --no-pager
```

На `nad-vm` должны зарегистрироваться два UUID:

```sh
sudo journalctl -u mirror-receiver.service --since "5 minutes ago" \
  | grep 'registered'
```

### 5. Подключение NAD

В NAD выберите режим захвата **AF_PACKET** и интерфейс **`nad-mirror`**.
`mirror-rx` используется приёмником для отправки, а восстановленные кадры появляются
как входящий трафик на второй стороне veth — `nad-mirror`. DPDK для этой veth-схемы
не используется.

```sh
sudo ip link set nad-mirror up
sudo ip link set nad-mirror promisc on
ip -s link show nad-mirror
```

До подключения NAD поток можно проверить через `tcpdump`:

```sh
sudo tcpdump -eni nad-mirror -c 20
```

### 6. Ручной запуск для диагностики

Чтобы увидеть сообщения непосредственно в терминале, сначала остановите службу
systemd. На `nad-vm`:

```sh
sudo systemctl stop mirror-receiver.service
sudo /usr/local/sbin/mirror-receiver \
  /etc/mirror-receiver/receiver.conf 2>&1 \
  | tee /tmp/mirror-receiver.log
```

На агентской VM:

```sh
sudo systemctl stop mirror-agent.service
sudo /usr/local/sbin/mirror-agent \
  /etc/mirror-agent/agent.conf 2>&1 \
  | tee /tmp/mirror-agent.log
```

Не запускайте одновременно ручной процесс и соответствующую службу systemd.

### 7. Проверка через iperf3

На `agent-b` (`192.168.1.37`) запустите сервер:

```sh
iperf3 -s
```

Если порт уже занят, проверьте существующий процесс:

```sh
pgrep -a iperf3
```

Перед тестом на `nad-vm` сохраните последние накопительные счётчики:

```sh
sudo journalctl -u mirror-receiver.service --no-pager \
  | grep 'component=receiver-summary\|component=receiver-pipeline\|component=receiver-agent' \
  | tail -4
```

На `agent-a` запустите пяти минутный TCP-тест с четырьмя потоками:

```sh
iperf3 -c 192.168.1.37 -t 300 -i 10 -P 4
```

После завершения подождите один интервал логирования и снова снимите значения:

```sh
sleep 12
sudo journalctl -u mirror-receiver.service --no-pager \
  | grep 'component=receiver-summary\|component=receiver-pipeline\|component=receiver-agent' \
  | tail -4
```

Значения накопительные с момента запуска приёмника. Результат конкретного теста —
разница между снимками после и до теста:

- `packets_received` для каждого агента: сколько записей пришло от A и B;
- `cross_agent_deduplicated`: сколько вторых наблюдений было подавлено;
- `final_output_packets`: сколько кадров передано в `nad-mirror`;
- `sequence_gaps`: сколько последовательностей потеряно до приёмника;
- `output_errors` и `output_oversized`: ошибки после приёма.
- `drops_packet_limit`, `drops_byte_limit`, `drops_allocation`: причины отказа
  выходного конвейера;
- `peer_rescued_after_reject`: отклонённые наблюдения, которые удалось сохранить
  благодаря копии от второго агента;
- `rejected_observation_pairs`: пары наблюдений от разных агентов, обе копии
  которых были отклонены в диагностическом окне.

Проверьте статистику каждого агента:

```sh
sudo journalctl -u mirror-agent.service --no-pager \
  | grep 'connection_state=connected' | tail -1
```

Особенно важны `kernel_truncated`, `oversized_dropped`, `forwarded_aggregates`,
`segmentation_failures`, `ring_dropped`, `queue_dropped`,
`checksum_not_ready` и `send_errors`.
Для оценки ограничения производительности отправителя используйте интервальные метрики `ingress_pps`,
`ingress_mbps`, `egress_pps`, `egress_mbps`, текущие и пиковые размеры очереди,
`queue_residence_p50_us`, `queue_residence_p95_us`,
`queue_residence_p99_us`, `queue_residence_max_us`, `batch_avg_records`,
`batch_avg_wire_bytes`, `ssl_write_calls`, `ssl_write_avg_us`, `cpu_percent` и
`rss_bytes`. Если `ingress_pps` длительно выше `egress_pps`, а residence и
глубина очереди постоянно растут, увеличение лимита очереди лишь откладывает
потерю. `batch_failed_records` показывает записи, извлечённые в пакет отправки
при ошибке TLS; без подтверждения прикладного уровня неизвестно, какая их часть
уже была прочитана приёмником, поэтому агент не повторяет такую отправку
автоматически.
При `segmentation_mode=receiver` агент передаёт каждый видимый Linux агрегат
GRO/GSO/TSO размером до `max_capture_frame_size` одной записью. Приёмник разбивает
поддерживаемые агрегаты TCP/IPv4 и TCP/IPv6 по MTU, объявленному агентом в
`REGISTER`, до вычисления отпечатка, дедупликации и восстановления порядка.
`max_input_frame_size` ограничивает размер принимаемого агрегата независимо от
`max_frame_size`, который остаётся жёстким лимитом одного кадра при выводе через
AF_PACKET. Усечение ядром, IP-фрагменты, слишком большие UDP-пакеты, повреждённые
и неподдерживаемые агрегаты учитываются отдельно. Для обычных переданных пакетов
с незавершённой контрольной суммой агент по-прежнему устанавливает флаг
протокола, а приёмник рассчитывает итоговую контрольную сумму TCP/UDP для
IPv4/IPv6. Нулевые `sequence_gaps` приёмника сами по себе ещё не доказывают
полноту исходного захвата.

### 8. Быстрые проверки и остановка

Посмотреть кадры, которые получает NAD:

```sh
sudo tcpdump -ni nad-mirror
```

Проверить отсутствие GRE/ERSPAN:

```sh
sudo tcpdump -ni nad-mirror gre
```

Остановить компоненты:

```sh
# На agent-a и agent-b
sudo systemctl stop mirror-agent.service

# На nad-vm
sudo systemctl stop mirror-receiver.service
sudo systemctl stop mirror-interface.service
```

Повторно запустить:

```sh
# На nad-vm
sudo systemctl start mirror-interface.service mirror-receiver.service

# На agent-a и agent-b
sudo systemctl start mirror-agent.service
```

## Локальный интерфейс приёмника

Локальной veth-парой управляет `mirror-interface.service`. Вручную выполнять
`ip link add` при штатной установке не нужно:

```sh
sudo systemctl enable --now mirror-interface.service
ip -br link show mirror-rx
ip -br link show nad-mirror
```

Сервис вызывает `/usr/local/libexec/mirror-interface-setup.sh`. Скрипт безопасно
проверяет существующие интерфейсы, при необходимости создаёт
`mirror-rx <-> nad-mirror`, устанавливает MTU `9216` и поднимает обе стороны.
Приёмник отправляет кадры в `mirror-rx`, а NAD захватывает их с `nad-mirror`.

## Важные особенности поведения

- Идентификатор источника в версии 1 не кодируется в передаваемом Ethernet-кадре. Он доступен только в журналах и счётчиках приёмника.
- Linux не захватывает и не передаёт FCS, преамбулу и SFD.
- Настройки offload только считываются и не изменяются. По умолчанию большие нефрагментированные агрегаты TCP/IPv4 и TCP/IPv6 один раз проходят через TLS, после чего приёмник программно сегментирует их до MTU исходного интерфейса перед дедупликацией. Для созданных сегментов рассчитываются итоговые контрольные суммы IP/TCP. `segmentation_mode=agent` оставлен как резервный режим совместимости.
- `kernel_truncated` учитывает случаи `tp_snaplen < tp_len`. Счётчик агента `oversized_dropped` учитывает полностью захваченные кадры, превышающие его лимит захвата; счётчики приёмника `segmentation_failures` и `output_oversized` учитывают агрегаты, которые невозможно преобразовать в корректные выходные кадры.
- При переполнении очереди теряются только зеркальные копии; поток захвата не блокируется на сетевой отправке.
- Клиентские потоки приёмника никогда не пишут напрямую в AF_PACKET. Перед копированием кадра они резервируют место в ограниченной очереди. Единственный планировщик вывода забирает ограниченные порции входных данных, справедливо распределяет выходную квоту между активными TCP-потоками и записывает пакеты в `mirror-rx` группами через `sendmmsg`.
- Восстановление порядка TCP ограничено по ресурсам и работает по принципу fail-open. Куча сроков ожидания пробуждает только те потоки, для которых истекло время удержания. Выпуск по тайм-ауту дополнительно требует входной отметки, подтверждающей, что все принятые до этого срока кадры уже разобраны. Не-TCP, фрагментированные и неподдерживаемые кадры сохраняют порядок поступления в конвейер.
- Дедупликация на приёмнике выполняется попарно и только между разными агентами. Повторяющиеся пакеты одного агента сохраняются.
- IP-фрагменты обходят дедупликацию пакетов. Транспортные записи по-прежнему идемпотентны по UUID агента, эпохе и номеру последовательности.
- TTL дедупликации использует монотонные часы приёмника; временные метки агентов применяются только для настраиваемой проверки близости наблюдений.
- Границы программно созданных TCP-сегментов соответствуют MTU интерфейса и сохраняют поток байтов, пространство номеров TCP и значимые флаги. Такие сегменты подходят для повторной сборки TCP, но не гарантируют точного воспроизведения исходных границ сегментации физической сетевой карты или идентификаторов IPv4.
- Исправление контрольной суммы намеренно применяется только к записям, которые ядро захвата явно пометило как имеющие незавершённую контрольную сумму. Обычный входящий пакет с действительно неверной контрольной суммой не переписывается. Фрагменты и неподдерживаемые протоколы передаются без изменений и увеличивают `checksum_repair_failures`.

## Учёт трафика на приёмнике

В каждом интервале журналирования приёмник выводит строки
`component=receiver-summary`, `component=receiver-pipeline` и по одной строке
`component=receiver-agent` для каждого подключённого агента. Сводка содержит
основные счётчики нагрузочного теста:

- `packets_received_from_agents`: проверенные записи `PACKET`, полученные от всех агентов; одна запись может содержать агрегат TCP;
- `segmented_aggregates` / `generated_segments`: входные записи, развёрнутые приёмником, и получившиеся Ethernet-кадры;
- `cross_agent_deduplicated`: наблюдения кадров после сегментации, подавленные как совпадающая копия от другого агента;
- `final_output_packets`: кадры после сегментации, успешно инжектированные в `mirror-rx`;
- `output_queue_packets`: кадры, которые сейчас ожидают в центральной очереди или буферах восстановления порядка;
- `output_queue_drops`: попытки добавить наблюдение, отклонённые ограниченным выходным конвейером. Это не число уникальных пакетов, потерянных в сети, поскольку копия от другого агента всё ещё может быть принята;
- `peer_rescued_after_reject`: отклонённые наблюдения A/B, совпадающая копия которых позднее была принята;
- `rejected_observation_pairs`: пары, в которых копии от разных агентов были отклонены в пределах диагностического окна. При наличии ровно двух наблюдающих агентов этот показатель оценивает число уникальных пакетов, потерянных при допуске в приёмник; при большем числе агентов это только число пар;
- `reordered_packets`: пакеты, выпущенные раньше предыдущего по конвейеру пакета для восстановления порядка потока;
- `reorder_timeouts`: выпуски в режиме fail-open, когда разрыв последовательности сохранялся дольше времени удержания;
- `reorder_bypassed`: TCP-пакеты, выпущенные без восстановления порядка из-за заполнения ограниченного реестра потоков.

Строка каждого агента содержит такую же привязку входа и выхода по
`agent_uuid`, включая `packets_received`, `cross_agent_duplicates`,
`output_packets`, разрывы последовательности и счётчики ошибок. Значения
накопительные с момента запуска приёмника. Сохраните эти строки до и после
запуска `iperf3`, затем вычтите начальные значения, чтобы получить показатели
конкретного прогона.

После завершения обработки корректного трафика должны выполняться следующие
балансы:

```text
packets_received = transport_accepted + transport_duplicates + transport_out_of_order
transport_accepted = cross_agent_duplicates + output_packets + output_errors + output_queue_drops + oversized + processing_errors
post_segmentation_observations = transport_accepted + generated_segments - segmented_aggregates
post_segmentation_observations = cross_agent_duplicates + output_packets + output_errors + output_queue_drops + oversized + processing_errors
```

Первый баланс `transport_accepted = ...` применим только тогда, когда ни одна
запись не подвергалась сегментации. В режиме сегментации на приёмнике используйте
два баланса с `post_segmentation_observations`: число исходных записей и число
выходных кадров намеренно больше не совпадают.

Поля `transport_unaccounted` и `processing_in_flight` в строке агента показывают
временный пакет, обрабатывавшийся в момент снимка, либо реальное расхождение
учёта. После остановки трафика оба значения должны вернуться к нулю. Привязка
выхода показывает, наблюдение какого агента сохранилось после дедупликации, но не
изменяет передаваемый кадр.

Строка `receiver-pipeline` раздельно показывает отказы из-за лимита пакетов,
лимита байтов, ошибки выделения памяти и завершения работы. Она также содержит
текущую и пиковую входную очередь, очередь восстановления порядка, количество
принятых, разобранных и выведенных пакетов в секунду, время работы на предельной
ёмкости, число групп отправки и системных вызовов, длительность этапов, время
нахождения в конвейере и число проверенных пакетов на каждый выведенный
TCP-пакет.

## Гарантии протокола и идентификации

- Приёмник требует точного соответствия между префиксом длины и заголовком версионированной записи.
- В режиме mTLS общее имя (Common Name) клиентского сертификата должно совпадать с настроенным UUID агента. Для рабочего клиентского сертификата используйте явный и постоянный UUID.
- Значение `agent_uuid = auto` сохраняется в `uuid_file` (по умолчанию `/var/lib/mirror-agent/agent.uuid`). Модуль systemd создаёт этот каталог состояния.
- В версии 1 режим вывода приёмника явно задан как `shared_veth`; UUID доступен только в состоянии, журналах и счётчиках приёмника.
- Неизвестные, повреждённые и неподдерживаемые параметры конфигурации отклоняются, а не игнорируются без сообщения.

## Реализованные меры защиты

- Тайм-ауты регистрации и отсутствия контрольных сообщений, а также жёсткий лимит одновременно подключённых клиентов.
- Экспоненциальная задержка переподключения, контрольные сообщения, записи статистики и учёт ограниченной очереди.
- Классический сокетный BPF-фильтр, зависящий от соединения, и повторная пользовательская проверка пяти параметров IPv4. После переподключения фильтр заменяется, поскольку временный порт меняется.
- Проверка геометрии кольцевого буфера захвата, учёт потерь в кольцевом буфере ядра, временные метки TPACKET, флаги достоверности VLAN и проверка размера после восстановления кадра.

Команда `ctest --test-dir build-cmake` запускает тесты протокола и фрейминга,
сериализации регистрации, тайм-аутов и обратного давления очереди,
восстановления VLAN, распознавания собственного транспортного потока и строгой
проверки конфигурации. Привилегированные проверки veth, AF_PACKET, mTLS,
кадров увеличенного размера и нескольких агентов остаются в процедуре тестирования на VM,
поскольку им нужны сетевые пространства имён Linux или виртуальные машины и
права `CAP_NET_RAW`/`CAP_NET_ADMIN`.

Локальный побайтовый тест AF_PACKET/veth проверяет кадры размером 64, 512, 1514,
1518 и 9000 байт, а также настроенный лимит слишком больших кадров:

```sh
sudo cmake --build build-cmake --target integration-test
```
