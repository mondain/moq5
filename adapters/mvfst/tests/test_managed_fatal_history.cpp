/* No-network fatal exit with a real publisher's reserved track history.
 * Include the implementation privately; this target must not also link the
 * production mvfst_adapter.cpp object. No installed test seam is needed. */
#include "../src/mvfst_adapter.cpp"
#include <moq/publisher.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace {
struct Block {
    void *ptr = nullptr;
    size_t size = 0;
};

struct Tracker {
    std::mutex mu;
    std::array<Block, 4096> blocks{};
    size_t live_bytes = 0;
    size_t live_blocks = 0;
    size_t errors = 0;
    void *session = nullptr;
    size_t session_frees = 0;

    bool contains(void *p) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto &b : blocks) if (b.ptr == p && p) return true;
        return false;
    }

    void *find_key(const unsigned char *key, size_t size) {
        std::lock_guard<std::mutex> lock(mu);
        void *found = nullptr;
        for (const auto &b : blocks) {
            if (b.ptr && b.size == size && std::memcmp(b.ptr, key, size) == 0) {
                if (found) { errors++; return nullptr; }
                found = b.ptr;
            }
        }
        return found;
    }
};

void *tracked_alloc(size_t size, void *ctx) {
    auto &t = *static_cast<Tracker *>(ctx);
    void *p = std::malloc(size ? size : 1);
    if (!p) return nullptr;
    std::lock_guard<std::mutex> lock(t.mu);
    for (auto &b : t.blocks) {
        if (!b.ptr) {
            b = {p, size};
            t.live_bytes += size;
            t.live_blocks++;
            return p;
        }
    }
    t.errors++;
    std::free(p);
    return nullptr;
}

void tracked_free(void *p, size_t size, void *ctx) {
    if (!p) return;
    auto &t = *static_cast<Tracker *>(ctx);
    std::lock_guard<std::mutex> lock(t.mu);
    for (auto &b : t.blocks) {
        if (b.ptr == p) {
            if (b.size != size) t.errors++;
            t.live_bytes -= b.size;
            t.live_blocks--;
            if (p == t.session) t.session_frees++;
            b = {};
            std::free(p);
            return;
        }
    }
    t.errors++;  /* Do not double-free after an allocator contract failure. */
}

void *tracked_realloc(void *p, size_t old_size, size_t size, void *ctx) {
    if (!p) return tracked_alloc(size, ctx);
    if (!size) { tracked_free(p, old_size, ctx); return nullptr; }
    auto &t = *static_cast<Tracker *>(ctx);
    {
        std::lock_guard<std::mutex> lock(t.mu);
        bool valid = false;
        for (const auto &b : t.blocks)
            if (b.ptr == p && b.size == old_size) valid = true;
        if (!valid) { t.errors++; return nullptr; }
    }
    void *next = tracked_alloc(size, ctx);
    if (!next) return nullptr;
    // The allocator contract supplies the old allocation extent.
    std::memcpy(next, p, old_size < size ? old_size : size);
    tracked_free(p, old_size, ctx);
    return next;
}

struct Fixture {
    Tracker tracker;
    moq_alloc_t alloc{};
    moq_publisher_t *publisher = nullptr;
    moq_pub_track_t *track = nullptr;
    void *history_key = nullptr;
    std::thread::id pump_tid;
    unsigned pump_calls = 0;
    bool setup_ok = false;

    Fixture() {
        alloc.ctx = &tracker;
        alloc.alloc = tracked_alloc;
        alloc.realloc = tracked_realloc;
        alloc.free = tracked_free;
    }
};

