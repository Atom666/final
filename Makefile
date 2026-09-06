CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS ?= -D_GNU_SOURCE -Iinclude
LDFLAGS ?=
LDLIBS_AGENT = -lssl -lcrypto -lpthread
LDLIBS_RECEIVER = -lssl -lcrypto -lxxhash -lpthread

BUILD_DIR := build
BIN_DIR := $(BUILD_DIR)/bin
OBJ_DIR := $(BUILD_DIR)/obj

COMMON_SRCS := src/common/config.c src/common/protocol.c src/common/tls.c src/common/util.c src/common/vlan.c
COMMON_OBJS := $(COMMON_SRCS:src/%.c=$(OBJ_DIR)/%.o)

AGENT_SRCS := src/agent/main.c src/agent/capture.c src/agent/platform.c src/agent/sync.c src/agent/segment.c src/agent/queue.c src/agent/batch.c src/agent/self_filter.c
AGENT_OBJS := $(AGENT_SRCS:src/%.c=$(OBJ_DIR)/%.o)

RECEIVER_SRCS := src/receiver/main.c src/receiver/client.c src/receiver/output.c src/receiver/pipeline.c src/receiver/fingerprint.c src/receiver/dedup.c src/receiver/checksum.c
RECEIVER_OBJS := $(RECEIVER_SRCS:src/%.c=$(OBJ_DIR)/%.o)

TEST_TOOL_COMMON_OBJ := $(OBJ_DIR)/test/traffic.o
TEST_TOOL_BINS := $(BIN_DIR)/mirror-test-sender $(BIN_DIR)/mirror-test-sink $(BIN_DIR)/mirror-test-verifier

TEST_SRCS := tests/test_protocol.c tests/test_queue.c tests/test_batch.c tests/test_vlan.c tests/test_self_filter.c tests/test_config.c tests/test_fingerprint.c tests/test_dedup.c tests/test_checksum.c tests/test_segment.c tests/test_pipeline.c tests/test_traffic.c
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BIN_DIR)/%)
HEADERS := $(shell find include -type f -name '*.h')

$(COMMON_OBJS) $(AGENT_OBJS) $(RECEIVER_OBJS) $(TEST_TOOL_COMMON_OBJ): $(HEADERS)

.PHONY: all clean test integration-test install

all: $(BIN_DIR)/mirror-agent $(BIN_DIR)/mirror-receiver $(TEST_TOOL_BINS)

$(BIN_DIR)/mirror-agent: $(COMMON_OBJS) $(AGENT_OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS_AGENT)

$(BIN_DIR)/mirror-receiver: $(COMMON_OBJS) $(RECEIVER_OBJS) $(OBJ_DIR)/agent/segment.o
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS_RECEIVER)

$(BIN_DIR)/mirror-test-sender: src/test/sender.c $(TEST_TOOL_COMMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lxxhash

$(BIN_DIR)/mirror-test-sink: src/test/sink.c $(TEST_TOOL_COMMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lxxhash

$(BIN_DIR)/mirror-test-verifier: src/test/verifier.c $(TEST_TOOL_COMMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lxxhash

$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN_DIR)/test_protocol: tests/test_protocol.c $(COMMON_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lssl -lcrypto

$(BIN_DIR)/test_queue: tests/test_queue.c $(OBJ_DIR)/agent/queue.o $(OBJ_DIR)/agent/sync.o $(OBJ_DIR)/common/util.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lpthread

$(BIN_DIR)/test_batch: tests/test_batch.c $(OBJ_DIR)/agent/batch.o $(OBJ_DIR)/agent/queue.o $(OBJ_DIR)/agent/sync.o $(OBJ_DIR)/common/protocol.o $(OBJ_DIR)/common/util.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lpthread

$(BIN_DIR)/test_vlan: tests/test_vlan.c $(COMMON_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lssl -lcrypto

$(BIN_DIR)/test_self_filter: tests/test_self_filter.c $(OBJ_DIR)/agent/self_filter.o $(OBJ_DIR)/agent/sync.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lpthread

$(BIN_DIR)/test_config: tests/test_config.c $(COMMON_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lssl -lcrypto

$(BIN_DIR)/test_fingerprint: tests/test_fingerprint.c $(OBJ_DIR)/receiver/fingerprint.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lxxhash

$(BIN_DIR)/test_dedup: tests/test_dedup.c $(OBJ_DIR)/receiver/dedup.o $(OBJ_DIR)/common/config.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lxxhash -lpthread

$(BIN_DIR)/test_checksum: tests/test_checksum.c $(OBJ_DIR)/receiver/checksum.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BIN_DIR)/test_segment: tests/test_segment.c $(OBJ_DIR)/agent/segment.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BIN_DIR)/test_pipeline: tests/test_pipeline.c $(OBJ_DIR)/receiver/pipeline.o $(OBJ_DIR)/receiver/output.o $(OBJ_DIR)/common/util.o $(OBJ_DIR)/common/config.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lpthread

$(BIN_DIR)/test_traffic: tests/test_traffic.c $(TEST_TOOL_COMMON_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lxxhash

test: $(TEST_BINS)
	$(BIN_DIR)/test_protocol
	$(BIN_DIR)/test_queue
	$(BIN_DIR)/test_batch
	$(BIN_DIR)/test_vlan
	$(BIN_DIR)/test_self_filter
	$(BIN_DIR)/test_config
	$(BIN_DIR)/test_fingerprint
	$(BIN_DIR)/test_dedup
	$(BIN_DIR)/test_checksum
	$(BIN_DIR)/test_segment
	$(BIN_DIR)/test_pipeline
	$(BIN_DIR)/test_traffic

$(BIN_DIR)/test_output_veth: tests/test_output_veth.c $(OBJ_DIR)/receiver/output.o $(OBJ_DIR)/common/util.o
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

integration-test: $(BIN_DIR)/test_output_veth
	@test "$$(id -u)" -eq 0 || { echo "integration-test requires root/CAP_NET_RAW and an existing veth pair"; exit 1; }
	./scripts/mirror-interface-setup.sh
	$(BIN_DIR)/test_output_veth

install: all
	install -d $(DESTDIR)/usr/local/sbin
	install -m 0755 $(BIN_DIR)/mirror-agent $(DESTDIR)/usr/local/sbin/mirror-agent
	install -m 0755 $(BIN_DIR)/mirror-receiver $(DESTDIR)/usr/local/sbin/mirror-receiver
	install -d $(DESTDIR)/usr/local/bin
	install -m 0755 $(TEST_TOOL_BINS) $(DESTDIR)/usr/local/bin/
	install -d $(DESTDIR)/usr/local/libexec
	install -m 0755 scripts/mirror-interface-setup.sh $(DESTDIR)/usr/local/libexec/mirror-interface-setup.sh
	install -d $(DESTDIR)/etc/mirror-agent $(DESTDIR)/etc/mirror-receiver
	install -m 0644 examples/agent.conf $(DESTDIR)/etc/mirror-agent/agent.conf.example
	install -m 0644 examples/receiver.conf $(DESTDIR)/etc/mirror-receiver/receiver.conf.example
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 systemd/*.service $(DESTDIR)/etc/systemd/system/

clean:
	rm -rf $(BUILD_DIR)
