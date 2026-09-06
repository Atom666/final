# Журнал проблем и исправлений

Документ фиксирует проблемы, обнаруженные при разработке и испытаниях
`mirror-agent` и `mirror-receiver` на трех VM. Значения счетчиков относятся к
конкретным тестовым прогонам и приведены как диагностические примеры.

Статусы:

- **Исправлено** — изменение реализовано и проверено тестами или на стенде.
- **Частично** — основная причина найдена, но требуется дополнительная проверка.
- **Открыто** — проблема локализована, окончательного решения пока нет.

## 1. Несовместимость GLIBC при переносе бинарников

**Симптом:** скопированный `mirror-receiver` не запускался на Debian 12:
`GLIBC_2.38 not found`.

**Причина:** бинарник был собран на Ubuntu с более новой GLIBC и динамически
ссылался на отсутствующую версию.

**Решение:** переносить исходники и собирать проект непосредственно на целевой
системе. Для этого добавлены `scripts/transfer-sources.sh` и `scripts/build.sh`.

**Статус:** **Исправлено**.

## 2. Не хватало build/runtime dependencies

**Симптом:** сборка или тестовые команды не выполнялись из-за отсутствующих
OpenSSL, xxHash, `iperf3`, `tcpdump`, `rsync` и других утилит.

**Решение:** добавлен `scripts/check-deps.sh`, а в инструкции перечислены пакеты
`build-essential`, `libssl-dev`, `libxxhash-dev`, `iproute2`, `openssl`,
`tcpdump`, `iperf3`, `netcat-openbsd` и `rsync`.

**Статус:** **Исправлено**.

## 3. Ошибки mTLS handshake

**Симптом:** агент постоянно выводил `TLS/register failed`, receiver —
`TLS handshake failed`.

**Причины:** несогласованный CA, неверные или отсутствующие client certificates,
а также конфигурация агента без путей к сертификатам при обязательном client
certificate на receiver.

**Решение:** сертификаты были очищены и выпущены заново от одного CA; проверены
issuer, subject, сроки действия и соответствие private key. В README добавлена
полная процедура установки сертификатов. `insecure_tls` используется только как
явный тестовый режим.

**Статус:** **Исправлено**.

## 4. Отсутствовала локальная veth-пара NAD

**Симптом:** не существовал `/sys/class/net/nad-mirror`, мониторинг показывал
`No such file or directory`.

**Причина:** не был создан локальный handoff-интерфейс receiver → NAD.

**Решение:** `mirror-interface.service` вызывает idempotent-скрипт
`scripts/mirror-interface-setup.sh`, который создает и поднимает
`mirror-rx <-> nad-mirror` с MTU 9216.

**Статус:** **Исправлено**.

## 5. NAD не видел трафик, хотя tcpdump видел

**Симптом:** `tcpdump -i nad-mirror` показывал кадры, а статистика NAD оставалась
нулевой.

**Причины:** был выбран неподходящий capture backend или основной процесс
`ptdpi` не был запущен. DPDK device list содержит физические PCI-интерфейсы, но
не локальную Linux veth.

**Решение:** для этой схемы настроены `capture_type: af-packet` и
`capture_if: nad-mirror`; проверяются `ptdpictl status-all`, ifindex и
`/proc/net/packet`. NAD должен захватывать `nad-mirror`, а receiver отправляет в
парный `mirror-rx`.

**Статус:** **Исправлено**.

## 6. Риск зацикливания собственного TLS-трафика агента

**Симптом:** транспорт зеркалирования идет через тот же интерфейс, который
захватывает агент, поэтому копия могла снова попасть в TLS-поток.

**Решение:** агент устанавливает socket BPF filter для собственного TCP flow и
дополнительно выполняет user-space 5-tuple check. Фильтр обновляется после
reconnect.

**Статус:** **Исправлено** для IPv4 Ethernet с поддерживаемой VLAN-формой;
IPv6 self-flow filter остается ограничением PoC.