int fatal_pump(moq_mvfst_managed_t *m, moq_mvfst_managed_lane_t *,
               uint64_t now, void *ctx) {
    auto &f = *static_cast<Fixture *>(ctx);
    f.pump_calls++;
    f.pump_tid = std::this_thread::get_id();
    moq_session_t *session = moq_mvfst_managed_session(m);
    if (session) {
        {
            std::lock_guard<std::mutex> lock(f.tracker.mu);
            f.tracker.session = session;
        }
        moq_pub_cfg_t pc;
        moq_pub_cfg_init_sized(&pc, sizeof(pc));
        if (moq_pub_create(session, &f.alloc, &pc, &f.publisher) == MOQ_OK) {
            static const uint8_t ns_data[] = "issue23";
            static const uint8_t name[] = "video";
            moq_bytes_t part{ns_data, sizeof(ns_data) - 1};
            moq_pub_track_cfg_t tc;
            moq_pub_track_cfg_init_sized(&tc, sizeof(tc));
            tc.track_namespace = {&part, 1};
            tc.track_name = {name, sizeof(name) - 1};
            tc.advertise_namespace = false;
            if (moq_pub_add_track(f.publisher, &tc, now, &f.track) == MOQ_OK) {
                /* session_track_hist.c's private key encoding: tuple count,
                 * big-endian u16 component lengths, then component bytes.
                 * No publication/write: history is reserved but unobserved,
                 * so publisher destruction must release this exact key. */
                static const unsigned char key[] = {
                    1, 0, 7, 'i', 's', 's', 'u', 'e', '2', '3',
                    0, 5, 'v', 'i', 'd', 'e', 'o'};
                f.history_key = f.tracker.find_key(key, sizeof(key));
                f.setup_ok = f.track && f.history_key && f.tracker.contains(session);
            }
        }
    }
    m->set_fatal(0x1);
    return 1;  /* The actual managed pump takes its natural teardown path. */
}

int run(moq_version_t version) {
    Fixture f;
    int failures = 0;
    auto check = [&failures](bool ok, const char *what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); failures++; }
    };
    moq_mvfst_managed_cfg_t cfg;
    moq_mvfst_managed_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = &f.alloc;
    cfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    cfg.host = nullptr;
    cfg.port = 0;
    cfg.on_lane_pump = fatal_pump;
    cfg.user_ctx = &f;
    const char *alpn = version == MOQ_VERSION_DRAFT_16 ? "moqt-16" : "moqt-18";
    cfg.alpn_list = &alpn;
    cfg.alpn_count = 1;
    moq_mvfst_managed_t *m = nullptr;
    moq_result_t rc = moq_mvfst_managed_create(&cfg, &m);
    check(rc == MOQ_OK && m, "managed creation");
    if (m) {
        bool exited;
        {
            std::unique_lock<std::mutex> lock(m->wait_mu);
            exited = m->wait_cv.wait_for(lock, std::chrono::seconds(5),
                [m] { return m->thread_exited.load(); });
        }
        check(exited, "natural exit before stop (timeout is failure)");
        check(moq_mvfst_managed_is_fatal(m), "real fatal state");
        check(m->fatal_code_val.load() == 0x1, "fatal code");
        check(!m->joined.load(), "natural exit was not an app stop/join");
        check(moq_mvfst_managed_stop(m) == MOQ_OK, "join after natural exit");
        check(!m->in_lane_pump, "no callback remains in flight after join");
        check(f.pump_calls == 1 && f.setup_ok, "real publisher/track/history in pump");
        check(f.pump_tid != std::this_thread::get_id(), "distinct cleanup thread");
        check(f.tracker.contains(f.tracker.session), "session live before publisher cleanup");
        check(f.tracker.session_frees == 0, "session not freed by fatal teardown");
    }
    // Also clean partial callback setup on creation/fatal-path failures.
    if (f.publisher) {
        moq_pub_destroy(f.publisher);
        f.publisher = nullptr;
    }
    if (m) {
        check(!f.tracker.contains(f.history_key), "reserved history key released by publisher");
        check(f.tracker.contains(f.tracker.session), "session live after publisher cleanup");
        check(f.tracker.session_frees == 0, "publisher did not destroy session");
        moq_mvfst_managed_destroy(m);
        check(f.tracker.session_frees == 1, "handle destroys session exactly once");
    }
    check(f.tracker.live_blocks == 0 && f.tracker.live_bytes == 0,
          "tracking allocator balanced");
    check(f.tracker.errors == 0, "allocator ownership and size contracts");
    if (!failures)
        std::printf("PASS: mvfst fatal history draft-%d\n",
                    version == MOQ_VERSION_DRAFT_16 ? 16 : 18);
    return failures;
}
}  // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "16") == 0)
        return run(MOQ_VERSION_DRAFT_16) ? 1 : 0;
    if (argc == 2 && std::strcmp(argv[1], "18") == 0)
        return run(MOQ_VERSION_DRAFT_18) ? 1 : 0;
    std::fprintf(stderr, "usage: %s 16|18\n", argv[0]);
    return 2;
}
