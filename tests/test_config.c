#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mirror/config.h>

static int write_cfg(const char *text, char path[64]) {
    snprintf(path, 64, "/tmp/mirror-config-%ld.ini", (long)getpid());
    FILE *f = fopen(path, "w"); if (!f) return -1;
    fputs(text, f); fclose(f); return 0;
}

int main(void) {
    char path[64], err[256]; struct agent_config a; struct receiver_config r;
    assert(write_cfg("[agent]\nreceiver_port=9443\ninsecure_tls=true\nqueue_drop_policy=drop_newest\noffload_policy=observe\n", path) == 0);
    assert(load_agent_config(path, &a, err, sizeof(err)) == 0); unlink(path);
    assert(a.frame_size == 131072);
    assert(a.max_capture_frame_size == 65575);
    assert(a.batch_max_records == 256 && a.batch_max_bytes == 262144);
    assert(a.batch_linger_us == 200 && a.transport_write_timeout_sec == 10);
    assert(strcmp(a.segmentation_mode, "receiver") == 0);
    assert(write_cfg("[agent]\nreceiver_port=9443\ninsecure_tls=true\nqueue_drop_policy=drop_newest\noffload_policy=observe\nsegmentation_mode=agent\nbatch_max_records=32\nbatch_max_bytes=131072\nbatch_linger_us=50\ntransport_write_timeout_sec=3\n", path) == 0);
    assert(load_agent_config(path, &a, err, sizeof(err)) == 0); unlink(path);
    assert(a.batch_max_records == 32 && a.batch_max_bytes == 131072 &&
           a.batch_linger_us == 50 && a.transport_write_timeout_sec == 3);
    assert(strcmp(a.segmentation_mode, "agent") == 0);
    assert(write_cfg("[agent]\nreceiver_port=9443\ninsecure_tls=true\nqueue_drop_policy=drop_newest\noffload_policy=observe\nbatch_max_bytes=1024\n", path) == 0);
    assert(load_agent_config(path, &a, err, sizeof(err)) != 0); unlink(path);
    assert(write_cfg("[agent]\nreceiver_port=70000\n", path) == 0);
    assert(load_agent_config(path, &a, err, sizeof(err)) != 0); unlink(path);
    assert(write_cfg("[agent]\nreceiver_por=9443\n", path) == 0);
    assert(load_agent_config(path, &a, err, sizeof(err)) != 0); unlink(path);
    assert(write_cfg("[receiver]\nserver_cert_file=x\nserver_key_file=y\ninsecure_tls=true\n[dedup]\nmode=cross_agent\nwindow_ms=500\nshards=2\nmax_entries=10\nrejected_max_entries=8\ntimestamp_tolerance_ms=500\ntransport_max_entries=4\n[agent_clock_offsets]\n11111111-1111-1111-1111-111111111111=-10\n[output]\nmode=af_packet\noutput_mode=shared_veth\nqueue_max_packets=123\nqueue_max_bytes=456789\nreorder_window_ms=17\nreorder_max_flows=321\nreorder_flow_timeout_sec=45\nscheduler_input_quantum_packets=11\nscheduler_input_quantum_bytes=22222\nscheduler_flow_quantum_packets=7\nbatch_max_packets=16\n", path) == 0);
    assert(load_receiver_config(path, &r, err, sizeof(err)) == 0); unlink(path);
    assert(r.dedup_enabled && r.dedup_shards == 2 && r.clock_offset_count == 1 && r.clock_offsets[0].offset_ns == -10);
    assert(r.output_queue_max_packets == 123 && r.output_queue_max_bytes == 456789);
    assert(r.max_input_frame_size == 65575);
    assert(r.reorder_window_ms == 17 && r.reorder_max_flows == 321 &&
           r.reorder_flow_timeout_sec == 45);
    assert(r.dedup_rejected_max_entries == 8);
    assert(r.scheduler_input_quantum_packets == 11 &&
           r.scheduler_input_quantum_bytes == 22222 &&
           r.scheduler_flow_quantum_packets == 7 &&
           r.output_batch_max_packets == 16);
    assert(write_cfg("[receiver]\nserver_cert_file=x\nserver_key_file=y\ninsecure_tls=true\n[output]\nmode=af_packet\noutput_mode=shared_veth\nqueue_max_packets=0\n", path) == 0);
    assert(load_receiver_config(path, &r, err, sizeof(err)) != 0); unlink(path);
    assert(write_cfg("[receiver]\nserver_cert_file=x\nserver_key_file=y\ninsecure_tls=true\n[output]\nmode=erspan\noutput_mode=shared_veth\n", path) == 0);
    assert(load_receiver_config(path, &r, err, sizeof(err)) != 0); unlink(path);
    assert(write_cfg("[receiver]\nserver_cert_file=x\nserver_key_file=y\ninsecure_tls=true\n[dedup]\nmode=tcp_stream\n[output]\nmode=af_packet\noutput_mode=shared_veth\n", path) == 0);
    assert(load_receiver_config(path, &r, err, sizeof(err)) != 0); unlink(path);
    assert(strstr(err, "not implemented") != NULL);
    assert(write_cfg("[receiver]\nserver_cert_file=x\nserver_key_file=y\ninsecure_tls=true\n[agent_clock_offsets]\n11111111-1111-1111-1111-111111111111=0\n11111111-1111-1111-1111-111111111111=1\n[output]\nmode=af_packet\noutput_mode=shared_veth\n", path) == 0);
    assert(load_receiver_config(path, &r, err, sizeof(err)) != 0); unlink(path);
    assert(write_cfg("[receiver]\nserver_cert_file=x\nserver_key_file=y\ninsecure_tls=true\n[erspan]\n", path) == 0);
    assert(load_receiver_config(path, &r, err, sizeof(err)) != 0); unlink(path);
    return 0;
}
