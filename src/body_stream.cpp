#include "body_stream.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

namespace grab {

namespace {

class BodyStream final : public IStream {
public:
    BodyStream(std::shared_ptr<const std::string> body, std::uint64_t position)
        : body_(std::move(body)), position_(position) {}
    BodyStream(const BodyStream&) = delete;
    BodyStream& operator=(const BodyStream&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISequentialStream) || riid == __uuidof(IStream)) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE Read(void* to, ULONG wanted, ULONG* read) override {
        if (to == nullptr) return STG_E_INVALIDPOINTER;
        std::scoped_lock lock(mutex_);
        const std::uint64_t at = std::min(position_, size());
        const auto n = static_cast<ULONG>(std::min<std::uint64_t>(wanted, size() - at));
        if (n > 0) std::memcpy(to, body_->data() + at, n);
        position_ = at + n;
        if (read != nullptr) *read = n;
        return n < wanted ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* /*from*/, ULONG /*n*/, ULONG* written) override {
        if (written != nullptr) *written = 0;
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* now) override {
        std::scoped_lock lock(mutex_);
        std::uint64_t base = 0;
        switch (origin) {
        case STREAM_SEEK_SET: break;
        case STREAM_SEEK_CUR: base = position_; break;
        case STREAM_SEEK_END: base = size(); break;
        default: return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t by = move.QuadPart;
        std::uint64_t to = 0;
        if (by < 0) {
            const std::uint64_t back = 0 - static_cast<std::uint64_t>(by);
            if (back > base) return STG_E_INVALIDFUNCTION; // before the start
            to = base - back;
        } else {
            const auto ahead = static_cast<std::uint64_t>(by);
            if (ahead > std::numeric_limits<std::uint64_t>::max() - base) return STG_E_INVALIDFUNCTION;
            to = base + ahead; // past the end is allowed; a Read there gives nothing
        }
        position_ = to;
        if (now != nullptr) now->QuadPart = to;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER /*size*/) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* to, ULARGE_INTEGER count, ULARGE_INTEGER* read,
                                     ULARGE_INTEGER* written) override {
        if (to == nullptr) return STG_E_INVALIDPOINTER;
        std::uint64_t at = 0;
        std::uint64_t n = 0;
        {
            std::scoped_lock lock(mutex_);
            at = std::min(position_, size());
            n = std::min<std::uint64_t>(count.QuadPart, size() - at);
            position_ = at + n;
        }
        std::uint64_t done = 0;
        HRESULT hr = S_OK;
        while (done < n && SUCCEEDED(hr)) {
            const auto part = static_cast<ULONG>(std::min<std::uint64_t>(n - done, std::uint64_t{1} << 30));
            ULONG wrote = 0;
            hr = to->Write(body_->data() + at + done, part, &wrote);
            done += wrote;
            if (wrote < part) break;
        }
        if (read != nullptr) read->QuadPart = n;
        if (written != nullptr) written->QuadPart = done;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD /*flags*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER /*offset*/, ULARGE_INTEGER /*count*/, DWORD /*type*/) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER /*offset*/, ULARGE_INTEGER /*count*/, DWORD /*type*/) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD /*flags*/) override {
        if (stat == nullptr) return STG_E_INVALIDPOINTER;
        *stat = STATSTG{};
        stat->pwcsName = nullptr; // no name, whatever the flags ask: there is none to give
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = size();
        stat->grfMode = STGM_READ;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if (out == nullptr) return STG_E_INVALIDPOINTER;
        std::scoped_lock lock(mutex_);
        *out = new BodyStream(body_, position_);
        return S_OK;
    }

private:
    ~BodyStream() = default; // by Release

    [[nodiscard]] std::uint64_t size() const { return body_ ? body_->size() : 0; }

    std::atomic<ULONG> refs_{1};
    std::shared_ptr<const std::string> body_; // never changes: read without the lock
    std::mutex mutex_;                        // guards position_
    std::uint64_t position_ = 0;
};

} // namespace

IStream* new_body_stream(std::shared_ptr<const std::string> body) { return new BodyStream(std::move(body), 0); }

} // namespace grab