## 7. Дублирование трафика от двух endpoint-агентов

**Симптом:** один сетевой пакет наблюдается как TX на одной VM и RX на другой,
поэтому receiver получал примерно две копии.

**Решение:** в receiver добавлены transport idempotency и bounded cross-agent
dedup по нормализованному `XXH3-128` fingerprint. Повторные наблюдения одного
агента сохраняются, а одна A/B-пара дает один output packet.

**Проверка:** в одном прогоне receiver получил 387 986 пакетов и подавил
189 973 cross-agent копии без transport duplicates и sequence gaps.

**Статус:** **Исправлено** на уровне попарной дедупликации. Глобальный порядок
кадров после объединения двух TLS-потоков рассматривается отдельно ниже.

## 8. Не было надежного учета полноты трафика

**Симптом:** по результату `iperf3` нельзя определить, сколько конкретных
пакетов потерялось в capture, queue, transport, receiver или NAD.

**Решение:** добавлены:

- agent counters для capture, truncation, ring/queue drops и send errors;
- receiver summary и отдельные per-agent counters;
- точные UDP sender/sink/verifier с `run_id` и packet ID;
- сравнение накопительных счетчиков до и после каждого прогона.

**Статус:** **Исправлено** для диагностики. `iperf3` остается нагрузочным
генератором, а не доказательством полноты доставки.

## 9. Переполнение очереди агента под высокой нагрузкой

**Симптом:** при тесте без offloads один агент показал `queue_dropped=9 932 174`,
второй — `1 902 923`; receiver зафиксировал ровно `11 835 097 sequence_gaps`.

**Причина:** после отключения GRO/GSO/TSO резко вырос packet rate, а один
capture thread и один TLS writer не успевали передавать копии.

**Повторная проверка с включенными offloads и software segmentation:** в
HTTP-прогоне на 10 000 сессий оба агента снова потеряли records:

- агент A: `captured=7 647 179`, `sent=6 670 030`,
  `queue_dropped=977 149` (12,78%);
- агент B: `captured=7 648 855`, `sent=6 156 079`,
  `queue_dropped=1 492 776` (19,52%);
- receiver показал `sequence_gaps=2 469 925`, что точно равно сумме
  `queue_dropped` двух агентов;
- на receiver при этом были `output_queue_drops=0`, `output_errors=0` и
  `output_oversized=0`.

Таким образом, потери происходят между capture/segmentation и TLS sender на
агентах, а не в receiver или локальном AF_PACKET output.

**Меры:** подтверждено точное соответствие queue drops и transport gaps;
используются bounded queue и `drop_newest`, чтобы агент не блокировал workload.
Для дальнейшего роста рассматриваются TLS batching, перенос segmentation на
receiver, memory pool, несколько transport workers/соединений,
VMXNET3/multiqueue либо отдельный transport NIC.

**Статус:** **Открыто** для предельной нагрузки. При контролируемой скорости
`queue_dropped=0`.

## 10. Сильное падение производительности после отключения offloads

**Симптом:** baseline без агентов снизился примерно с 3,04 Gbit/s до
593 Mbit/s; с агентами тест показал около 447 Mbit/s.

**Причина:** отключение segmentation/receive offloads увеличило PPS и стоимость
обработки каждого пакета. Дополнительным ограничением был виртуальный адаптер
VMware E1000.

**Решение:** offloads оставлены включенными (`offload_policy=observe`). Вместо
глобального отключения принято решение нормализовать агрегаты внутри агента.

**Статус:** **Частично**. Корректность agent-side segmentation реализована;
предельную производительность после изменения еще нужно измерить на VM.

## 11. BAD_CHECKSUM и невозможность распознать HTTP

**Симптом:** NAD показывал `BAD_CHECKSUM`, затем `MISSED_START` и `SSN_BROKEN`;
полные HTTP-сессии оставались обычными TCP-сессиями.

