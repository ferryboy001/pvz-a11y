// SPDX-License-Identifier: LGPL-3.0-or-later
#include "Speech.hpp"
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

struct PrismBackend { size_t id; bool initialized = false; float volume = 0, rate = 0; };
struct PrismContext {};

struct FakePrism {
    PrismContext context;
    std::atomic<bool> available[3]{{true}, {false}, {true}}; // ZDSR, NVDA, SAPI
    std::atomic<bool> failInitialize{false}, failSpeak{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool blockInitialize = false, initializing = false, release = false;
    int live = 0, initialized = 0, freedInitialized = 0, stops = 0;
    std::thread::id owner;
    struct Utterance { size_t id; std::string text; float volume, rate; bool interrupt; };
    std::vector<Utterance> utterances;
    static inline FakePrism* self;
    static constexpr const char* names[] = {"ZDSR", "NVDA", "SAPI"};
    void CheckThread() {
        if (owner != std::this_thread::get_id()) throw std::runtime_error("backend used outside its owning speech thread");
    }
    template<typename F> void Wait(F predicate) {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), predicate)) throw std::runtime_error("speech worker timed out");
    }
};

struct SpeechTestAccess {
    static void Start(Speech& speech, FakePrism& fake) {
        FakePrism::self = &fake;
        auto& api = speech.prism;
        api.count = [](PrismContext*) -> size_t { return 3; };
        api.id_at = [](PrismContext*, size_t i) -> PrismBackendId { return i; };
        api.create = [](PrismContext*, PrismBackendId id) {
            auto& f = *FakePrism::self; f.CheckThread(); ++f.live;
            return new PrismBackend{static_cast<size_t>(id)};
        };
        api.name = [](PrismBackend* b) { FakePrism::self->CheckThread(); return FakePrism::names[b->id]; };
        api.features = [](PrismBackend* b) -> uint64_t {
            auto& f = *FakePrism::self; f.CheckThread();
            return PRISM_BACKEND_SUPPORTS_SPEAK | (f.available[b->id] ? PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME : 0);
        };
        api.initialize = [](PrismBackend* b) {
            auto& f = *FakePrism::self; f.CheckThread();
            if (b->id == 1) {
                std::unique_lock lock(f.mutex);
                if (f.blockInitialize) {
                    f.initializing = true; f.changed.notify_all();
                    f.changed.wait(lock, [&] { return f.release; });
                }
                if (f.failInitialize) return PRISM_ERROR_BACKEND_NOT_AVAILABLE;
            }
            b->initialized = true; ++f.initialized;
            return PRISM_ERROR_ALREADY_INITIALIZED; // 非致命，须接受
        };
        api.speak = [](PrismBackend* b, const char* text, bool interrupt) {
            auto& f = *FakePrism::self; f.CheckThread();
            if (!f.available[b->id] || f.failSpeak.exchange(false)) return PRISM_ERROR_SPEAK_FAILURE;
            std::lock_guard lock(f.mutex);
            f.utterances.push_back({b->id, text, b->volume, b->rate, interrupt}); f.changed.notify_all();
            return PRISM_OK;
        };
        api.stop = [](PrismBackend*) { auto& f = *FakePrism::self; f.CheckThread(); ++f.stops; return PRISM_OK; };
        api.backend_free = [](PrismBackend* b) {
            auto& f = *FakePrism::self; f.CheckThread(); --f.live;
            if (b->initialized) ++f.freedInitialized;
            delete b;
        };
        api.set_volume = [](PrismBackend* b, float value) { FakePrism::self->CheckThread(); b->volume = value; return PRISM_OK; };
        api.set_rate = [](PrismBackend* b, float value) { FakePrism::self->CheckThread(); b->rate = value; return PRISM_OK; };
        api.shutdown = [](PrismContext*) { FakePrism::self->CheckThread(); };
        speech.prismContext = &fake.context;
        std::promise<void> ready;
        auto result = ready.get_future();
        speech.worker = std::thread([&speech, &fake, ready = std::move(ready)]() mutable {
            fake.owner = std::this_thread::get_id();
            speech.RefreshPrism(); ready.set_value(); speech.Run(); speech.CloseInternal();
        });
        result.get();
    }
};

int main() {
    FakePrism fake;
    Speech speech;
    try {
        auto require = [](bool condition) { if (!condition) throw std::runtime_error("speech regression failed"); };
        SpeechTestAccess::Start(speech, fake);
        speech.Settings(65, 4);
        auto say = [&](size_t id, const char* text, bool interrupt = false) {
            size_t count;
            { std::lock_guard lock(fake.mutex); count = fake.utterances.size(); }
            speech.Say(text, interrupt);
            fake.Wait([&] { return fake.utterances.size() > count; });
            const auto& u = fake.utterances.back();
            require(u.id == id && u.text == text && u.interrupt == interrupt);
            require(std::abs(u.volume - .65f) < .001f && std::abs(u.rate - .7f) < .001f);
        };
        auto waitBackend = [&](const char* name) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (speech.BackendName() != name && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(speech.BackendName() == name);
        };
        say(0, "initial ZDSR");
        fake.available[0] = false; fake.available[1] = true;
        waitBackend("prism/NVDA");
        say(1, "NVDA after ZDSR exits");
        // 无播报输入时也在后台复查；重复文本不能因为旧后端去重而丢失。
        fake.available[1] = false;
        waitBackend("prism/SAPI");
        say(2, "NVDA after ZDSR exits");
        fake.available[1] = true; fake.failInitialize = true;
        say(2, "NVDA API not ready");
        fake.failInitialize = false;
        waitBackend("prism/NVDA");
        say(1, "NVDA retries successfully");
        fake.failSpeak = true;
        say(1, "retry current text on speak failure", true);
        // 阻塞后端初始化时，游戏线程依然可以入队设置、播报和停止。
        fake.available[1] = false; waitBackend("prism/SAPI");
        say(2, "fallback before slow switch");
        { std::lock_guard lock(fake.mutex); fake.blockInitialize = true; }
        fake.available[1] = true;
        fake.Wait([&] { return fake.initializing; });
        speech.Say("obsolete", false); speech.StopSpeaking(); speech.Settings(65, 4);
        speech.Say("obsolete again", false); speech.Say("latest", true); speech.Pump();
        { std::lock_guard lock(fake.mutex); fake.release = true; fake.changed.notify_all(); }
        fake.Wait([&] { return !fake.utterances.empty() && fake.utterances.back().text == "latest"; });
        for (const auto& u : fake.utterances) require(u.text != "obsolete" && u.text != "obsolete again");
        speech.SetForceSapi(true); waitBackend("sapi");
        fake.available[0] = true;
        // 强制 SAPI 时检测继续进行，解除后能恢复到当前最高优先级读屏。
        speech.SetForceSapi(false); waitBackend("prism/ZDSR");
        say(0, "restore after forced SAPI");
        speech.Close(); speech.Close();
        require(fake.live == 0 && fake.initialized == fake.freedInitialized && fake.stops > 0);
        std::cout << "Speech backend switching, retries, settings, queue and ownership passed\n";
    } catch (const std::exception& error) {
        { std::lock_guard lock(fake.mutex); fake.release = true; fake.changed.notify_all(); }
        speech.Close(); std::cerr << error.what() << '\n'; return 1;
    }
}