**Причина:** AF_PACKET видел outgoing пакеты с `TP_STATUS_CSUMNOTREADY`, то есть
до финального расчета checksum сетевым стеком/NIC.

**Решение:** агент передает protocol flag `CHECKSUM_NOT_READY`, receiver
рассчитывает IPv4/IPv6 TCP/UDP/ICMP checksum перед fingerprint и AF_PACKET
output. Добавлены counters `checksum_repairs` и `checksum_repair_failures` и
регрессионный тест на реальном поврежденном SYN из PCAP.

**Статус:** **Исправлено**. В проверенных прогонах failures были равны нулю.

## 12. Крупные GRO/GSO packets не помещались в capture/output limit

**Симптом:** агент A в HTTP-тесте показал `captured=71815` и
`truncated=32182`; NAD получил `GAP_DETECT` в 990 из 1000 длинных сессий.

**Причина:** старый capture ceiling был 9216 байт. Счетчик `truncated` смешивал
реальный случай `tp_snaplen < tp_len` и полностью захваченный aggregate больше
лимита. Такие records не получали sequence number, поэтому receiver мог
показывать `sequence_gaps=0`, хотя TCP payload уже отсутствовал.

**Решение:** в агент добавлена software segmentation:

- capture ceiling увеличен до 65575, ring frame — до 131072;
- TCP/IPv4 и TCP/IPv6 aggregates делятся по фактическому MTU capture interface;
- обновляются IP lengths, TCP sequence, FIN/PSH/CWR, IPv4 ID и checksums;
- добавлены `kernel_truncated`, `oversized_dropped`,
  `segmented_aggregates`, `generated_segments` и `segmentation_failures`;
- добавлены IPv4/IPv6/VLAN/extension-header unit tests.

**Статус:** **Частично**. Сегментация проверена на VM: `kernel_truncated=0`,
`oversized_dropped=0` и `segmentation_failures=0`. Однако генерация миллионов
обычных сегментов создала новый bottleneck очереди/TLS sender, описанный в
пунктах 9 и 17. Oversized UDP, fragments и malformed aggregates не
сегментируются и учитываются в `oversized_dropped`.

## 13. GAP_DETECT при объединении двух агентов

**Симптом:** для 1000 HTTP-сессий:

- только агент B: 15 `GAP_DETECT` среди 850 найденных HTTP-сессий;
- оба агента: 787 ошибок из 1000;
- только агент A: 990 ошибок из 1000.

**Причина:** агент A терял oversized GRO observations, а receiver объединял
неодинаковые представления одного потока из двух независимых TLS connections.
Текущая попарная дедупликация не гарантирует исходный глобальный порядок выдачи
кадров в `nad-mirror`.

**Решение:** реализованы два этапа:

- одинаковая agent-side TCP segmentation устраняет пропущенные aggregates и
  улучшает совпадение fingerprints;
- client threads больше не вызывают `AF_PACKET sendto` напрямую;
- добавлен bounded central output pipeline с одним writer;
- bidirectional TCP flow state учитывает обе стороны handshake;
- сегменты упорядочиваются по TCP sequence, а межнаправленный выбор использует
  adjusted timestamp;
- SYN/FIN sequence space и реальные retransmissions сохраняются;
- после `reorder_window_ms` поток продолжает работу fail-open;
- добавлены `output_queue_drops`, `reordered_packets`, `reorder_timeouts` и
  `reorder_bypassed`.

**Проверка после исправления:** в одном A+B прогоне NAD обработал 999 HTTP
сессий, из которых 6 имели ошибки (0,60%). Receiver изменил порядок 6 688
пакетов, не потерял ни одного принятого пакета и выполнил 134 fail-open timeout.
Одновременно агент A потерял 146 660 records в своей очереди, поэтому этот
прогон не доказывает полную корректность reorder.

В более тяжелом прогоне на 10 000 сессий NAD показал 1 807 сессий с
`MISSED_START`, `SSN_BROKEN` и `GAP_DETECT`. В том же прогоне оба агента суммарно
потеряли 2 469 925 records, а receiver зарегистрировал ровно столько же
`sequence_gaps`. Receiver не может восстановить TCP handshake или payload,
которые не были переданы ни одним агентом.

**Статус:** **Частично**. Центральный writer/reorder работает и измеряется, но
итоговую корректность NAD нельзя оценить до прогона с `queue_dropped=0` на обоих
агентах. Дополнительно остается проверить по PCAP необходимость строгого
приоритета `SYN -> SYN+ACK -> ACK` при объединении разных agent clocks.

## 14. Неверная интерпретация packet/session statistics NAD

**Симптом:** сначала казалось, что NAD получил только 671 из 10 000 соединений.

**Причина:** 671 сессия была распознана как HTTP, а еще 9334 находились в
обычных TCP/error категориях. Packet counters receiver, Ethernet bytes и
application bytes NAD также сравнивались как одинаковые величины.

**Решение:** отдельно сравниваются:

- `ab Complete requests`;
- nginx access log по уникальному `run_id`;
- TCP SYN и HTTP requests в контрольном PCAP на `nad-mirror`;
- HTTP и TCP/error sessions в NAD;
- agent/receiver counters за тот же временной интервал.

**Статус:** **Исправлено** на уровне методики тестирования.

## 15. Повторное использование run_id и рассинхронизация времени

**Симптом:** один вывод `ab` содержал 10 000 запросов, а nginx log по тому же
`run_id` — 20 000.

**Причина:** один идентификатор использовался в двух прогонах. Кроме того,
timestamp `run_id` и receiver logs отличались примерно на 203 секунды, что
усложняло сопоставление интервалов.

**Решение:** для каждого запуска используется `RUN_ID=$(date +%s%N)`, до теста
проверяется нулевое число записей, а время трех VM контролируется через
`timedatectl`/NTP.

**Статус:** **Исправлено** на уровне процедуры.

## 16. Недостаточная длительность и отсутствие baseline

**Симптом:** по 30-секундному `iperf3` делались выводы о производительности
агента без сравнения с теми же VM без зеркалирования.

**Решение:** измерения разделены на baseline без агентов, offload baseline и
прогон с агентами; рекомендуемая предварительная длительность увеличена до
5 минут. Длительный soak test имеет смысл только после достижения нулевых
capture/queue/transport/output losses в коротком контролируемом тесте.

**Статус:** **Исправлено** на уровне методики.

## 17. Agent-side segmentation многократно увеличивает packet rate

**Симптом:** при включенных GRO/GSO агенты больше не отбрасывают крупные
Linux-visible aggregates как oversized, но их bounded queues переполняются уже
обычными Ethernet-сегментами.

**Измерение:** в тяжелом HTTP-прогоне:

- агент A преобразовал 321 371 aggregates в 7 224 262 segments;
- агент B преобразовал 313 005 aggregates в 7 222 650 segments;
- число queue items и protocol records выросло примерно в 22 раза относительно
  числа исходных aggregates;
- `ring_dropped=0`, `kernel_truncated=0`, `segmentation_failures=0` и
  `send_errors=0`, но TLS sender не успевал за producer.

**Причина:** segmentation выполняется в capture thread до bounded queue. Каждый
получившийся сегмент требует отдельного allocation, элемента очереди,
protocol header и последовательности операций отправки. Общий объем payload
почти не уменьшается, но резко растут PPS, число блокировок и вызовов OpenSSL.

**Реализованный первый этап:**

1. Добавлен bulk-pop нескольких queue items под одной блокировкой.
2. Records сериализуются напрямую в reusable bounded buffer без отдельного
   `malloc/free` payload на каждый пакет.
3. Добавлены лимиты `batch_max_records`, `batch_max_bytes`, короткий
   `batch_linger_us` и `transport_write_timeout_sec`.
4. Добавлены ingress/egress rates, peak queue, residence p50/p95/p99/max,
   batch/TLS write, CPU и RSS метрики.
5. Wire-протокол не изменён: receiver получает тот же поток
   `[len][header][payload]...` и не зависит от границ `SSL_write`.
6. Ошибка TLS учитывает `batch_failed` и `batch_failed_records`. Автоматический
   retry невозможен без ACK, поскольку часть batch могла уже попасть receiver.

**Следующие этапы:**

1. Повторить 10k HTTP и 5-minute тесты и измерить реальный выигрыш.
2. Исправить транзакционность dedup cache и output queue reservation.
3. Разделить receiver input aggregate limit и output Ethernet frame limit.
4. Перенести TCP GRO segmentation на receiver: агент отправляет aggregate один
   раз, receiver сегментирует его до fingerprint/dedup/reorder/output.
5. Если одного sender недостаточно, рассмотреть bounded flow sharding по
   нескольким transport connections с сохранением порядка внутри flow.

**Статус:** **Частично**. Batching, метрики и unit-тесты реализованы; влияние на
`queue_dropped`, CPU/RSS и throughput требуется измерить на агентских VM.

## 18. Большая очередь противоречит ограничению ресурсов агента

**Симптом:** временное предложение увеличить очередь до миллионов пакетов и
нескольких гигабайт неприемлемо для рабочих VM, где агент не должен мешать
основной нагрузке.

**Причина:** увеличение bounded queue может поглотить кратковременный burst, но
не увеличивает устойчивую скорость TLS sender. Если ingress быстрее egress на
всем протяжении теста, большая очередь только откладывает drop и расходует RAM.

**Решение/ограничение:** сохранить малый явный лимит, например текущие
`queue_max_packets=65536` и `queue_max_bytes=268435456`, и повышать скорость
обработки через batching и перенос тяжелой нормализации на receiver. Изменение
лимитов допускается только после измерения доступной памяти и peak queue depth.

**Статус:** **Частично**. Batching реализован с дополнительным фиксированным
буфером 256 KiB; receiver-side segmentation ещё не реализована. Гигабайтная
очередь не считается целевым исправлением.

## 19. Двухсторонний захват маскирует часть потерь, но не гарантирует полноту

**Симптом:** при A+B тесте NAD мог показать только 6 ошибочных сессий из 999,
хотя агент A потерял 146 660 records. В более тяжелом тесте теряли уже оба
агента, после чего число поврежденных сессий резко выросло.

**Причина:** один и тот же wire packet обычно виден на обеих VM. Если потеряла
только одна сторона, наблюдение второго агента может сохраниться после
cross-agent dedup. Если оба агента теряют разные части одного TCP flow, receiver
получает неполную мозаику и NAD показывает `MISSED_START`, `SSN_BROKEN` или
`GAP_DETECT`.

**Вывод:** низкое число NAD errors в A+B прогоне не доказывает отсутствие
agent-side losses. Для каждого теста обязательно отдельно проверяются
`queue_dropped` обоих агентов и соответствующий `sequence_gaps` receiver.

**Статус:** **Исправлено** на уровне интерпретации метрик; транспортные потери
остаются открытой проблемой.

## Текущие критерии успешного прогона

Для стандартного `segmentation_mode=receiver` тест считается успешным, если:

```text
kernel_truncated=0
oversized_dropped=0
forwarded_aggregates>0
segmented_aggregates=0
generated_segments=0
segmentation_failures=0
ring_dropped=0
queue_dropped=0
send_errors=0
```

Для receiver:

```text
sequence_gaps=0
checksum_repair_failures=0
processing_errors=0
output_errors=0
output_oversized=0
segmented_aggregates>0
generated_segments>segmented_aggregates
segmentation_failures=0
```

Для HTTP-теста количество completed requests в `ab`, status 200 в nginx,
TCP/HTTP sessions в контрольном PCAP и sessions в NAD должно относиться к одному
`run_id` и одному временному интервалу. Ошибки `BAD_CHECKSUM`, `MISSED_START`,
`SSN_BROKEN`, `GAP_DETECT` и `OUT_OF_WINDOW` должны быть равны нулю.

## 20. Переполнение output queue после дедупликации могло удалить обе копии

**Симптом:** после внедрения agent batching receiver принял 13 703 850 пакетов,
дедуплицировал 6 668 592, но потерял ещё 1 279 723 пакета в центральной output
queue. Арифметика прогона сходилась:

```text
13 703 850 - 6 668 592 = 7 035 258
5 755 535 + 1 279 723 = 7 035 258
```

**Причина:** client-thread сначала добавлял наблюдение A в dedup cache, затем
пытался поставить кадр в output queue. Если очередь отвергала A, наблюдение всё
равно оставалось в cache и могло подавить копию B. В результате до NAD не
доходила ни одна из двух копий.

**Исправление:** операция сделана транзакционной внутри dedup shard. При первом
наблюдении output submit выполняется до публикации cache entry. При отказе
очереди entry не создаётся, поэтому копия другого агента может пройти. Проверка,
submit и публикация защищены одним shard mutex, поэтому параллельные client
threads не могут обойти этот порядок.

Добавлен unit-тест последовательности `A rejected -> B accepted -> A
suppressed`. Само переполнение output queue этим не устраняется: оно остаётся
отдельным ограничением производительности receiver.

**Статус:** **Исправлено** для корректности dedup/output; производительность
центральной output queue требует отдельной оптимизации.

## 21. Метрики queue residence использовали несовместимые часы

**Симптом:** агент выводил невозможные значения `queue_residence` порядка
`10^15` микросекунд и percentile `2251799813685248` микросекунд.

**Причина:** время enqueue измерялось через `CLOCK_MONOTONIC`, а dequeue и
длительность `SSL_write` через wall clock. Вычитание значений из разных шкал
давало величину, близкую к Unix timestamp.

**Исправление:** ingress/egress interval, enqueue/dequeue residence и
`SSL_write` duration теперь используют только `CLOCK_MONOTONIC`. Wall clock
сохранён для protocol timestamps и epoch, где требуется календарное время.

**Статус:** **Исправлено**; агентские бинарники нужно обновить перед следующим
сбором latency percentile.

## 22. Центральный output pipeline не успевал обслуживать принятый поток

**Симптом:** после исправления транзакционной дедупликации receiver отклонял
3 377 394-3 559 323 попытки enqueue за прогон. Это 34-36% admission attempts,
но не процент уникальных потерь: `A rejected -> B accepted` содержит один reject
без потери wire packet, а `A rejected -> B rejected` содержит два rejects для
одного потерянного packet.

**Причины:** прежний worker отсоединял весь input list, полностью разбирал его,
затем сканировал все flow buckets и дренировал hot flow. Reorder повторно
просматривал связный список, каждый кадр отправлялся отдельным `sendto`, а
capacity проверялась только после `calloc`, `malloc` и `memcpy`.

**Исправление:** реализован bounded scheduler:

1. Capacity резервируется до выделения и копирования frame buffer; packet-limit,
   byte-limit, allocation и stopping учитываются отдельно.
2. Input обрабатывается bounded quantum по packets и bytes.
3. Active flows обслуживаются round-robin с bounded output quantum.
4. Ожидающие gap flow находятся в deadline heap; полный flow-table scan остался
   только для редкой очистки пустых состояний.
5. Timeout разрешается только после ingress watermark: все observations,
   принятые до deadline, должны быть разобраны.
6. Обычная последовательная вставка hot-flow выполняется за O(1), поиск
   ограничивается первым ready candidate каждого направления.
7. AF_PACKET output объединяется до 64 frames в `sendmmsg`.
8. Добавлены current/peak stage backlog, PPS, stage timing, residence,
   time-at-capacity, batch/syscall и examined-per-emitted метрики.
9. Bounded rejected cache считает `peer_rescued_after_reject` и парную оценку
   `rejected_observation_pairs`; эти записи не участвуют в suppression.

**Проверка:** unit-тесты покрывают packet/byte limits, транзакционную пару
reject/rescue, двойной reject и 10 000 последовательных TCP segments одного
flow. Полная test matrix проходит под ASan/UBSan (LeakSanitizer отключён, потому
что среда запуска использует ptrace).

**Статус:** **Реализовано**, требуется повторный 10k HTTP прогон на трёх VM и
сравнение `receiver-pipeline` deltas с предыдущими измерениями.

## 23. Agent-side segmentation создавала миллионы TLS records

**Симптом:** один агент за HTTP-прогон видел около 290 тысяч GRO/GSO aggregates,
но до очереди создавал более 7,2 млн software segments. Даже после TLS batching
bounded queue достигала 65 536 элементов и теряла records.

**Исправление:** режим по умолчанию изменён на `segmentation_mode=receiver`.
Агент отправляет aggregate одним неизменённым protocol record и учитывает его в
`forwarded_aggregates`. Receiver принимает кадры до отдельного
`max_input_frame_size`, восстанавливает VLAN и сегментирует поддерживаемый TCP
aggregate по source MTU из `REGISTER` до fingerprint/dedup/reorder/output.
`max_frame_size` остаётся независимым ограничением AF_PACKET output.

Созданные сегменты имеют корректные IP/TCP checksums, TCP sequence и flags.
Старый `segmentation_mode=agent` сохранён как fallback. Wire version не менялась,
поэтому старые агенты совместимы с новым receiver; при обновлении новый receiver
нужно развернуть раньше новых агентов.

**Ожидаемый эффект:** вместо миллионов небольших records агент отправляет сотни
тысяч aggregates. Объём payload существенно не меняется, но резко снижаются PPS,
queue operations, record headers и работа capture thread.

**Статус:** **Реализовано**, требуется VM-проверка `forwarded_aggregates>0`,
`receiver_generated_segments_total>0`, `queue_dropped=0` и отсутствие NAD errors.

## 24. Linux-only API не позволял собрать агент для Windows

**Проблема:** агент напрямую зависел от `AF_PACKET`, `TPACKET_V3`, Linux socket
BPF, pthread, `/proc`, `ethtool`, POSIX time/socket API и Makefile. Простая
кросс-компиляция была невозможна: в Windows отсутствуют эти capture API, а тип
Winsock `SOCKET` нельзя безопасно считать обычным POSIX file descriptor.

**Исправление:** добавлена основная CMake-сборка и разделены платформенные слои.
Общий protocol/TLS/batching/queue код используется на обеих ОС. Linux сохраняет
существующий AF_PACKET backend, Windows использует отдельный Npcap backend,
Winsock, IP Helper API, Windows condition variables и process metrics.

Npcap backend:

1. перечисляет устройства через `--list-interfaces` и открывает точное device
   name;
2. принимает только Ethernet `DLT_EN10MB`;
3. задаёт snap length, timeout и bounded kernel buffer до activation;
4. исключает собственный IPv4 TCP/mTLS flow динамическим pcap BPF и повторной
   user-space 5-tuple проверкой;
5. определяет направление по MAC адаптера, консервативно помечает outbound
   checksum как незавершённую и передаёт oversized aggregates receiver;
6. учитывает Npcap `ps_drop` в существующей ring-drop метрике.

**Ограничения:** Npcap не предоставляет аналоги Linux VLAN metadata и
`TP_STATUS_CSUMNOTREADY`; доступны только теги, присутствующие inline, а
outbound checksum требует консервативной receiver-side финализации. Текущая
Windows-цель является console executable, а не нативным Windows Service.

**Статус:** **Реализовано в исходниках и CMake**. Linux CMake/Makefile сборки и
12 unit tests проходят. Windows target требует финальной сборки и capture-теста
на Windows VM с установленными Visual Studio, Npcap runtime/SDK и OpenSSL.
